// Woods ambience: a 25 s loop (design/make_ambience.py) played forever, with volume / mute applied, plus the alarm chime.
// Freenove: 16 kHz mono mixed by audioMix. CYD: a 11.025 kHz mono WAV (smaller, so it fits the flash) streamed by
// audioBubbles44 at 44.1 kHz, linearly interpolated x4 (its 8-bit DAC chirps at 16 kHz). The loop's seam is already
// crossfaded. The alarm chime is synthesised in audioMix on both boards and ignores the mute.
#include "audio.h"
#include "board.h"
#include "hal.h"
#include <string.h>

#ifdef BOARD_CYD28
extern const uint8_t amb_start[] asm("_binary_src_ambience_wav_start");
extern const uint8_t amb_end[] asm("_binary_src_ambience_wav_end");
#else
extern const uint8_t amb_start[] asm("_binary_src_ambience_raw_start");
extern const uint8_t amb_end[] asm("_binary_src_ambience_raw_end");
#endif

// Sine by table lookup (x in cycles): sinf per sample was eating the CYD's core and the DAC output crackled.
static int16_t g_sinT[512];
static float sinC(float x) {
  if (!g_sinT[1]) for (int i = 0; i < 512; i++) g_sinT[i] = (int16_t)(sinf(6.2831853f * i / 512.0f) * 32000.0f);
  x -= (int)x; if (x < 0) x += 1.0f;
  return g_sinT[(int)(x * 512.0f) & 511] * (1.0f / 32000.0f);
}

void audioDing() {}
void audioCash() {}
void audioLamb() {}
void audioBubMode(int, int) {}

static volatile int g_alLevel = 0;
void audioAlarm(int level) { g_alLevel = level < 0 ? 0 : level > 100 ? 100 : level; }
bool audioAlarmOn() { return g_alLevel > 0; }

// Two rising bell arpeggios every 1.6 s.
static int32_t alarmSample(uint32_t n) {
  const float t = (n % 25600) / 16000.0f;
  static const float F[4] = {1047, 1319, 1568, 2093};
  float v = 0;
  for (int rep = 0; rep < 2; rep++)
    for (int k = 0; k < 4; k++) {
      const float u = t - rep * 0.8f - k * 0.13f;
      if (u <= 0 || u > 0.45f) continue;
      v += expf(-u * 7.0f) * min(1.0f, u / 0.004f) * sinC(F[k] * u);
    }
  return (int32_t)(v * 9000.0f);
}

// Mix the chime into out (16 kHz). It ignores mute and the volume setting: it has its own loudness.
static void addAlarm(int16_t *out, int n) {
  static uint32_t alPos = 0;
  const int lvl = g_alLevel;
  if (lvl == 0) { alPos = 0; return; }
  for (int i = 0; i < n; i++) {
    int32_t s = out[i] + (alarmSample(alPos++) * lvl) / 100;
    out[i] = (int16_t)constrain(s, -32768, 32767);
  }
}

// A rifle shot, synthesised: a noise crack, a low boom, then two fading echoes off the hills (16 kHz).
static volatile uint32_t g_shotPos = 0xFFFFFFFF;
void audioShot() { g_shotPos = 0; }
static int32_t shotSample(uint32_t n) {
  const float t = n / 16000.0f;
  static uint32_t rnd = 777;
  rnd = rnd * 1664525u + 1013904223u;
  const float noise = ((int32_t)(rnd >> 16) - 32768) / 32768.0f;
  float v = expf(-t * 40.0f) * noise * 0.9f                 // the crack
          + expf(-t * 11.0f) * 0.7f * sinC(70.0f * t);      // the boom
  for (int k = 1; k <= 2; k++) {                            // echoes
    const float u = t - 0.38f * k;
    if (u > 0) v += expf(-u * 14.0f) * noise * (0.3f / k) + expf(-u * 7.0f) * 0.25f * sinC(60.0f * u) / k;
  }
  return (int32_t)(v * 20000.0f);
}
static void addShot(int16_t *out, int n) {
  uint32_t p = g_shotPos;
  if (p == 0xFFFFFFFF) return;
  for (int i = 0; i < n; i++) {
    if (p >= 16000 * 1.6f) { g_shotPos = 0xFFFFFFFF; return; }
    if (g_snd && !g_asleep && !g_isNight) out[i] = (int16_t)constrain(out[i] + (shotSample(p) * (int32_t)g_vol) / 60, -32768, 32767);
    p++;
  }
  g_shotPos = p;
}

#ifdef BOARD_CYD28
void audioBubbles44(int16_t *out, int n) {
  static const uint8_t *pcm = nullptr; static size_t len = 0, pos = 0;
  if (!pcm) {                                                  // find the WAV's data chunk
    const uint8_t *p = amb_start + 12;                         // skip "RIFF....WAVE"
    while (p + 8 <= amb_end) {
      const uint32_t sz = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
      if (!memcmp(p, "data", 4)) { pcm = p + 8; len = min((size_t)sz, (size_t)(amb_end - pcm)) / 2; break; }
      p += 8 + sz + (sz & 1);
    }
    if (!pcm) len = 0;
  }
  const bool on = len > 0 && g_snd && !g_asleep && !g_isNight;
  static int ph = 0;                                           // 11.025 kHz source -> 4 output samples per source sample
  for (int i = 0; i < n; i++) {
    int32_t x = 0;
    if (on) {
      int16_t a, b;
      memcpy(&a, pcm + pos * 2, 2);
      memcpy(&b, pcm + (pos + 1 >= len ? 0 : pos + 1) * 2, 2);
      x = a + (((int32_t)b - a) * ph) / 4;
      if (++ph >= 4) { ph = 0; if (++pos >= len) pos = 0; }
    }
    out[i] = (int16_t)((x * g_vol) / 60);
  }
}

void audioMix(int16_t *out, int n) {                           // the ambience is the separate 44.1 kHz stream; this is the chime only
  memset(out, 0, n * sizeof(int16_t));
  addAlarm(out, n);
  addShot(out, n);
}
#else
void audioMix(int16_t *out, int n) {
  static size_t idx = 0;
  const int16_t *pcm = (const int16_t *)amb_start;
  const size_t len = (amb_end - amb_start) / 2;
  const bool on = len > 0 && g_snd && !g_asleep && !g_isNight;
  for (int i = 0; i < n; i++) {
    int32_t s = on ? (pcm[idx] * (int32_t)g_vol) / 60 : 0;
    out[i] = (int16_t)constrain(s, -32768, 32767);
    if (on && ++idx >= len) idx = 0;
  }
  addAlarm(out, n);
  addShot(out, n);
}
#endif
