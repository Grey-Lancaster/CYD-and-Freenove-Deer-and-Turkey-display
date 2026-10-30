// Deer Hunt - an animated autumn woods for the Freenove FNK0104S (480x320, PSRAM) and the CYD 2.8" (320x240, no PSRAM).
// Deer and turkeys wander the forest floor; touch scatters corn and the nearest animals walk over to eat it.
// Same structure as the Fish Tank firmware: the world lives in a 480x320 virtual space and a Canvas maps it to the panel.
// The Freenove renders a full frame in PSRAM, the CYD renders and pushes in horizontal strips. WiFi is set up on the
// screen (TouchWifiProvisioner, BOOT skips it); online there is a web page, ArduinoOTA and ElegantOTA at /update.
// BOOT then shows the connection info (press again to close). A clock and date sit top-right (NTP, time zone, 12/24 h)
// and there is an alarm clock like the fish tank's: a silent sunrise, then a chime over a big clock with Snooze / Stop.
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ElegantOTA.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "board.h"
#include "canvas.h"
#include "hal.h"
#include "wifi_setup.h"
#include "audio.h"

#define FW_VERSION "1.1.1"

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite frame = TFT_eSprite(&tft);    // Freenove: the whole frame
TFT_eSprite bg = TFT_eSprite(&tft);       // Freenove: static background
TFT_eSprite strip = TFT_eSprite(&tft);    // CYD: one horizontal strip
Canvas cv;
Preferences prefs;
WebServer server(80);

// Settings / state the HALs expect (no sound in the woods yet)
volatile int g_vol = 60;
volatile bool g_snd = true;
volatile bool g_sBub = true, g_sDiv = false, g_sFeed = false;
int g_rot = DEFAULT_ROT;
volatile bool g_asleep = false;
volatile bool g_isNight = false;
bool g_invert = false;
bool g_swaprb = DEFAULT_SWAPRB;

String g_host = DEFAULT_HOST;      // mDNS / OTA / DHCP name
static bool g_online = false;      // WiFi connected (web page, OTA available)
static uint32_t restartAt = 0, infoUntil = 0;
static float g_fps = 0;
static float curT = 0;

// Clock, date and alarm settings (persisted in NVS, editable from the web page)
bool g_h24 = false;                // 24-hour clock
bool g_hunt = true;               // the hunter visits now and then (web page switch)
String g_tz = "EST5EDT,M3.2.0,M11.1.0";   // POSIX TZ string
int g_datefmt = 0;                 // 0 Tue 10/20/26, 1 Tue 20/10/26, 2 Tue 2026-10-20, 3 Tue, Oct 20, 4 hidden
bool g_alOn = false;
int g_alMin = 7 * 60;              // minutes after midnight
int g_alDays = 0x7F;               // bit 0 = Sunday .. bit 6 = Saturday
int g_alSnz = 9;                   // snooze, minutes
int g_alVol = 70;                  // alarm loudness 0..100, separate from the woods volume
enum AlState { AL_IDLE, AL_RAMP, AL_RING, AL_SNOOZE };
AlState alState = AL_IDLE;
uint32_t alT0 = 0, alRampLen = 60000, alSnoozeUntil = 0;
int alFiredDay = -1;               // day of the year it last started, so it rings once a day
bool alMissed = false, alPanel = false;
uint32_t alPanelAt = 0;
static inline bool alActive() { return alState == AL_RAMP || alState == AL_RING; }
static inline uint16_t bswap16(uint16_t v) { return (v >> 8) | (v << 8); }

static void saveSettings() {
  prefs.putBool("h24", g_h24); prefs.putString("tz", g_tz); prefs.putInt("dfmt", g_datefmt);
  prefs.putBool("invert", g_invert); prefs.putBool("swaprb", g_swaprb);
  prefs.putBool("hunt", g_hunt);
  prefs.putBool("alon", g_alOn); prefs.putInt("almin", g_alMin); prefs.putInt("aldays", g_alDays); prefs.putInt("alsnz", g_alSnz); prefs.putInt("alvol", g_alVol);
}

static inline uint16_t RGB(int r, int g, int b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline int lerpi(int a, int b, float t) { return a + (int)((b - a) * t); }
static inline uint16_t mix(int r0, int g0, int b0, int r1, int g1, int b1, float t) {
  return RGB(lerpi(r0, r1, t), lerpi(g0, g1, t), lerpi(b0, b1, t));
}

#define HORIZON 180      // where the sky meets the forest floor
#define GROUND_MIN 224   // animals walk between these rows (nearer = lower on the screen = bigger)
#define GROUND_MAX 306

static float scaleAt(float y) { return 0.62f + 0.55f * (y - GROUND_MIN) / (GROUND_MAX - GROUND_MIN); }

// ---------------------------------------------------------------- animals
enum Kind { BUCK, DOE, FAWN, TOM, HEN };
enum Mode { WALK, GRAZE, LOOK, EAT, FLEE, HIDDEN, DOWN };   // FLEE / HIDDEN / DOWN: the hunter's doing

struct Critter {
  Kind k;
  float x, y, ty;       // ty: row it is drifting toward
  int dir;              // +1 facing right, -1 facing left
  float spd;            // px/s at scale 1
  float ph;             // gait phase
  float head;           // 0 head up .. 1 head down (smoothed)
  float fan;            // tom's tail fan 0 closed .. 1 open (smoothed)
  Mode m;
  float timer;
  int corn;             // index of the corn piece it is heading for / eating, or -1
};

Critter crit[] = {
  {BUCK, 120, 262, 262, 1,  22, 0.0f, 0, 0, GRAZE, 2, -1},
  {DOE,  330, 246, 246, -1, 20, 1.0f, 0, 0, WALK,  4, -1},
  {FAWN, 300, 252, 252, -1, 20, 2.0f, 0, 0, WALK,  4, -1},
  {DOE,  420, 232, 232, -1, 17, 3.0f, 0, 0, LOOK,  3, -1},
  {TOM,  210, 290, 290, 1,  19, 0.5f, 0, 0, WALK,  5, -1},
  {HEN,  400, 280, 280, -1, 16, 1.5f, 0, 0, GRAZE, 3, -1},
  {HEN,   70, 238, 238, 1,  15, 2.5f, 0, 0, WALK,  6, -1},
};
const int NCRIT = sizeof(crit) / sizeof(crit[0]);
#define FAWN_MOTHER 1   // index of the doe the fawn trails

struct Corn { float x, y, z, life; bool on; int by; };   // z: height in the pile
#define NCORN 24
Corn corn[NCORN];

struct Leaf { float x, y, vy, vx, ph; uint16_t c; };
Leaf leaves[18];

// ---- the hunter: now and then he pops up from behind a rock, shoots, and either misses (everyone runs off and hides) or
// gets the big buck or the tom and drags it away. Whatever he gets walks back in a while later. Cartoon only.
enum HunterState { H_OFF, H_RISE, H_AIM, H_SHOT, H_LOWER, H_RUN, H_DRAG };
struct Hunter {
  HunterState st;
  float t;              // seconds in this state
  float since;          // seconds since the shot (smoke, BANG)
  int side;             // 0 = rock bottom-left, 1 = rock bottom-right
  float x, y;           // feet
  float sx, sy;         // where the muzzle was at the shot
  int dir;              // facing
  int tgt;              // critter he shot, or -1
  int dragDir;          // which edge he drags it to
  bool hit;
  int force;            // -2 random, -1 miss, else the critter index that gets shot (for testing)
  uint32_t next;        // millis() of the next automatic visit
  bool want;            // a visit someone asked for (web page, rock tap), waiting until the animals are all back
  int wantSide, wantForce;
};
Hunter hunter = {H_OFF, 0, 99, 0, 46, 314, 0, 0, 1, -1, 1, false, -2, 90000, false, -1, -2};
static inline bool hunterAlert() { return hunter.st == H_AIM || (hunter.st == H_SHOT && hunter.t < 0.12f); }

// A tap drops a pile of kernels, heaped toward the middle.
static void spawnCorn(float wx, float wy) {
  const float cx = constrain(wx, 24, VW - 24), cy = constrain(wy, GROUND_MIN + 6, GROUND_MAX - 2);
  for (int n = 0; n < 10; n++) {
    for (auto &c : corn) {
      if (c.on) continue;
      float dx = (random(-13, 14) + random(-13, 14)) * 0.5f;
      c.x = cx + dx;
      c.y = cy + random(-3, 4);
      c.z = random(0, 100) / 100.0f * 4.5f * (1.0f - fabsf(dx) / 13.0f);
      c.life = 45; c.on = true; c.by = -1;
      break;
    }
  }
}

static void pickNext(Critter &c) {
  const bool deer = c.k <= FAWN;
  int r = random(100);
  if (r < 50) {
    c.m = WALK;
    c.timer = 3.0f + random(50) / 10.0f;
    c.dir = (c.x < 90) ? 1 : (c.x > VW - 90) ? -1 : (random(2) ? 1 : -1);
    c.ty = random(GROUND_MIN, GROUND_MAX);
  } else if (r < 80) {
    c.m = GRAZE; c.timer = 2.0f + random(30) / 10.0f;
  } else {
    c.m = LOOK; c.timer = 1.5f + random(20) / 10.0f;
  }
  (void)deer;
}

static void updateCritter(int i, float dt) {
  Critter &c = crit[i];
  if (c.m == DOWN) return;             // lying where it fell; the hunter moves it
  if (c.m == HIDDEN) {                 // out of sight until the timer runs out, then it walks back in from the edge it left by
    c.timer -= dt;
    if (c.timer <= 0) { c.x = c.dir > 0 ? VW + 50 : -50; c.dir = -c.dir; c.y = random(GROUND_MIN, GROUND_MAX); c.ty = c.y; c.m = WALK; c.timer = 6; c.head = 0; c.fan = 0; }
    return;
  }
  if (c.m == FLEE) {                   // bolting away from the hunter
    if (c.timer > 0) { c.timer -= dt; c.head += (0.0f - c.head) * min(1.0f, dt * 6); return; }   // a moment's reaction first
    const float fv = c.spd * 6.0f * scaleAt(c.y);
    c.x += c.dir * fv * dt; c.ph += fv * dt * 0.16f; c.head = 0;
    if (c.x < -70 || c.x > VW + 70) { c.m = HIDDEN; c.timer = 8 + random(8); }
    return;
  }
  if (alActive() || hunterAlert()) {   // sunrise / alarm / the hunter takes aim: everyone stops and looks up, the tom fans his tail
    if (c.corn >= 0) { corn[c.corn].by = -1; c.corn = -1; }
    c.m = LOOK; c.timer = 2.0f;
    c.head += (0.0f - c.head) * min(1.0f, dt * 4);
    c.fan += (((c.k == TOM) ? 1.0f : 0.0f) - c.fan) * min(1.0f, dt * 2.5f);
    return;
  }
  const float sc = scaleAt(c.y);
  c.timer -= dt;

  // heading for corn?
  if (c.corn >= 0 && !corn[c.corn].on) { c.corn = -1; if (c.m == WALK || c.m == EAT) pickNext(c); }
  if (c.corn < 0 && c.k != FAWN) {
    int best = -1; float bd = 230;
    for (int n = 0; n < NCORN; n++) {
      if (!corn[n].on || corn[n].by >= 0) continue;
      float d = fabsf(corn[n].x - c.x) + fabsf(corn[n].y - c.y);
      if (d < bd) { bd = d; best = n; }
    }
    if (best >= 0) { c.corn = best; corn[best].by = i; c.m = WALK; }
  }

  float v = 0;
  if (c.corn >= 0 && c.m != EAT) {
    Corn &f = corn[c.corn];
    float dx = f.x - c.x, dy = f.y - c.y;
    // stop with the head (not the body) over the kernel, so a crowd rings the pile instead of stacking on it
    const float reach = (c.k <= FAWN ? 38.0f : 22.0f) * (c.k == DOE ? 0.92f : c.k == HEN ? 0.85f : c.k == FAWN ? 0.62f : 1.0f);
    if (fabsf(dx) > reach + 3) { c.dir = dx >= 0 ? 1 : -1; v = c.spd * 1.5f * sc; c.x += c.dir * v * dt; }
    c.y += constrain(dy, -1.0f, 1.0f) * 20 * dt;
    if (fabsf(dx) <= reach + 3 && fabsf(dy) < 6) { c.m = EAT; c.timer = 2.5f + random(20) / 10.0f; v = 0; }
  } else if (c.m == EAT) {
    if (c.timer <= 0) {
      if (c.corn >= 0) corn[c.corn].on = false;
      c.corn = -1; c.m = GRAZE; c.timer = 1.0f;
    }
  } else if (c.k == FAWN) {
    // trail the doe
    Critter &d = crit[FAWN_MOTHER];
    float tx = d.x - d.dir * 34, dx = tx - c.x;
    if (d.m == HIDDEN) { c.m = GRAZE; }
    else if (fabsf(dx) > 12) { c.m = WALK; c.dir = dx >= 0 ? 1 : -1; v = c.spd * 1.2f * sc; c.x += c.dir * v * dt; }
    else { c.m = (d.m == WALK) ? GRAZE : d.m; c.dir = d.dir; }
    c.y += constrain(d.y + 6 - c.y, -1.0f, 1.0f) * 12 * dt;
  } else if (c.m == WALK) {
    v = c.spd * sc;
    c.x += c.dir * v * dt;
    c.y += (c.ty - c.y) * 0.3f * dt;
    if (c.x < -45) c.dir = 1;
    if (c.x > VW + 45) c.dir = -1;
    if (c.timer <= 0) pickNext(c);
  } else if (c.timer <= 0) pickNext(c);

  c.y = constrain(c.y, GROUND_MIN, GROUND_MAX);
  c.ph += v * dt * 0.16f;
  const bool down = c.m == GRAZE || c.m == EAT;
  c.head += ((down ? 1.0f : 0.0f) - c.head) * min(1.0f, dt * 4);
  c.fan += (((c.m == LOOK && c.k == TOM) ? 1.0f : 0.0f) - c.fan) * min(1.0f, dt * 2.5f);
}

// ---------------------------------------------------------------- the hunter (behaviour)
static const int HUNT_X[2] = {46, 428};      // where he stands behind each rock
static const int HUNT_DIR[2] = {1, -1};      // and which way he faces

static void startHunter(int side, int force = -2) {
  if (hunter.st != H_OFF) return;
  hunter.side = side < 0 ? random(2) : side;
  hunter.x = HUNT_X[hunter.side]; hunter.y = 314; hunter.dir = HUNT_DIR[hunter.side];
  hunter.tgt = -1; hunter.hit = false; hunter.force = force; hunter.t = 0; hunter.since = 99;
  hunter.st = H_RISE;
}

static void hunterDone(uint32_t now) {
  hunter.st = H_OFF; hunter.tgt = -1;
  hunter.next = now + (180 + random(240)) * 1000UL;      // the next visit in 3 to 7 minutes
}

// He only comes out when every animal is on the screen: none hiding, running, or still on its way back in.
static bool allBack() {
  for (int i = 0; i < NCRIT; i++) {
    const Critter &c = crit[i];
    if (c.m == HIDDEN || c.m == FLEE || c.m == DOWN || c.x < 8 || c.x > VW - 8) return false;
  }
  return true;
}

// Ask for a visit (web page, rock tap): it waits behind the rock until the animals are all back.
static void callHunter(int side, int force = -2) {
  if (hunter.st != H_OFF) return;
  hunter.want = true; hunter.wantSide = side; hunter.wantForce = force;
}

static bool onScreen(const Critter &c) { return c.m != HIDDEN && c.m != DOWN && c.x > 12 && c.x < VW - 12; }

// The shot: 40% a miss, 30% the big buck, 30% the tom. Everyone else runs for it.
static void hunterFires() {
  audioShot();
  const float hs = 1.3f * scaleAt(314);
  hunter.sx = hunter.x + hunter.dir * 33 * hs; hunter.sy = hunter.y - 32.5f * hs; hunter.since = 0;
  const int roll = random(100);
  int want = hunter.force != -2 ? hunter.force : roll < 40 ? -1 : roll < 70 ? 0 : 4;      // crit[0] is the buck, crit[4] the tom
  if (want >= 0 && !onScreen(crit[want])) want = (want == 0 && onScreen(crit[4])) ? 4 : (want == 4 && onScreen(crit[0])) ? 0 : -1;
  hunter.tgt = want; hunter.hit = want >= 0;
  for (int i = 0; i < NCRIT; i++) {
    Critter &c = crit[i];
    if (c.corn >= 0) { corn[c.corn].by = -1; c.corn = -1; }
    if (c.m == HIDDEN) continue;
    if (i == want) { c.m = DOWN; c.head = 1; continue; }
    c.m = FLEE; c.dir = c.x >= hunter.x ? 1 : -1; c.timer = random(0, 45) / 100.0f;     // a moment's reaction, then run
  }
}

static void updateHunter(float dt) {
  const uint32_t now = millis();
  if (hunter.st == H_OFF) {
    const bool due = g_hunt && now >= hunter.next;
    if ((hunter.want || due) && !alActive() && allBack()) {      // wait until every animal is back on the screen
      startHunter(hunter.want ? hunter.wantSide : -1, hunter.want ? hunter.wantForce : -2);
      hunter.want = false;
    }
    return;
  }
  if (alActive()) {                          // the alarm wins: the hunter leaves and anything he got walks back soon
    if (hunter.tgt >= 0 && crit[hunter.tgt].m == DOWN) {
      Critter &a = crit[hunter.tgt];
      a.m = HIDDEN; a.timer = 3; a.dir = a.x < VW / 2 ? -1 : 1;
    }
    hunterDone(now);
    return;
  }
  hunter.t += dt;
  hunter.since += dt;
  switch (hunter.st) {
    case H_RISE:  if (hunter.t >= 1.3f) { hunter.st = H_AIM; hunter.t = 0; } break;
    case H_AIM:   if (hunter.t >= 1.6f) { hunter.st = H_SHOT; hunter.t = 0; hunterFires(); } break;
    case H_SHOT:
      if (hunter.t >= 1.1f) {
        hunter.t = 0;
        if (hunter.hit) { hunter.st = H_RUN; hunter.dragDir = crit[hunter.tgt].x < VW / 2 ? -1 : 1; }
        else hunter.st = H_LOWER;
      }
      break;
    case H_LOWER: if (hunter.t >= 1.0f) hunterDone(now); break;
    case H_RUN: {                            // run out to the animal and take hold of it from the side he will pull toward
      Critter &a = crit[hunter.tgt];
      const float gx = a.x + hunter.dragDir * (a.k <= FAWN ? 36 : 22) * scaleAt(a.y);
      const float dx = gx - hunter.x, dy = (a.y + 2) - hunter.y;
      hunter.dir = dx >= 0 ? 1 : -1;
      if (fabsf(dx) > 3) hunter.x += hunter.dir * min(95.0f * dt, fabsf(dx));
      hunter.y += constrain(dy, -1.0f, 1.0f) * 70 * dt;
      if (fabsf(dx) <= 3 && fabsf(dy) < 3) { hunter.st = H_DRAG; hunter.t = 0; hunter.dir = hunter.dragDir; }
      break;
    }
    case H_DRAG: {                           // drag it off the nearest edge
      Critter &a = crit[hunter.tgt];
      hunter.x += hunter.dragDir * 34 * dt;
      a.x = hunter.x - hunter.dragDir * (a.k <= FAWN ? 36 : 22) * scaleAt(a.y);
      a.y = hunter.y - 2;
      if (hunter.x < -60 || hunter.x > VW + 60) {      // out of sight: it wanders back in a while
        a.m = HIDDEN; a.timer = 20 + random(15); a.dir = hunter.dragDir;
        hunterDone(now);
      }
      break;
    }
    default: break;
  }
}

static void updateWorld(float dt, float t) {
  updateHunter(dt);
  for (int i = 0; i < NCRIT; i++) updateCritter(i, dt);
  for (auto &c : corn) {
    if (!c.on) continue;
    c.life -= dt;
    if (c.life <= 0) c.on = false;
  }
  for (auto &l : leaves) {
    l.y += l.vy * dt;
    l.x += (l.vx + 9 * sinf(t * 1.3f + l.ph)) * dt;
    if (l.y > VH + 6 || l.x < -8 || l.x > VW + 8) { l.y = -6; l.x = random(VW); }
  }
}

// ---------------------------------------------------------------- drawing
struct Pose { float x, y, s; int d; };
static inline float PX(const Pose &p, float a) { return p.x + p.d * a * p.s; }
static inline float PY(const Pose &p, float a) { return p.y + a * p.s; }

static void leg(const Pose &p, float hx, float hy, float sw, float lift, float w, uint16_t col, uint16_t hoof) {
  float kx = hx + sw * 0.35f, ky = hy + 13;
  float fx = hx + sw, fy = -lift;
  cv.drawWideLine(PX(p, hx), PY(p, hy), PX(p, kx), PY(p, ky), w * p.s, col, col);
  cv.drawWideLine(PX(p, kx), PY(p, ky), PX(p, fx), PY(p, fy), w * p.s * 0.8f, col, col);
  cv.fillCircle(PX(p, fx), PY(p, fy), 1.5f * p.s, hoof);
}

static void drawDeer(const Critter &c, float t) {
  const bool buck = c.k == BUCK, fawn = c.k == FAWN;
  Pose p = {c.x, c.y, scaleAt(c.y) * (fawn ? 0.62f : buck ? 1.0f : 0.92f), c.dir};
  uint16_t body = buck ? RGB(138, 90, 50) : fawn ? RGB(172, 118, 70) : RGB(160, 112, 68);
  uint16_t dark = buck ? RGB(100, 64, 36) : fawn ? RGB(135, 90, 52) : RGB(122, 84, 50);
  uint16_t belly = RGB(222, 200, 160), hoof = RGB(40, 30, 24);
  const bool moving = c.m == WALK || (c.corn >= 0 && c.m != EAT);

  // legs: far pair, then body, then near pair
  struct L { float hx, off; bool far; } legs[4] = {{-12, 3.14f, true}, {10, 0, true}, {-17, 0, false}, {15, 3.14f, false}};
  auto drawLeg = [&](const L &l) {
    float sw = moving ? sinf(c.ph + l.off) * 8 : 0;
    float lift = moving ? max(0.0f, cosf(c.ph + l.off)) * 4 : 0;
    leg(p, l.hx, -26, sw, lift, 3.2f, l.far ? dark : body, hoof);
  };
  for (auto &l : legs) if (l.far) drawLeg(l);

  cv.fillEllipse(PX(p, 0), PY(p, -34), 25 * p.s, 11 * p.s, body);
  cv.fillEllipse(PX(p, 12), PY(p, -35), 14 * p.s, 11 * p.s, body);
  cv.fillEllipse(PX(p, -14), PY(p, -34), 13 * p.s, 11 * p.s, body);
  cv.fillEllipse(PX(p, 0), PY(p, -27), 21 * p.s, 4 * p.s, belly);
  // tail (flicks while looking)
  float tw = (c.m == LOOK) ? sinf(t * 14) * 3 : 0;
  cv.fillTriangle(PX(p, -24), PY(p, -43 + tw), PX(p, -33), PY(p, -39 + tw), PX(p, -25), PY(p, -31), body);
  cv.fillTriangle(PX(p, -26), PY(p, -38 + tw), PX(p, -32), PY(p, -37 + tw), PX(p, -26), PY(p, -32), RGB(240, 235, 225));
  if (fawn) {
    static const int8_t sp[8][2] = {{-14, -40}, {-6, -42}, {2, -41}, {10, -40}, {-10, -35}, {-2, -36}, {6, -36}, {14, -37}};
    for (auto &s : sp) cv.fillCircle(PX(p, s[0]), PY(p, s[1]), 1.3f * p.s, RGB(245, 235, 210));
  }
  for (auto &l : legs) if (!l.far) drawLeg(l);

  // neck and head
  float bob = moving ? sinf(c.ph * 2) * 1.2f : 0;
  float hx = 36 + (40 - 36) * c.head, hy = -62 + (-9 + 62) * c.head + bob;
  cv.drawWideLine(PX(p, 17), PY(p, -40), PX(p, hx - 4), PY(p, hy + 1), 9 * p.s, body, body);
  float ex = (c.m == LOOK) ? sinf(t * 11) * 2 : 0;                       // ear flick
  cv.fillTriangle(PX(p, hx - 4), PY(p, hy - 3), PX(p, hx - 11 + ex), PY(p, hy - 10), PX(p, hx - 1), PY(p, hy - 8), dark);
  cv.fillCircle(PX(p, hx), PY(p, hy), 5.5f * p.s, body);
  float mx = hx + 6 - 4 * c.head, my = hy + 1 + 4 * c.head;
  cv.fillEllipse(PX(p, mx), PY(p, my), 4.5f * p.s, 3.2f * p.s, RGB(196, 166, 126));
  cv.fillCircle(PX(p, mx + 3.5f), PY(p, my - 0.5f), 1.4f * p.s, RGB(25, 20, 18));
  cv.fillCircle(PX(p, hx + 1.5f), PY(p, hy - 2), 1.2f * p.s, RGB(15, 12, 10));
  if (buck) {
    uint16_t ant = RGB(214, 194, 150), ant2 = RGB(168, 148, 108);
    for (int pass = 0; pass < 2; pass++) {
      float o = pass ? 3.5f : 0;
      uint16_t a = pass ? ant2 : ant;
      float bx = hx - 2 + o, by = hy - 4;
      float b1x = hx - 6 + o, b1y = hy - 14, b2x = hx + 2 + o, b2y = hy - 25;
      float w = 1.7f * p.s;
      cv.drawWideLine(PX(p, bx), PY(p, by), PX(p, b1x), PY(p, b1y), w, a, a);
      cv.drawWideLine(PX(p, b1x), PY(p, b1y), PX(p, b2x), PY(p, b2y), w, a, a);
      cv.drawWideLine(PX(p, b1x), PY(p, b1y), PX(p, b1x - 8), PY(p, b1y - 4), w, a, a);
      cv.drawWideLine(PX(p, b2x), PY(p, b2y), PX(p, b2x + 1), PY(p, b2y - 7), w, a, a);
      cv.drawWideLine(PX(p, b2x), PY(p, b2y), PX(p, b2x + 7), PY(p, b2y - 3), w, a, a);
    }
  }
}

static void drawTurkey(const Critter &c, float t) {
  const bool tom = c.k == TOM;
  Pose p = {c.x, c.y, scaleAt(c.y) * (tom ? 1.0f : 0.85f), c.dir};
  uint16_t body = tom ? RGB(86, 56, 36) : RGB(124, 94, 64);
  uint16_t wing = tom ? RGB(62, 40, 28) : RGB(98, 72, 48);
  uint16_t legc = RGB(205, 150, 110);
  const bool moving = c.m == WALK || (c.corn >= 0 && c.m != EAT);

  // tail first, behind the body
  if (tom) {
    float open = 0.35f + 0.65f * c.fan;
    const int N = 9;
    float ang0 = -0.45f, ang1 = 1.55f, R = 30 * open + 8;
    float prevx = 0, prevy = 0;
    for (int i = 0; i <= N; i++) {
      float a = ang0 + (ang1 - ang0) * i / N;
      float ex = -12 - R * sinf(a), ey = -22 - R * cosf(a) * 0.9f;
      if (i > 0) {
        uint16_t col = (i & 1) ? RGB(158, 88, 40) : RGB(206, 168, 108);
        cv.fillTriangle(PX(p, -12), PY(p, -22), PX(p, prevx), PY(p, prevy), PX(p, ex), PY(p, ey), col);
      }
      prevx = ex; prevy = ey;
    }
    cv.drawLine(PX(p, -12), PY(p, -22), PX(p, prevx), PY(p, prevy), RGB(120, 70, 36));
  } else {
    cv.fillTriangle(PX(p, -11), PY(p, -22), PX(p, -25), PY(p, -26), PX(p, -22), PY(p, -11), wing);
  }

  // legs (far, near)
  for (int n = 0; n < 2; n++) {
    float off = n ? 3.14f : 0;
    float sw = moving ? sinf(c.ph * 1.2f + off) * 6 : 0;
    float lift = moving ? max(0.0f, cosf(c.ph * 1.2f + off)) * 3 : 0;
    uint16_t lc = n ? legc : RGB(165, 118, 84);
    float hx = n ? 3 : -2;
    cv.drawWideLine(PX(p, hx), PY(p, -9), PX(p, hx + sw), PY(p, -lift), 2.2f * p.s, lc, lc);
    cv.drawLine(PX(p, hx + sw), PY(p, -lift), PX(p, hx + sw + 4), PY(p, -lift), lc);
    cv.drawLine(PX(p, hx + sw), PY(p, -lift), PX(p, hx + sw - 3), PY(p, -lift + 1), lc);
  }

  cv.fillEllipse(PX(p, 0), PY(p, -18), 15 * p.s, 10 * p.s, body);
  cv.fillEllipse(PX(p, 7), PY(p, -17), 9 * p.s, 9 * p.s, body);
  cv.fillEllipse(PX(p, -3), PY(p, -19), 11 * p.s, 6.5f * p.s, wing);
  for (int i = 0; i < 4; i++)     // feather bars on the wing
    cv.drawLine(PX(p, -9 + i * 4), PY(p, -23), PX(p, -10 + i * 4), PY(p, -15), tom ? RGB(120, 84, 52) : RGB(150, 118, 84));
  if (tom) cv.drawLine(PX(p, 10), PY(p, -17), PX(p, 11), PY(p, -9), RGB(20, 14, 12));   // beard

  // neck and head
  float bob = moving ? sinf(c.ph * 2.4f) * 1.5f : 0;
  float peck = (c.m == EAT) ? (sinf(t * 11) * 0.5f + 0.5f) * 0.12f : 0;
  float h = min(1.0f, c.head + peck);
  float hx = 17 + 6 * h, hy = -37 + 35 * h + bob;
  uint16_t neck = tom ? RGB(120, 150, 190) : RGB(168, 140, 118);
  cv.drawWideLine(PX(p, 10), PY(p, -23), PX(p, hx), PY(p, hy), 4.2f * p.s, neck, neck);
  cv.fillCircle(PX(p, hx), PY(p, hy), 3.2f * p.s, neck);
  cv.fillTriangle(PX(p, hx + 2), PY(p, hy - 1.2f), PX(p, hx + 7), PY(p, hy + 0.6f), PX(p, hx + 2), PY(p, hy + 1.8f), RGB(226, 196, 100));
  if (tom) {
    cv.fillEllipse(PX(p, hx + 3), PY(p, hy + 3.4f), 1.6f * p.s, 3 * p.s, RGB(200, 40, 40));   // wattle
    cv.fillEllipse(PX(p, hx + 3.5f), PY(p, hy - 2.6f), 1.3f * p.s, 1.8f * p.s, RGB(205, 50, 50));
  }
  cv.fillCircle(PX(p, hx + 0.8f), PY(p, hy - 0.8f), 0.9f * p.s, RGB(15, 12, 10));
}

static void drawShadow(const Critter &c) {
  float s = scaleAt(c.y);
  bool deer = c.k <= FAWN;
  float rx = deer ? (c.k == FAWN ? 17 : 29) : (c.k == TOM ? 22 : 17);
  cv.fillEllipse(c.x, c.y + 1, rx * s, 3.6f * s, RGB(52, 44, 28));
}

static void drawTrunkFront(float x, float w) {
  cv.fillRect(x, 0, w, VH, RGB(44, 32, 24));
  cv.fillRect(x + w * 0.65f, 0, w * 0.35f, VH, RGB(30, 22, 17));
  for (int i = 0; i < 6; i++) {
    float lx = x + w * (0.12f + 0.11f * i);
    for (int y = 0; y < VH; y += 40) cv.drawLine(lx, y + 7 * (i & 1), lx + 1.5f, y + 36 + 7 * (i & 1), RGB(66, 50, 36));
  }
}

// ---------------------------------------------------------------- the hunter (drawing)
// The two big rocks in the bottom corners. They are in the background too; the hunter's are drawn again over him.
struct RockDef { int cx, cy, rx, ry, hx, hy, hrx, hry; uint16_t c, hc; };
static const RockDef ROCKS[2] = {
  {40, 314, 34, 22, 32, 305, 16, 8, RGB(92, 90, 94), RGB(128, 126, 130)},
  {434, 316, 32, 21, 426, 308, 14, 7, RGB(86, 84, 90), RGB(120, 118, 124)},
};
static void drawRock(Canvas &c, int i) {
  const RockDef &r = ROCKS[i];
  c.fillEllipse(r.cx, r.cy, r.rx, r.ry, r.c);
  c.fillEllipse(r.hx, r.hy, r.hrx, r.hry, r.hc);
}

static float critScale(const Critter &c) {
  return scaleAt(c.y) * (c.k == FAWN ? 0.62f : c.k == BUCK ? 1.0f : c.k == DOE ? 0.92f : c.k == TOM ? 1.0f : 0.85f);
}

// An animal the hunter got: lying on its back, legs up, X for an eye. Cartoon, no mess.
static void drawDown(const Critter &c) {
  Pose p = {c.x, c.y, critScale(c), c.dir};
  const uint16_t X = RGB(25, 20, 18);
  if (c.k <= FAWN) {
    const bool buck = c.k == BUCK, fawn = c.k == FAWN;
    const uint16_t body = buck ? RGB(138, 90, 50) : fawn ? RGB(172, 118, 70) : RGB(160, 112, 68);
    const uint16_t dark = buck ? RGB(100, 64, 36) : fawn ? RGB(135, 90, 52) : RGB(122, 84, 50);
    static const int8_t L[4][4] = {{-14, -14, -19, -31}, {-7, -15, -11, -32}, {8, -15, 13, -32}, {15, -14, 20, -31}};
    for (auto &l : L) {
      cv.drawWideLine(PX(p, l[0]), PY(p, l[1]), PX(p, l[2]), PY(p, l[3]), 3.0f * p.s, dark, dark);
      cv.fillCircle(PX(p, l[2]), PY(p, l[3]), 1.6f * p.s, RGB(40, 30, 24));
    }
    cv.fillEllipse(PX(p, 0), PY(p, -10), 26 * p.s, 9 * p.s, body);
    cv.fillEllipse(PX(p, 0), PY(p, -13), 20 * p.s, 4 * p.s, RGB(222, 200, 160));
    cv.fillTriangle(PX(p, -25), PY(p, -14), PX(p, -32), PY(p, -9), PX(p, -25), PY(p, -6), RGB(240, 235, 225));
    cv.drawWideLine(PX(p, 20), PY(p, -9), PX(p, 29), PY(p, -6), 8 * p.s, body, body);
    cv.fillCircle(PX(p, 31), PY(p, -6), 5.5f * p.s, body);
    cv.fillEllipse(PX(p, 37), PY(p, -4), 4.5f * p.s, 3.2f * p.s, RGB(196, 166, 126));
    cv.fillTriangle(PX(p, 28), PY(p, -9), PX(p, 22), PY(p, -15), PX(p, 30), PY(p, -12), dark);
    cv.drawLine(PX(p, 30), PY(p, -9), PX(p, 34), PY(p, -5), X);          // X eye
    cv.drawLine(PX(p, 34), PY(p, -9), PX(p, 30), PY(p, -5), X);
    if (buck) {
      const uint16_t ant = RGB(214, 194, 150);
      cv.drawWideLine(PX(p, 30), PY(p, -11), PX(p, 28), PY(p, -26), 1.7f * p.s, ant, ant);
      cv.drawWideLine(PX(p, 28), PY(p, -26), PX(p, 22), PY(p, -30), 1.7f * p.s, ant, ant);
      cv.drawWideLine(PX(p, 29), PY(p, -19), PX(p, 35), PY(p, -24), 1.7f * p.s, ant, ant);
    }
  } else {
    const bool tom = c.k == TOM;
    const uint16_t body = tom ? RGB(86, 56, 36) : RGB(124, 94, 64), wing = tom ? RGB(62, 40, 28) : RGB(98, 72, 48);
    const uint16_t legc = RGB(205, 150, 110), neck = tom ? RGB(120, 150, 190) : RGB(168, 140, 118);
    if (tom) {
      for (int i = 0; i < 7; i++) {                                         // the fan, spread on the ground
        const float a = 0.5f + 1.0f * i / 6, ex = -12 - 24 * sinf(a), ey = -9 - 24 * cosf(a) * 0.55f, a2 = 0.5f + 1.0f * (i + 1) / 6;
        cv.fillTriangle(PX(p, -12), PY(p, -9), PX(p, ex), PY(p, ey), PX(p, -12 - 24 * sinf(a2)), PY(p, -9 - 24 * cosf(a2) * 0.55f), (i & 1) ? RGB(158, 88, 40) : RGB(206, 168, 108));
      }
    } else cv.fillTriangle(PX(p, -11), PY(p, -9), PX(p, -25), PY(p, -12), PX(p, -22), PY(p, -4), wing);
    cv.drawWideLine(PX(p, 2), PY(p, -14), PX(p, -2), PY(p, -27), 2.2f * p.s, legc, legc);       // feet in the air
    cv.drawWideLine(PX(p, 7), PY(p, -14), PX(p, 5), PY(p, -28), 2.2f * p.s, legc, legc);
    cv.drawLine(PX(p, -2), PY(p, -27), PX(p, -6), PY(p, -26), legc);
    cv.drawLine(PX(p, 5), PY(p, -28), PX(p, 9), PY(p, -27), legc);
    cv.fillEllipse(PX(p, 0), PY(p, -8), 15 * p.s, 8 * p.s, body);
    cv.fillEllipse(PX(p, -2), PY(p, -11), 10 * p.s, 5 * p.s, wing);
    cv.drawWideLine(PX(p, 11), PY(p, -8), PX(p, 21), PY(p, -3), 4.2f * p.s, neck, neck);
    cv.fillCircle(PX(p, 22), PY(p, -3), 3.2f * p.s, neck);
    cv.fillTriangle(PX(p, 24), PY(p, -4), PX(p, 29), PY(p, -2.5f), PX(p, 24), PY(p, -1.5f), RGB(226, 196, 100));
    if (tom) cv.fillEllipse(PX(p, 25), PY(p, 0), 1.6f * p.s, 2.5f * p.s, RGB(200, 40, 40));
    cv.drawLine(PX(p, 21), PY(p, -5), PX(p, 23.5f), PY(p, -2), X);
    cv.drawLine(PX(p, 23.5f), PY(p, -5), PX(p, 21), PY(p, -2), X);
  }
}

static void drawHunter(float t) {
  const Hunter &h = hunter;
  if (h.st == H_OFF) return;
  float rise = h.st == H_RISE ? min(1.0f, h.t / 1.3f) : h.st == H_LOWER ? max(0.0f, 1.0f - h.t / 1.0f) : 1.0f;
  rise = rise * rise * (3 - 2 * rise);                                  // ease in and out
  const float s = 1.3f * scaleAt(constrain(h.y, (float)GROUND_MIN, (float)GROUND_MAX + 8));
  Pose p = {h.x, h.y + (1 - rise) * 50 * s, s, h.dir};                  // sinks behind the rock
  const uint16_t vest = RGB(238, 110, 22), vestD = RGB(190, 80, 16), pants = RGB(128, 116, 66), boot = RGB(36, 28, 22);
  const uint16_t skin = RGB(232, 188, 146), gun = RGB(60, 62, 68), wood = RGB(112, 72, 38);
  const bool aiming = h.st == H_AIM || h.st == H_SHOT, moving = h.st == H_RUN || h.st == H_DRAG;
  const float gait = h.t * (h.st == H_RUN ? 13.0f : 7.0f);

  for (int n = 0; n < 2; n++) {                                         // legs
    const float hip = n ? 2.0f : -2.0f, sw = moving ? sinf(gait + n * 3.14f) * 7 : 0, fx = hip + sw;
    cv.drawWideLine(PX(p, hip), PY(p, -17), PX(p, fx), PY(p, -1), 3.8f * p.s, pants, pants);
    cv.fillEllipse(PX(p, fx + 1.5f), PY(p, -1), 3.2f * p.s, 1.8f * p.s, boot);
  }
  cv.fillEllipse(PX(p, 0), PY(p, -26), 7.2f * p.s, 10 * p.s, vest);     // blaze orange vest
  cv.drawLine(PX(p, -3), PY(p, -32), PX(p, -3), PY(p, -19), vestD);
  float hx = 0, hy = 0;                                                 // where his hand is (for the rope)
  if (aiming) {
    const float k = (h.st == H_SHOT && h.since < 0.15f) ? 2.0f : 0.0f;  // recoil
    cv.drawWideLine(PX(p, -3 - k), PY(p, -27), PX(p, 9 - k), PY(p, -30), 3.4f * p.s, wood, wood);
    cv.drawWideLine(PX(p, 8 - k), PY(p, -30), PX(p, 33 - k), PY(p, -32.5f), 2.2f * p.s, gun, gun);
    cv.drawWideLine(PX(p, 3), PY(p, -29), PX(p, 18 - k), PY(p, -31), 3.4f * p.s, vestD, vestD);
    cv.fillCircle(PX(p, 18 - k), PY(p, -31), 1.8f * p.s, skin);
  } else if (h.st == H_DRAG) {
    cv.drawWideLine(PX(p, -5), PY(p, -38), PX(p, -1), PY(p, -12), 2.4f * p.s, gun, gun);       // rifle slung on his back
    cv.drawWideLine(PX(p, 3), PY(p, -27), PX(p, 11), PY(p, -21), 3.4f * p.s, vestD, vestD);
    hx = PX(p, 11); hy = PY(p, -21);
    cv.fillCircle(hx, hy, 1.8f * p.s, skin);
  } else {
    cv.drawWideLine(PX(p, 0), PY(p, -20), PX(p, 22), PY(p, -33), 2.4f * p.s, gun, gun);         // rifle carried
    cv.drawWideLine(PX(p, 3), PY(p, -27), PX(p, 10), PY(p, -23), 3.4f * p.s, vestD, vestD);
    cv.fillCircle(PX(p, 10), PY(p, -23), 1.8f * p.s, skin);
  }
  cv.fillCircle(PX(p, 3), PY(p, -37), 4.8f * p.s, skin);                // head, eye, orange cap
  cv.fillCircle(PX(p, 5.5f), PY(p, -37.5f), 0.9f * p.s, RGB(15, 12, 10));
  cv.fillEllipse(PX(p, 3), PY(p, -40.5f), 5.4f * p.s, 3.2f * p.s, vest);
  cv.fillTriangle(PX(p, 6), PY(p, -40), PX(p, 12), PY(p, -38.5f), PX(p, 6), PY(p, -37.5f), vestD);

  if (h.st == H_DRAG) {                                                 // the rope to what he is dragging
    const Critter &a = crit[h.tgt];
    cv.drawWideLine(hx, hy, a.x + h.dragDir * (a.k <= FAWN ? 22 : 12) * scaleAt(a.y), a.y - 7, 1.4f, RGB(150, 120, 70), RGB(150, 120, 70));
  }
  const bool nearRock = h.st != H_RUN && h.st != H_DRAG ? true : fabsf(h.x - ROCKS[h.side].cx) < ROCKS[h.side].rx;
  if (nearRock) drawRock(cv, h.side);                                   // he is behind it

  if (h.since < 2.4f) {                                                 // the shot: flash, smoke, BANG
    if (h.since < 0.14f) {
      cv.fillCircle(h.sx, h.sy, 7, RGB(255, 226, 120));
      cv.fillCircle(h.sx, h.sy, 4, RGB(255, 255, 235));
      for (int k = 0; k < 5; k++) {
        const float a = (k - 2) * 0.5f;
        cv.drawWideLine(h.sx, h.sy, h.sx + h.dir * 18 * cosf(a), h.sy + 18 * sinf(a), 2.5f, RGB(255, 226, 120), RGB(255, 226, 120));
      }
    }
    for (int j = 0; j < 4; j++) {
      const float age = h.since - 0.06f * j;
      if (age <= 0 || age >= 2.4f) continue;
      cv.fillCircle(h.sx + h.dir * (age * 16 + j * 2), h.sy - age * 12, 3 + age * 7, mix(228, 228, 234, 150, 158, 176, age / 2.4f));
    }
    if (h.since < 0.5f) {
      cv.setTextDatum(MC_DATUM);
      cv.setTextColor(RGB(60, 30, 10)); cv.drawString("BANG!", h.sx + h.dir * 30 + 1, h.sy - 22 + 1, FONT_BIG);
      cv.setTextColor(RGB(255, 236, 140)); cv.drawString("BANG!", h.sx + h.dir * 30, h.sy - 22, FONT_BIG);
    }
  }
}

static void drawInfo(uint32_t now);

// ---------------------------------------------------------------- clock + date
// The boards have no battery clock. Time comes from NTP, from a device's browser (web page) or, after a restart,
// from the last time we saved (so the clock is roughly right immediately).
static uint32_t baseSecs = 0;      // seconds since midnight at millis()==0 (fallback clock)

static void initClockFallback() {
  int h = 0, m = 0, s = 0;
  sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
  baseSecs = h * 3600 + m * 60 + s;
}

static bool timeIsSet() { struct tm t; return getLocalTime(&t, 0); }
static void saveTimeNow() { if (timeIsSet()) prefs.putUInt("lastt", (uint32_t)time(nullptr)); }
static void setClockEpoch(uint32_t epoch) {
  struct timeval tv = {(time_t)epoch, 0};
  settimeofday(&tv, nullptr);
  saveTimeNow();
}
static bool ntpSynced() { return sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED; }

// Time and date text in the chosen formats (date is empty until the clock is set, or when hidden).
static void clockStrings(char *buf, size_t bn, char *date, size_t dn) {
  int h, mi;
  date[0] = 0;
  struct tm tmv;
  if (getLocalTime(&tmv, 0)) {
    h = tmv.tm_hour; mi = tmv.tm_min;
    static const char *dnm[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *mnm[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    switch (g_datefmt) {
      case 0: snprintf(date, dn, "%s %02d/%02d/%02d", dnm[tmv.tm_wday], tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year % 100); break;
      case 1: snprintf(date, dn, "%s %02d/%02d/%02d", dnm[tmv.tm_wday], tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100); break;
      case 2: snprintf(date, dn, "%s %04d-%02d-%02d", dnm[tmv.tm_wday], tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday); break;
      case 3: snprintf(date, dn, "%s, %s %d", dnm[tmv.tm_wday], mnm[tmv.tm_mon], tmv.tm_mday); break;
      default: break;
    }
  } else {
    uint32_t s = (baseSecs + millis() / 1000) % 86400;
    h = s / 3600; mi = (s / 60) % 60;
  }
  if (g_h24) snprintf(buf, bn, "%02d:%02d", h, mi);
  else {
    int h12 = h % 12; if (h12 == 0) h12 = 12;
    snprintf(buf, bn, "%d:%02d %s", h12, mi, h >= 12 ? "PM" : "AM");
  }
}

// Top-right corner, white text on a plaque the colour of the sky behind it.
static void drawClock() {
  char buf[16], date[24];
  clockStrings(buf, sizeof(buf), date, sizeof(date));
  const float tw = cv.s->textWidth(buf, 4) / cv.sx, dw = date[0] ? cv.s->textWidth(date, 2) / cv.sx : 0;
  const float bell = g_alOn ? 22 : 0, w = max(tw + bell, dw) + 16;
  const float h = (date[0] ? DATE_Y + 16 / cv.sy : TIME_Y + 28 / cv.sy) + 4;
  cv.fillRoundRect(VW - 4 - w, 2, w, h, 8, RGB(104, 140, 186));
  cv.drawRoundRect(VW - 4 - w, 2, w, h, 8, RGB(70, 102, 148));
  cv.setTextDatum(TR_DATUM);
  cv.setTextColor(RGB(30, 52, 84));
  cv.drawString(buf, VW - 9, TIME_Y + 1, 4);
  if (date[0]) cv.drawString(date, VW - 9, DATE_Y + 1, 2);
  cv.setTextColor(RGB(255, 255, 255));
  cv.drawString(buf, VW - 10, TIME_Y, 4);
  if (date[0]) cv.drawString(date, VW - 10, DATE_Y, 2);
  if (g_alOn) {                                   // little bell: an alarm is set
    const float bx = VW - 10 - tw - 16, by = TIME_Y + 13;
    const uint16_t gold = RGB(240, 200, 60);
    cv.fillEllipse(bx, by, 6, 7, gold);
    cv.fillRect(bx - 8, by + 4, 16, 3, gold);
    cv.fillCircle(bx, by + 10, 2, gold);
  }
}

// ---------------------------------------------------------------- alarm clock
// A minute before the set time the woods brighten like a sunrise (silently) and every animal stops and looks up, the tom
// fanning his tail; at the time the chime plays, getting louder, over a big clock until Stop or Snooze (touch, or the BOOT
// button). It sounds even with the woods muted.
static void alSetSound(int lvl) {
  const bool was = audioAlarmOn();
  audioAlarm(lvl);
  if (was != (lvl > 0)) halApplySound();
}
static void alStop() { alState = AL_IDLE; alSetSound(0); }
static void alSnooze(uint32_t now) { alState = AL_SNOOZE; alSnoozeUntil = now + (uint32_t)g_alSnz * 60000UL; alSetSound(0); }
static void alTest(uint32_t now) { alRampLen = 8000; alT0 = now; alState = AL_RAMP; alMissed = false; }
static float alProgress(uint32_t now) { return alState == AL_RAMP ? min(1.0f, (now - alT0) / (float)alRampLen) : 1.0f; }

static void updateAlarm(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 250) return;
  last = now;
  if (alState == AL_IDLE && g_alOn && !alPanel) {                 // not while the set panel is open: wait for Done
    struct tm tm;
    if (getLocalTime(&tm, 0) && ((g_alDays >> tm.tm_wday) & 1) && alFiredDay != tm.tm_yday) {
      const int d = g_alMin * 60 - (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);   // seconds until the alarm
      if (d <= 60 && d > -120) {
        alFiredDay = tm.tm_yday; alRampLen = 60000; alMissed = false;
        alT0 = now - (d > 0 ? (uint32_t)(60 - d) * 1000UL : 60000UL);
        alState = AL_RAMP;
      }
    }
  }
  if (alState == AL_RAMP && now - alT0 >= alRampLen) { alState = AL_RING; alT0 = now; }
  else if (alState == AL_SNOOZE && now >= alSnoozeUntil) { alState = AL_RING; alT0 = now; }
  else if (alState == AL_RING && now - alT0 > 600000UL) { alStop(); alMissed = true; }   // nobody came: give up after 10 min
  if (alActive()) {
    const float e = min(1.0f, (now - alT0) / 30000.0f);
    alSetSound(alState == AL_RAMP ? 0 : (int)(g_alVol * (0.6f + 0.4f * e)));      // silent sunrise lead-in; the chime starts at the set time
  }
  if (alPanel && now - alPanelAt > 30000) { alPanel = false; saveSettings(); }
}

static void seg7(float x, float y, float h, int d, uint16_t c) {       // one 7-segment digit, top-left x,y
  static const uint8_t M[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  const float w = h * 0.55f, t = h * 0.13f, hh = h / 2, v = hh - 1.5f * t;
  const uint8_t m = M[d];
  if (m & 1)  cv.fillRect(x + t, y, w - 2 * t, t, c);
  if (m & 2)  cv.fillRect(x + w - t, y + t, t, v, c);
  if (m & 4)  cv.fillRect(x + w - t, y + hh + t / 2, t, v, c);
  if (m & 8)  cv.fillRect(x + t, y + h - t, w - 2 * t, t, c);
  if (m & 16) cv.fillRect(x, y + hh + t / 2, t, v, c);
  if (m & 32) cv.fillRect(x, y + t, t, v, c);
  if (m & 64) cv.fillRect(x + t, y + hh - t / 2, w - 2 * t, t, c);
}
static void digitsGeom(float cx, float h, float &x0, float &hC, float &mC) {
  const float w = h * 0.55f, gap = h * 0.18f, cw = h * 0.22f, W = 4 * w + cw + 4 * gap;
  x0 = cx - W / 2;
  hC = x0 + (2 * w + gap) / 2;
  mC = x0 + 2 * w + gap + gap + cw + gap + (2 * w + gap) / 2;
}
static void drawDigits(float cx, float y, float h, int hh, int mm, bool lead, bool colon, uint16_t c) {
  float x0, hC, mC;
  digitsGeom(cx, h, x0, hC, mC);
  const float w = h * 0.55f, gap = h * 0.18f, cw = h * 0.22f;
  float x = x0;
  if (lead || hh >= 10) seg7(x, y, h, hh / 10, c);
  x += w + gap; seg7(x, y, h, hh % 10, c);
  x += w + gap;
  if (colon) { const float s = h * 0.12f; cv.fillRect(x + (cw - s) / 2, y + h * 0.3f, s, s, c); cv.fillRect(x + (cw - s) / 2, y + h * 0.65f, s, s, c); }
  x += cw + gap; seg7(x, y, h, mm / 10, c);
  x += w + gap; seg7(x, y, h, mm % 10, c);
}

// Warm sunrise wash over the whole scene, stronger at the top.
static void alarmTint(float amt) {
  uint16_t *px = (uint16_t *)cv.s->getPointer();
  const int w = cv.s->width(), hgt = cv.s->height();
  for (int y = 0; y < hgt; y++) {
    const float wy = (y + cv.oy) / cv.sy;
    const int k = (int)(amt * 200.0f * (1.0f - 0.5f * wy / VH));
    if (k <= 0) continue;
    uint16_t *row = px + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      const uint16_t v = bswap16(row[x]);
      int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
      r += ((31 - r) * k) >> 8; g += ((50 - g) * k) >> 8; b += ((12 - b) * k) >> 8;
      row[x] = bswap16((r << 11) | (g << 5) | b);
    }
  }
}

#define BTN_SNZ_X 60
#define BTN_STOP_X 250
#define BTN_Y 232
#define BTN_W 170
#define BTN_H 58

static void drawAlarmScreen(float t, uint32_t now) {
  const float p = alProgress(now);
  const float pulse = alState == AL_RING ? 0.5f + 0.5f * sinf(t * 4.0f) : 0;
  alarmTint(min(1.0f, (0.3f + 0.7f * p) * (1.0f + 0.12f * pulse)));
  if (alState != AL_RING) { drawClock(); return; }       // the sunrise lead-in is just the glow; the big clock and buttons come with the sound
  char buf[16], date[24];
  clockStrings(buf, sizeof(buf), date, sizeof(date));
  const int h12 = atoi(buf);
  const char *col = strchr(buf, ':');
  const int mm = col ? atoi(col + 1) : 0;
  const char *ap = col ? strchr(col, ' ') : nullptr;
  const bool colon = (now / 500) % 2 == 0;
  drawDigits(VW / 2 + 2, 82, 78, h12, mm, g_h24, true, RGB(120, 60, 10));
  drawDigits(VW / 2, 80, 78, h12, mm, g_h24, colon, RGB(255, 244, 205));
  cv.setTextDatum(TC_DATUM);
  if (ap) { cv.setTextColor(RGB(255, 244, 205)); cv.drawString(ap + 1, VW / 2, 166, 4); }
  cv.setTextColor(RGB(255, 255, 255)); cv.drawString("Time to get up!", VW / 2, 28, 4);
  cv.fillRoundRect(BTN_SNZ_X, BTN_Y, BTN_W, BTN_H, 12, RGB(120, 80, 36));
  cv.fillRoundRect(BTN_STOP_X, BTN_Y, BTN_W, BTN_H, 12, RGB(176, 44, 34));
  cv.drawRoundRect(BTN_SNZ_X, BTN_Y, BTN_W, BTN_H, 12, RGB(244, 200, 84));
  cv.drawRoundRect(BTN_STOP_X, BTN_Y, BTN_W, BTN_H, 12, RGB(255, 190, 180));
  cv.setTextColor(RGB(255, 255, 255));
  cv.setTextDatum(MC_DATUM);
  char sb[16];
  snprintf(sb, sizeof(sb), "%d min", g_alSnz);
  cv.drawString("Snooze", BTN_SNZ_X + BTN_W / 2, BTN_Y + 20, 4);
  cv.drawString(sb, BTN_SNZ_X + BTN_W / 2, BTN_Y + 43, 2);
  cv.drawString("Stop", BTN_STOP_X + BTN_W / 2, BTN_Y + BTN_H / 2, 4);
}

// On-screen alarm setter: tap the time in the corner.
#define PANEL_DIG_H 56
static void drawAlarmPanel() {
  const uint16_t bg = RGB(34, 24, 16), fg = RGB(255, 238, 196), acc = RGB(214, 150, 44);
  cv.fillRoundRect(70, 22, 340, 276, 14, bg);
  cv.drawRoundRect(70, 22, 340, 276, 14, acc);
  cv.setTextDatum(TC_DATUM);
  cv.setTextColor(fg);
  cv.drawString("Alarm clock", 240, 32, 4);
  float x0, hC, mC;
  digitsGeom(240, PANEL_DIG_H, x0, hC, mC);
  const int h24 = g_alMin / 60, mm = g_alMin % 60;
  const int hs = g_h24 ? h24 : (h24 % 12 == 0 ? 12 : h24 % 12);
  drawDigits(240, 110, PANEL_DIG_H, hs, mm, g_h24, true, g_alOn ? RGB(255, 214, 95) : RGB(140, 126, 106));
  const float cx[2] = {hC, mC};
  for (int i = 0; i < 2; i++) {
    cv.fillTriangle(cx[i] - 16, 100, cx[i] + 16, 100, cx[i], 76, acc);
    cv.fillTriangle(cx[i] - 16, 178, cx[i] + 16, 178, cx[i], 202, acc);
  }
  if (!g_h24) {
    cv.fillRoundRect(345, 126, 56, 36, 8, RGB(120, 80, 36));
    cv.setTextDatum(MC_DATUM);
    cv.drawString(h24 < 12 ? "AM" : "PM", 373, 144, 4);
  }
  cv.setTextDatum(MC_DATUM);
  cv.fillRoundRect(90, 244, 130, 42, 10, g_alOn ? RGB(50, 128, 66) : RGB(96, 88, 78));
  cv.drawString(g_alOn ? "Alarm ON" : "Alarm OFF", 155, 265, 4);
  cv.fillRoundRect(260, 244, 130, 42, 10, RGB(120, 80, 36));
  cv.drawString("Done", 325, 265, 4);
  cv.setTextColor(RGB(190, 170, 140));
  cv.drawString("days and loudness: web page", 240, 224, 2);
}

// Touch while the panel is open. Arrows repeat while held; the buttons act once.
static void alPanelTouch(float wx, float wy, bool press, bool rep, uint32_t now) {
  alPanelAt = now;
  auto in = [&](float x, float y, float w, float h) { return wx >= x && wx < x + w && wy >= y && wy < y + h; };
  float x0, hC, mC;
  digitsGeom(240, PANEL_DIG_H, x0, hC, mC);
  const int hh = g_alMin / 60, mi = g_alMin % 60;
  bool changed = false;
  if (press || rep) {
    if (in(hC - 38, 66, 76, 52))       { g_alMin = ((hh + 1) % 24) * 60 + mi; changed = true; }
    else if (in(hC - 38, 160, 76, 52)) { g_alMin = ((hh + 23) % 24) * 60 + mi; changed = true; }
    else if (in(mC - 38, 66, 76, 52))  { g_alMin = hh * 60 + (mi + 1) % 60; changed = true; }
    else if (in(mC - 38, 160, 76, 52)) { g_alMin = hh * 60 + (mi + 59) % 60; changed = true; }
  }
  if (press) {
    if (!g_h24 && in(340, 120, 66, 48)) { g_alMin = (g_alMin + 720) % 1440; changed = true; }
    else if (in(90, 244, 130, 42)) { g_alOn = !g_alOn; alFiredDay = -1; }
    else if (in(260, 244, 130, 42)) { alPanel = false; saveSettings(); }
  }
  if (changed) { g_alOn = true; alFiredDay = -1; }
}

static void drawScene(float t) {
  // depth-sort the animals by their row (nearer = lower = drawn later)
  int order[NCRIT];
  for (int i = 0; i < NCRIT; i++) order[i] = i;
  for (int i = 1; i < NCRIT; i++) {
    int k = order[i], j = i - 1;
    while (j >= 0 && crit[order[j]].y > crit[k].y) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }
  for (int pass = 0; pass < 2; pass++)          // the base of the pile first, then the kernels heaped on top
    for (auto &f : corn) {
      if (!f.on || (f.z > 1.5f) != (pass == 1)) continue;
      cv.fillEllipse(f.x, f.y - f.z, 1.7f, 1.1f, RGB(236, 190, 60));
      cv.drawPixel(f.x - 1, f.y - f.z - 1, RGB(255, 240, 170));
    }
  for (int i = 0; i < NCRIT; i++) if (crit[order[i]].m != HIDDEN) drawShadow(crit[order[i]]);
  for (int i = 0; i < NCRIT; i++) {
    const Critter &c = crit[order[i]];
    if (c.m == HIDDEN) continue;
    if (c.m == DOWN) drawDown(c);
    else if (c.k <= FAWN) drawDeer(c, t); else drawTurkey(c, t);
  }
  drawHunter(t);
  for (auto &l : leaves) {
    float s = sinf(t * 3 + l.ph);
    cv.fillEllipse(l.x, l.y, 2.6f, 1.2f + 0.9f * fabsf(s), l.c);
  }
  drawTrunkFront(-6, 34);
  drawTrunkFront(VW - 28, 40);
  if (alActive()) drawAlarmScreen(t, millis()); else drawClock();
  if (alPanel) drawAlarmPanel();
  if (infoUntil > millis()) drawInfo(millis());
}

// Static scenery: sky, sun, ridge, three layers of trees, forest floor.
static void pine(Canvas &c, float x, float base, float h, uint16_t col) {
  for (int i = 0; i < 3; i++) {
    float top = base - h + i * h * 0.28f, bot = top + h * 0.46f, hw = (0.13f + 0.07f * i) * h;
    c.fillTriangle(x, top, x - hw, bot, x + hw, bot, col);
  }
}

static uint32_t treeSeed = 11;
static int rnd(int n) { treeSeed = treeSeed * 1664525u + 1013904223u; return (int)((treeSeed >> 8) % n); }

struct Pal { uint16_t dark, mid, light; };
static const Pal PALS[4] = {
  {RGB(120, 52, 26), RGB(190, 92, 34), RGB(228, 138, 52)},     // orange
  {RGB(150, 100, 28), RGB(214, 150, 44), RGB(244, 200, 84)},   // gold
  {RGB(110, 42, 28), RGB(160, 62, 34), RGB(204, 96, 48)},      // red
  {RGB(70, 78, 36), RGB(110, 116, 48), RGB(150, 150, 64)},     // olive
};

// A leafy clump: shaded underside, body, then small highlights up and to the left.
static void crown(Canvas &c, float x, float y, float r, const Pal &p) {
  float bx[6], by[6], br[6];
  for (int i = 0; i < 6; i++) {
    bx[i] = x + rnd((int)(r * 2)) - r;
    by[i] = y + rnd((int)r) - r * 0.6f;
    br[i] = r * 0.5f + rnd((int)(r * 0.35f));
  }
  for (int i = 0; i < 6; i++) c.fillEllipse(bx[i], by[i] + 3, br[i], br[i] * 0.75f, p.dark);
  for (int i = 0; i < 6; i++) c.fillEllipse(bx[i], by[i], br[i] * 0.92f, br[i] * 0.7f, p.mid);
  for (int i = 0; i < 6; i++) c.fillEllipse(bx[i] - br[i] * 0.25f, by[i] - br[i] * 0.25f, br[i] * 0.45f, br[i] * 0.3f, p.light);
}

// Trunk tapers from the roots to the crown, a shaded right side, two branches that reach out to leafy puffs.
static void tree(Canvas &c, float x, float base, float top, float w, float cr, const Pal &p, uint16_t bark, uint16_t shade) {
  c.fillTriangle(x - w * 0.8f, base, x + w * 0.8f, base, x - w * 0.4f, top, bark);
  c.fillTriangle(x + w * 0.8f, base, x + w * 0.4f, top, x - w * 0.4f, top, bark);
  c.fillTriangle(x + w * 0.05f, base, x + w * 0.8f, base, x + w * 0.4f, top, shade);
  c.fillTriangle(x - w * 0.8f, base, x - w * 1.8f, base + 5, x - w * 0.5f, base - 12, bark);       // roots
  c.fillTriangle(x + w * 0.8f, base, x + w * 1.8f, base + 5, x + w * 0.5f, base - 12, shade);
  float by = top + (base - top) * 0.3f;
  c.drawWideLine(x, by, x - cr * 0.8f, top - cr * 0.05f, w * 0.42f, bark, bark);
  c.drawWideLine(x, by + 6, x + cr * 0.85f, top + cr * 0.1f, w * 0.42f, shade, shade);
  crown(c, x - cr * 0.8f, top, cr * 0.55f, p);
  crown(c, x + cr * 0.85f, top + cr * 0.15f, cr * 0.55f, p);
  crown(c, x, top - cr * 0.3f, cr, p);
}

static void renderBackground(Canvas &c) {
  TFT_eSprite *s = c.s;
  treeSeed = 11;
  for (int r = 0; r < s->height(); r++) {
    float vy = (r + c.oy) / c.sy;
    if (vy < HORIZON) s->drawFastHLine(0, r, s->width(), mix(112, 148, 192, 248, 216, 164, vy / HORIZON));
  }
  // low autumn sun with a soft halo
  for (int k = 0; k < 8; k++)
    c.fillCircle(372, 78, 52 - k * 6.5f, mix(214, 200, 176, 255, 242, 204, k / 7.0f));

  // far ridge and pines
  for (int x = -20; x < VW + 60; x += 76) c.fillEllipse(x, HORIZON + 8, 90, 34 + rnd(12), RGB(128, 134, 150));
  for (int x = 4; x < VW; x += 13 + rnd(7)) pine(c, x, HORIZON + 6, 44 + rnd(30), RGB(78, 100, 98));

  // forest floor
  for (int r = 0; r < s->height(); r++) {
    float vy = (r + c.oy) / c.sy;
    if (vy >= HORIZON) s->drawFastHLine(0, r, s->width(), mix(116, 104, 58, 72, 52, 33, (vy - HORIZON) / (VH - HORIZON)));
  }

  // middle autumn trees: tapered trunk, two branches, a shaded crown of oranges, golds and reds
  for (int x = 20; x < VW; x += 44 + rnd(16)) {
    float base = HORIZON + 12 + rnd(12), top = base - 78 - rnd(36), w = 4 + rnd(2);
    tree(c, x, base, top, w, 20 + rnd(8), PALS[rnd(4)], RGB(78, 58, 44), RGB(54, 40, 30));
  }
  // bushes along the tree line
  for (int x = 0; x < VW; x += 34 + rnd(24))
    c.fillEllipse(x, HORIZON + 16, 22 + rnd(14), 9 + rnd(5), mix(76, 82, 42, 106, 88, 40, rnd(100) / 100.0f));

  // near trees: tall dark trunks that run off the top of the screen
  static const int nearX[4] = {92, 196, 300, 392};
  for (int i = 0; i < 4; i++) {
    float x = nearX[i] + rnd(12), w = 6 + rnd(3), base = 206 + rnd(12);
    const Pal &pal = PALS[rnd(3)];
    uint16_t bark = RGB(62, 46, 34), shade = RGB(42, 30, 22);
    float top = base - 150 - rnd(22);
    tree(c, x, base, top, w, 28, pal, bark, shade);
    float by = top + 48 + rnd(20), dx = (rnd(2) ? 1 : -1) * (20 + rnd(10));      // one extra limb with a leafy puff
    c.drawWideLine(x, by + 10, x + dx, by, w * 0.5f, bark, bark);
    crown(c, x + dx, by - 2, 14 + rnd(5), pal);
    for (int k = 0; k < 5; k++) c.drawLine(x - w * 0.4f + k * w * 0.25f, top + 30 + rnd(40), x - w * 0.4f + k * w * 0.25f, top + 70 + rnd(70), shade);   // bark
  }

  // a fallen log
  c.fillRoundRect(318, 209, 72, 13, 6, RGB(84, 60, 40));
  c.fillRoundRect(318, 209, 72, 4, 2, RGB(106, 80, 54));
  c.fillEllipse(318, 215.5f, 4, 6.5f, RGB(150, 118, 78));

  // leaf litter, grass tufts, rocks
  static const uint16_t litter[4] = {RGB(188, 96, 36), RGB(214, 150, 50), RGB(150, 64, 30), RGB(100, 78, 40)};
  for (int i = 0; i < 380; i++) {
    int x = rnd(VW), y = HORIZON + 4 + rnd(VH - HORIZON - 4);
    if (i % 3) c.drawPixel(x, y, litter[rnd(4)]); else c.fillEllipse(x, y, 2, 1, litter[rnd(4)]);
  }
  for (int i = 0; i < 70; i++) {
    int x = rnd(VW), y = 214 + rnd(104);
    uint16_t g = mix(90, 96, 44, 124, 112, 52, rnd(100) / 100.0f);
    float hgt = 5 + (y - 214) * 0.05f;
    c.drawLine(x, y, x - 2, y - hgt, g); c.drawLine(x, y, x, y - hgt - 2, g); c.drawLine(x, y, x + 2, y - hgt, g);
  }
  drawRock(c, 0); drawRock(c, 1);                 // the two big rocks, which the hunter hides behind
}

// ---------------------------------------------------------------- WiFi, web page, OTA
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Deer Hunt</title><style>body{font:16px system-ui;background:#1a1612;color:#eadfcb;max-width:560px;margin:0 auto;padding:16px}
h1{margin:0 0 4px}small{color:#a89b82}.c{background:#2a231b;border-radius:10px;padding:14px;margin:12px 0}
label{display:block;margin:8px 0}select,input[type=number]{background:#1a1612;color:#eadfcb;border:1px solid #5a4a36;border-radius:6px;padding:4px}
button,a.b{background:#b8742e;color:#fff;border:0;border-radius:8px;padding:9px 14px;font:inherit;text-decoration:none;display:inline-block;margin:4px 4px 0 0;cursor:pointer}
</style></head><body><h1>Deer Hunt</h1><small id="v"></small>
<div class="c"><b>Status</b><div id="s">...</div></div>
<div class="c"><b>Sound</b><br><label><input type="checkbox" id="snd"> Woods sounds on</label>
<label>Volume <input type="range" min="0" max="100" id="vl" style="vertical-align:middle;width:55%"> <span id="vv"></span></label></div>
<div class="c"><b>Date &amp; time</b> <small id="now"></small>
<label>Time zone <select id="tz">
<option value="EST5EDT,M3.2.0,M11.1.0">US Eastern</option><option value="CST6CDT,M3.2.0,M11.1.0">US Central</option>
<option value="MST7MDT,M3.2.0,M11.1.0">US Mountain</option><option value="MST7">US Arizona</option>
<option value="PST8PDT,M3.2.0,M11.1.0">US Pacific</option><option value="AKST9AKDT,M3.2.0,M11.1.0">US Alaska</option>
<option value="HST10">US Hawaii</option><option value="GMT0BST,M3.5.0/1,M10.5.0">UK / Ireland</option>
<option value="CET-1CEST,M3.5.0,M10.5.0">Central Europe</option><option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe</option>
<option value="IST-5:30">India</option><option value="CST-8">China</option><option value="JST-9">Japan</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia East</option><option value="UTC0">UTC</option></select></label>
<label>Date format <select id="dfmt"><option value="0">Tue 10/20/26 (month/day)</option><option value="1">Tue 20/10/26 (day/month)</option>
<option value="2">Tue 2026-10-20</option><option value="3">Tue, Oct 20</option><option value="4">Hide the date</option></select></label>
<label><input type="checkbox" id="h24"> 24-hour time</label>
<button id="settime">Set time from this device</button> <small id="tmsg"></small></div>
<div class="c"><b>Alarm clock</b> <small id="alstat"></small>
<label><input type="checkbox" id="alon"> Alarm on</label>
<label>Wake at <span><select id="al_h"></select> : <select id="al_m"></select></span></label>
<div id="aldays" style="margin:6px 0"></div>
<label>Snooze (minutes) <input type="number" id="alsnz" min="1" max="30" style="width:80px"></label>
<label>Alarm loudness <input type="range" id="alvol" min="1" max="100" style="vertical-align:middle;width:50%"> <span id="alvv"></span></label>
<button id="apply">Apply</button> <button id="altest">Test alarm</button> <button id="alstop">Stop alarm</button> <small id="amsg"></small>
<div><small>A minute before, the woods silently brighten like a sunrise and every animal stops to look; at the set time the chime starts over a big clock. You can also tap the time on the screen to set it. The alarm sounds even if the woods are muted.</small></div></div>
<div class="c"><b>Hunter</b><br><label><input type="checkbox" id="hunt"> The hunter visits now and then</label>
<button onclick="fetch('/api/hunter')">Send the hunter</button>
<div><small>He hides behind a rock in a bottom corner, pops up and shoots. A miss sends every animal running; sometimes he gets the big buck or the tom and drags it off, and it wanders back later. Tap a rock on the screen to call him too.</small></div></div>
<div class="c"><b>Screen</b><br><button onclick="fetch('/api/rot')">Flip screen 180&deg;</button>
<button onclick="fetch('/api/corn')">Drop corn</button>
<label><input type="checkbox" id="swaprb"> Swap red/blue (RGB / BGR)</label>
<label><input type="checkbox" id="inv"> Invert colours (negative)</label>
<small>If the colours look wrong, such as blue skin tones or a negative image, try these. A white screen usually means the other CYD build.</small></div>
<div class="c"><b>Maintenance</b><br><a class="b" href="/update">Firmware update</a>
<button onclick="if(confirm('Forget WiFi and restart?'))fetch('/api/wifi/forget')">Forget WiFi</button>
<button onclick="if(confirm('Restart?'))fetch('/api/reboot')">Restart</button></div>
<script>
const $=i=>document.getElementById(i);let dirty=0,busy=0;
const mark=()=>{dirty=1;$('amsg').textContent=''};
(function(){let h='',m='',d='';for(let i=0;i<24;i++)h+='<option>'+String(i).padStart(2,'0')+'</option>';
for(let i=0;i<60;i++)m+='<option>'+String(i).padStart(2,'0')+'</option>';
['Sun','Mon','Tue','Wed','Thu','Fri','Sat'].forEach((n,i)=>d+='<label style="display:inline-block;margin:2px 10px 2px 0"><input type="checkbox" id="ad'+i+'"> '+n+'</label>');
$('al_h').innerHTML=h;$('al_m').innerHTML=m;$('aldays').innerHTML=d})();
const getT=()=>$('al_h').value+':'+$('al_m').value;
async function load(){const s=await(await fetch('/api/status')).json();
$('v').textContent='v'+s.version+' - '+s.board;
$('s').innerHTML=s.ssid+' ('+s.rssi+' dBm)<br>'+s.ip+' / '+s.host+'.local<br>up '+Math.round(s.uptime/60)+' min, '+s.fps.toFixed(1)+' fps';
$('now').textContent=s.now+(s.ntp?'  (internet time)':'');
if(!s.ntp&&!window.timeSent){window.timeSent=1;fetch('/api/settime?epoch='+Math.floor(Date.now()/1000),{method:'POST'})}
$('alstat').textContent=['','sunrise starting…','RINGING','snoozed'][s.alstate]||(s.almiss?'last alarm was not stopped':'');
$('inv').checked=s.inv;$('swaprb').checked=s.swaprb;$('hunt').checked=s.hunt;
if(document.activeElement!==$('vl')){$('snd').checked=s.snd;$('vl').value=s.vol;$('vv').textContent=s.vol}
if(!busy&&!dirty){$('alon').checked=s.alon;const t=s.atime.split(':');$('al_h').value=t[0];$('al_m').value=t[1];
for(let i=0;i<7;i++)$('ad'+i).checked=!!((s.adays>>i)&1);$('alsnz').value=s.asnz;$('alvol').value=s.avol;$('alvv').textContent=s.avol;
$('tz').value=s.tz;$('dfmt').value=s.dfmt;$('h24').checked=s.h24}}
async function send(){busy=1;await fetch('/api/set?tz='+encodeURIComponent($('tz').value)+'&dfmt='+$('dfmt').value+'&h24='+($('h24').checked?1:0)+'&alon='+($('alon').checked?1:0)+'&atime='+getT()+'&adays='+[0,1,2,3,4,5,6].reduce((a,i)=>a+($('ad'+i).checked?1<<i:0),0)+'&asnz='+$('alsnz').value+'&avol='+$('alvol').value,{method:'POST'});busy=0;dirty=0;$('amsg').textContent='Applied ✓';load()}
['tz','dfmt','h24','alon','al_h','al_m','ad0','ad1','ad2','ad3','ad4','ad5','ad6','alsnz'].forEach(i=>{$(i).onchange=mark;$(i).oninput=mark});
$('alvol').oninput=()=>{$('alvv').textContent=$('alvol').value;mark()};
$('apply').onclick=send;$('altest').onclick=()=>fetch('/api/alarm?cmd=test',{method:'POST'});$('alstop').onclick=()=>fetch('/api/alarm?cmd=stop',{method:'POST'});
$('settime').onclick=async()=>{await fetch('/api/settime?epoch='+Math.floor(Date.now()/1000),{method:'POST'});$('tmsg').textContent='time set ✓';load()};
const snd=()=>fetch('/api/snd?on='+($('snd').checked?1:0)+'&vol='+$('vl').value);
$('snd').onchange=snd;$('vl').oninput=()=>{$('vv').textContent=$('vl').value;snd()};
const col=()=>fetch('/api/set?inv='+($('inv').checked?1:0)+'&swaprb='+($('swaprb').checked?1:0),{method:'POST'});
$('inv').onchange=col;$('swaprb').onchange=col;
$('hunt').onchange=()=>fetch('/api/set?hunt='+($('hunt').checked?1:0),{method:'POST'});
load();setInterval(load,3000);
</script></body></html>)HTML";

static String statusJson() {
  String ssid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  ssid.replace("\\", ""); ssid.replace("\"", "");
  char tbuf[16], dbuf[24];
  clockStrings(tbuf, sizeof(tbuf), dbuf, sizeof(dbuf));
  char b[880];
  snprintf(b, sizeof(b),
           "{\"version\":\"%s\",\"board\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"host\":\"%s\",\"uptime\":%lu,\"fps\":%.1f,\"rot\":%d,"
           "\"snd\":%s,\"vol\":%d,\"now\":\"%s  %s\",\"timeset\":%s,\"ntp\":%s,\"tz\":\"%s\",\"dfmt\":%d,\"h24\":%s,\"inv\":%s,\"swaprb\":%s,\"hunt\":%s,\"hst\":%d,"
           "\"alon\":%s,\"atime\":\"%02d:%02d\",\"adays\":%d,\"asnz\":%d,\"avol\":%d,\"alstate\":%d,\"almiss\":%s}",
           FW_VERSION, BOARD_NAME, ssid.c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str(), g_host.c_str(), millis() / 1000UL, g_fps, g_rot,
           g_snd ? "true" : "false", (int)g_vol, tbuf, dbuf, timeIsSet() ? "true" : "false", ntpSynced() ? "true" : "false", g_tz.c_str(), g_datefmt, g_h24 ? "true" : "false", g_invert ? "true" : "false", g_swaprb ? "true" : "false", g_hunt ? "true" : "false", (int)hunter.st,
           g_alOn ? "true" : "false", g_alMin / 60, g_alMin % 60, g_alDays, g_alSnz, g_alVol, (int)alState, alMissed ? "true" : "false");
  return String(b);
}

// A numeric request argument, only if it is present and not empty (an empty value must not reset a setting to its minimum).
static bool numArg(const char *name, int lo, int hi, int &out) {
  if (!server.hasArg(name)) return false;
  String s = server.arg(name); s.trim();
  if (!s.length()) return false;
  out = constrain((int)s.toInt(), lo, hi);
  return true;
}

#ifdef BOARD_CYD28
static void renderStrip(int i);
#endif

static void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  // Raw copy of the screen (RGB565, big-endian) for screenshots.
  server.on("/api/frame", HTTP_GET, []() {
    const size_t total = PANEL_W * PANEL_H * 2;
    server.setContentLength(total);
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-W", String(PANEL_W));
    server.sendHeader("X-H", String(PANEL_H));
    server.send(200, "application/octet-stream", "");
#ifdef BOARD_CYD28
    for (int i = 0; i < PANEL_H / STRIP_H; i++) {
      renderStrip(i);
      server.sendContent((const char *)strip.getPointer(), PANEL_W * STRIP_H * 2);
    }
#else
    const uint8_t *p = (const uint8_t *)frame.getPointer();
    for (size_t off = 0; off < total; off += 4096) server.sendContent((const char *)p + off, min((size_t)4096, total - off));
#endif
  });
  server.on("/api/status", HTTP_GET, []() { server.sendHeader("Access-Control-Allow-Origin", "*"); server.send(200, "application/json", statusJson()); });
  server.on("/api/set", HTTP_ANY, []() {
    int v;
    if (numArg("h24", 0, 1, v)) g_h24 = v != 0;
    if (numArg("hunt", 0, 1, v)) g_hunt = v != 0;
    if (numArg("dfmt", 0, 4, v)) g_datefmt = v;
    if (numArg("alon", 0, 1, v)) { g_alOn = v != 0; alFiredDay = -1; }
    if (numArg("adays", 0, 127, v)) g_alDays = v;
    if (numArg("asnz", 1, 30, v)) g_alSnz = v;
    if (numArg("avol", 1, 100, v)) g_alVol = v;
    { String s = server.arg("atime"); s.trim(); int c = s.indexOf(':');
      if (c > 0) { int h = s.substring(0, c).toInt(), mi = s.substring(c + 1).toInt(); if (h >= 0 && h < 24 && mi >= 0 && mi < 60) { g_alMin = h * 60 + mi; alFiredDay = -1; } } }
    if (server.hasArg("tz")) {
      String z = server.arg("tz"); z.trim();
      bool ok = z.length() >= 3 && z.length() <= 47;
      for (size_t i = 0; ok && i < z.length(); i++) { char c = z[i]; ok = isalnum((unsigned char)c) || strchr(",.:/+-", c); }
      if (ok && z != g_tz) { g_tz = z; setenv("TZ", g_tz.c_str(), 1); tzset(); }
    }
    bool colorsChanged = false;
    if (numArg("inv", 0, 1, v)) { bool b = v != 0; colorsChanged |= b != g_invert; g_invert = b; }
    if (numArg("swaprb", 0, 1, v)) { bool b = v != 0; colorsChanged |= b != g_swaprb; g_swaprb = b; }
    if (colorsChanged) halApplyColors();
    saveSettings();
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/settime", HTTP_ANY, []() {
    uint32_t e = (uint32_t)server.arg("epoch").toInt();
    if (e < 1700000000UL || e > 4000000000UL) { server.send(400, "application/json", "{\"error\":\"bad time\"}"); return; }
    setClockEpoch(e);
    server.send(200, "application/json", statusJson());
  });
  server.on("/api/alarm", HTTP_ANY, []() {         // cmd=test (8 s sunrise, then ringing) or cmd=stop
    String c = server.arg("cmd");
    if (c == "test") alTest(millis()); else if (c == "stop") alStop();
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/snd", HTTP_ANY, []() {
    if (server.hasArg("on")) { g_snd = server.arg("on").toInt() != 0; prefs.putBool("snd", g_snd); }
    if (server.hasArg("vol")) { g_vol = constrain(server.arg("vol").toInt(), 0, 100); prefs.putInt("vol", g_vol); }
    halApplySound();
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/rot", HTTP_ANY, []() {
    g_rot = g_rot == 1 ? 3 : 1;
    prefs.putInt("rot", g_rot);
    halApplyRotation();
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/hunter", HTTP_ANY, []() {       // send the hunter now; o=miss|buck|tom forces the outcome (testing)
    String o = server.arg("o");
    callHunter(-1, o == "miss" ? -1 : o == "buck" ? 0 : o == "tom" ? 4 : -2);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/corn", HTTP_ANY, []() { spawnCorn(random(60, VW - 60), random(GROUND_MIN + 10, GROUND_MAX - 10)); server.send(200, "application/json", "{\"ok\":true}"); });
  server.on("/api/wifi/forget", HTTP_ANY, []() {
    wifiForgetAll();
    server.send(200, "application/json", "{\"ok\":true}");
    restartAt = millis() + 800;
  });
  server.on("/api/reboot", HTTP_ANY, []() { server.send(200, "text/plain", "rebooting"); restartAt = millis() + 500; });
  // Firmware update page at /update (ElegantOTA, the same one the fish tank and CYD-PhotoFrame use). The upload is handled
  // inside server.handleClient(), so the woods pause while it runs; progress is drawn on the screen as well.
  ElegantOTA.begin(&server);
  ElegantOTA.onStart([]() { tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE); tft.drawString("Firmware update...", 10, 10, 4); });
  ElegantOTA.onProgress([](size_t cur, size_t total) {
    static uint32_t lastDraw = 0;
    if (!total || (millis() - lastDraw < 150 && cur < total)) return;   // each repaint costs SPI time, so throttle
    lastDraw = millis();
    tft.fillRect(10, 60, PANEL_W - 20, 20, TFT_DARKGREY);
    tft.fillRect(10, 60, (int)((PANEL_W - 20) * (float)cur / total), 20, TFT_GREEN);
  });
  server.begin();
}

static void startServices() {
  g_online = true;
  WiFi.setAutoReconnect(true);
  configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");
  ArduinoOTA.setHostname(g_host.c_str());
  ArduinoOTA.onStart([]() { tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE); tft.drawString("OTA update...", 10, 10, 4); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int total) {
    tft.fillRect(10, 60, PANEL_W - 20, 20, TFT_DARKGREY);
    tft.fillRect(10, 60, (int)((PANEL_W - 20) * (float)p / total), 20, TFT_GREEN);
  });
  ArduinoOTA.begin();
  MDNS.begin(g_host.c_str());
  MDNS.addService("http", "tcp", 80);
  setupWeb();
  Serial.printf("WiFi connected, IP %s, host %s\n", WiFi.localIP().toString().c_str(), g_host.c_str());
}

// WiFi comes from the on-screen picker (TouchWifiProvisioner). BOOT skips it: the woods then run offline.
static void connectWifi() {
  static volatile bool up = false;
  wifiSetupBegin(g_host.c_str(), [](const String &) { up = true; });
  uint32_t armed = millis();
  while (!up) {
    wifiSetupLoop();
    if (millis() - armed > 1500 && digitalRead(0) == LOW) break;     // BOOT pressed: skip
    delay(5);
  }
  tft.fillScreen(TFT_BLACK);
  if (up) startServices();
}

// BOOT button: connection info for 20 s
static void drawInfo(uint32_t now) {
  cv.fillRoundRect(30, 50, 420, 190, 12, RGB(30, 22, 14));
  cv.drawRoundRect(30, 50, 420, 190, 12, RGB(214, 150, 44));
  cv.setTextDatum(TL_DATUM);
  cv.setTextColor(RGB(255, 255, 255));
  cv.drawString("Deer Hunt  v" FW_VERSION, 46, 60, FONT_BIG);
  char b[96];
  cv.setTextColor(RGB(244, 200, 84));
  snprintf(b, sizeof(b), "http://%s.local", g_host.c_str());
  cv.drawString(b, 46, 100, FONT_BIG);
  snprintf(b, sizeof(b), "http://%s", WiFi.localIP().toString().c_str());
  cv.drawString(b, 46, 132, FONT_BIG);
  cv.setTextColor(RGB(234, 223, 203));
  snprintf(b, sizeof(b), "WiFi: %s  (%d dBm)", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "not connected", WiFi.RSSI());
  cv.drawString(b, 46, 172, FONT_SMALL);
  snprintf(b, sizeof(b), "Up %lu min  %.1f fps  %s", millis() / 60000UL, g_fps, BOARD_NAME);
  cv.drawString(b, 46, 194, FONT_SMALL);
  snprintf(b, sizeof(b), "Press BOOT to close (%lu s)", (unsigned long)((infoUntil - now) / 1000));
  cv.drawString(b, 46, 216, FONT_SMALL);
}

#ifdef BOARD_CYD28
static void renderStrip(int i) {
  cv.s = &strip; cv.oy = i * STRIP_H;
  renderBackground(cv);
  drawScene(curT);
}
#endif

void setup() {
  Serial.begin(115200);
  prefs.begin("deer", false);
  g_rot = prefs.getInt("rot", DEFAULT_ROT);
  if (g_rot != 1 && g_rot != 3) g_rot = DEFAULT_ROT;
  g_host = prefs.getString("host", DEFAULT_HOST);
  g_snd = prefs.getBool("snd", true);
  g_vol = constrain(prefs.getInt("vol", 60), 0, 100);
  g_hunt = prefs.getBool("hunt", true);
  g_h24 = prefs.getBool("h24", false); g_tz = prefs.getString("tz", g_tz); g_datefmt = constrain(prefs.getInt("dfmt", 0), 0, 4);
  g_alOn = prefs.getBool("alon", false); g_alMin = constrain(prefs.getInt("almin", 7 * 60), 0, 1439); g_alDays = prefs.getInt("aldays", 0x7F) & 0x7F;
  g_alSnz = constrain(prefs.getInt("alsnz", 9), 1, 30); g_alVol = constrain(prefs.getInt("alvol", 70), 1, 100);
  setenv("TZ", g_tz.c_str(), 1); tzset();
  { uint32_t lt = prefs.getUInt("lastt", 0);                    // restore the last saved time (no battery clock)
    if (lt > 1700000000UL) { struct timeval tv = {(time_t)lt + 5, 0}; settimeofday(&tv, nullptr); } }
  g_invert = prefs.getBool("invert", false);
  g_swaprb = prefs.getBool("swaprb", DEFAULT_SWAPRB);
  pinMode(0, INPUT_PULLUP);        // BOOT button
  halAudioStart();                 // before the touch controller: the Freenove shares I2C with its codec
  halDisplayBegin();
  halTouchBegin();

  cv.sx = PANEL_W / (float)VW;
  cv.sy = PANEL_H / (float)VH;
#ifdef BOARD_FREENOVE_S3
  frame.setColorDepth(16);
  bg.setColorDepth(16);
  void *fp = frame.createSprite(PANEL_W, PANEL_H);
  void *bp = bg.createSprite(PANEL_W, PANEL_H);
  if (!fp || !bp) {
    tft.setTextColor(TFT_RED);
    tft.drawString("Sprite alloc failed (PSRAM?)", 10, 10, 4);
    while (true) delay(1000);
  }
  Canvas bgc; bgc.s = &bg; bgc.sx = cv.sx; bgc.sy = cv.sy;
  renderBackground(bgc);
#else
  strip.setColorDepth(16);
  if (!strip.createSprite(PANEL_W, STRIP_H)) {
    tft.setTextColor(TFT_RED);
    tft.drawString("Out of memory", 10, 10, 4);
    while (true) delay(1000);
  }
#endif
  randomSeed(esp_random());
  static const uint16_t leafCols[4] = {RGB(204, 100, 34), RGB(226, 160, 48), RGB(168, 64, 30), RGB(190, 130, 40)};
  for (auto &l : leaves) l = {(float)random(VW), (float)random(VH), 12.0f + random(14), (float)random(-4, 8), random(628) / 100.0f, leafCols[random(4)]};
  for (auto &c : corn) c.on = false;
  initClockFallback();
  connectWifi();
  hunter.next = millis() + 90000;               // the first visit about a minute and a half after start
}

void loop() {
  static uint32_t last = millis(), lastTouch = 0, lastCorn = 0, fpsT = millis();
  static int frames = 0;
  uint32_t now = millis();
  float dt = (now - last) / 1000.0f; last = now;
  if (dt > 0.1f) dt = 0.1f;
  float t = now / 1000.0f;

  if (g_online) {
    ArduinoOTA.handle();
    server.handleClient();
    ElegantOTA.loop();
    static uint32_t lastWifi = 0;
    if (WiFi.status() != WL_CONNECTED && now - lastWifi > 30000) { lastWifi = now; WiFi.reconnect(); }
  }
  if (restartAt && millis() > restartAt) ESP.restart();
  static uint32_t lastSave = 0;
  static bool savedAfterNtp = false;
  if (!savedAfterNtp && ntpSynced()) { savedAfterNtp = true; saveTimeNow(); }
  if (now - lastSave > 6UL * 3600000UL) { lastSave = now; saveTimeNow(); }

  static bool lastBtn = true;
  bool btn = digitalRead(0);
  if (!btn && lastBtn) { if (alActive()) alStop(); else infoUntil = (infoUntil > now) ? 0 : now + 20000; }   // BOOT: stop the alarm, else connection info
  lastBtn = btn;

  int px, py;
  const bool touched = halTouch(px, py);
  static bool wasTouched = false;
  static uint32_t pressAt = 0;
  const bool press = touched && !wasTouched;
  if (press) pressAt = now;
  wasTouched = touched;
  const bool rep = touched && !press && now - pressAt > 450;
  if (touched && (press || now - lastTouch > 120)) {
    lastTouch = now;
    const float wx = px / cv.sx, wy = max(py / cv.sy, 14.0f);
    if (alState == AL_RING) {
      if (press && wy >= BTN_Y && wy < BTN_Y + BTN_H) {
        if (wx >= BTN_SNZ_X && wx < BTN_SNZ_X + BTN_W) alSnooze(now);
        else if (wx >= BTN_STOP_X && wx < BTN_STOP_X + BTN_W) alStop();
      }
    } else if (alPanel) {
      alPanelTouch(wx, wy, press, rep, now);
    } else if (wx > VW - 190 && wy < 72) {
      if (press) { alPanel = true; alPanelAt = now; }                // tap the time to set the alarm
    } else if (g_hunt && press && hunter.st == H_OFF && wy > 286 && (wx < 76 || wx > 392)) {
      callHunter(wx < 76 ? 0 : 1);                                   // tap a rock to call the hunter (he waits for the animals)
    } else if (press || now - lastCorn > 350) {                      // touch scatters corn
      lastCorn = now;
      spawnCorn(wx, max(wy, (float)GROUND_MIN));
    }
  }

  updateAlarm(now);
  updateWorld(dt, t);
  curT = t;

#ifdef BOARD_FREENOVE_S3
  cv.s = &frame; cv.oy = 0;
  memcpy(frame.getPointer(), bg.getPointer(), PANEL_W * PANEL_H * 2);
  drawScene(t);
  frame.pushSprite(0, 0);
#else
  for (int i = 0; i < PANEL_H / STRIP_H; i++) {
    renderStrip(i);
    strip.pushSprite(0, i * STRIP_H);
  }
#endif

  frames++;
  if (now - fpsT >= 5000) {
    g_fps = frames * 1000.0f / (now - fpsT);
    Serial.printf("fps: %.1f  heap %u\n", g_fps, (unsigned)ESP.getFreeHeap());
    frames = 0; fpsT = now;
  }
}
