"""PC-side preview of the Deer Hunt scene: a line-for-line port of src/main.cpp (scenery, animals, behaviour).
Renders 480x320 frames with RGB565 colours like the panel, no anti-aliasing.
  python design/preview.py            -> design/woods_*.png, design/woods.mp4 (needs ffmpeg)
"""
import math, os, random, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

VW, VH = 480, 320
HORIZON, GROUND_MIN, GROUND_MAX = 180, 224, 306
OUT = os.path.dirname(os.path.abspath(__file__))


def RGB(r, g, b):
    r, g, b = int(r) & 0xF8, int(g) & 0xFC, int(b) & 0xF8
    return (r | r >> 5, g | g >> 6, b | b >> 5)


def mix(r0, g0, b0, r1, g1, b1, t):
    return RGB(r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t)


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


class Canvas:
    def __init__(self):
        self.im = Image.new("RGB", (VW, VH))
        self.d = ImageDraw.Draw(self.im)

    def fillRect(self, x, y, w, h, c):
        self.d.rectangle([round(x), round(y), round(x) + max(1, round(w)) - 1, round(y) + max(1, round(h)) - 1], fill=c)

    def fillRoundRect(self, x, y, w, h, r, c):
        self.d.rounded_rectangle([round(x), round(y), round(x + w) - 1, round(y + h) - 1], radius=r, fill=c)

    def fillEllipse(self, x, y, rx, ry, c):
        rx, ry = max(1, round(rx)), max(1, round(ry))
        self.d.ellipse([round(x) - rx, round(y) - ry, round(x) + rx, round(y) + ry], fill=c)

    def fillCircle(self, x, y, r, c):
        r = max(1, round(r))
        self.d.ellipse([round(x) - r, round(y) - r, round(x) + r, round(y) + r], fill=c)

    def fillTriangle(self, x0, y0, x1, y1, x2, y2, c):
        self.d.polygon([(round(x0), round(y0)), (round(x1), round(y1)), (round(x2), round(y2))], fill=c)

    def drawLine(self, x0, y0, x1, y1, c):
        self.d.line([round(x0), round(y0), round(x1), round(y1)], fill=c)

    def drawWideLine(self, x0, y0, x1, y1, w, c, c2):
        w = max(1, round(w))
        self.d.line([x0, y0, x1, y1], fill=c, width=w)
        if w > 2:
            r = w / 2
            for x, y in ((x0, y0), (x1, y1)):
                self.d.ellipse([x - r, y - r, x + r, y + r], fill=c)

    def drawPixel(self, x, y, c):
        self.d.point((round(x), round(y)), fill=c)

    def drawString(self, text, x, y, c, size=22):
        try:
            f = ImageFont.truetype("arial.ttf", size)
        except OSError:
            f = ImageFont.load_default()
        self.d.text((round(x), round(y)), text, fill=c, font=f, anchor="mm")

    def hline(self, y, c):
        self.d.line([0, y, VW - 1, y], fill=c)


cv = Canvas()


def scale_at(y):
    return 0.62 + 0.55 * (y - GROUND_MIN) / (GROUND_MAX - GROUND_MIN)


# ------------------------------------------------------------ animals
BUCK, DOE, FAWN, TOM, HEN = range(5)
WALK, GRAZE, LOOK, EAT, FLEE, HIDDEN, DOWN = range(7)


class Critter:
    def __init__(self, k, x, y, d, spd, ph, m, timer):
        self.k, self.x, self.y, self.ty, self.dir, self.spd, self.ph = k, x, y, y, d, spd, ph
        self.head = self.fan = 0.0
        self.m, self.timer, self.corn = m, timer, -1


def make_world():
    return [
        Critter(BUCK, 120, 262, 1, 22, 0.0, GRAZE, 2),
        Critter(DOE, 330, 246, -1, 20, 1.0, WALK, 4),
        Critter(FAWN, 300, 252, -1, 20, 2.0, WALK, 4),
        Critter(DOE, 420, 232, -1, 17, 3.0, LOOK, 3),
        Critter(TOM, 210, 290, 1, 19, 0.5, WALK, 5),
        Critter(HEN, 400, 280, -1, 16, 1.5, GRAZE, 3),
        Critter(HEN, 70, 238, 1, 15, 2.5, WALK, 6),
    ]


FAWN_MOTHER = 1
crit = make_world()
NCORN = 24
corn = [dict(x=0, y=0, z=0, life=0, on=False, by=-1) for _ in range(NCORN)]
LEAF_COLS = [RGB(204, 100, 34), RGB(226, 160, 48), RGB(168, 64, 30), RGB(190, 130, 40)]
leaves = []


def spawn_corn(wx, wy):
    cx, cy = clamp(wx, 24, VW - 24), clamp(wy, GROUND_MIN + 6, GROUND_MAX - 2)
    for _ in range(10):
        for c in corn:
            if c["on"]:
                continue
            dx = (random.randint(-13, 13) + random.randint(-13, 13)) * 0.5
            c["x"], c["y"] = cx + dx, cy + random.randint(-3, 3)
            c["z"] = random.random() * 4.5 * (1 - abs(dx) / 13)
            c["life"], c["on"], c["by"] = 45, True, -1
            break


def pick_next(c):
    r = random.randrange(100)
    if r < 50:
        c.m = WALK
        c.timer = 3.0 + random.randrange(50) / 10
        c.dir = 1 if c.x < 90 else -1 if c.x > VW - 90 else random.choice((1, -1))
        c.ty = random.randint(GROUND_MIN, GROUND_MAX - 1)
    elif r < 80:
        c.m, c.timer = GRAZE, 2.0 + random.randrange(30) / 10
    else:
        c.m, c.timer = LOOK, 1.5 + random.randrange(20) / 10


def update_critter(i, dt):
    c = crit[i]
    if c.m == DOWN:                       # lying where it fell; the hunter moves it
        return
    if c.m == HIDDEN:                     # out of sight until the timer runs out, then back in from the edge it left by
        c.timer -= dt
        if c.timer <= 0:
            c.x = VW + 50 if c.dir > 0 else -50
            c.dir = -c.dir
            c.y = random.randint(GROUND_MIN, GROUND_MAX - 1)
            c.ty, c.m, c.timer, c.head, c.fan = c.y, WALK, 6, 0.0, 0.0
        return
    if c.m == FLEE:                       # bolting away from the hunter
        if c.timer > 0:
            c.timer -= dt
            c.head += (0.0 - c.head) * min(1.0, dt * 6)
            return
        fv = c.spd * 6.0 * scale_at(c.y)
        c.x += c.dir * fv * dt
        c.ph += fv * dt * 0.16
        c.head = 0
        if c.x < -70 or c.x > VW + 70:
            c.m, c.timer = HIDDEN, 8 + random.randrange(8)
        return
    if hunter_alert():                    # the hunter takes aim: everyone stops and looks up, the tom fans his tail
        if c.corn >= 0:
            corn[c.corn]["by"] = -1
            c.corn = -1
        c.m, c.timer = LOOK, 2.0
        c.head += (0.0 - c.head) * min(1.0, dt * 4)
        c.fan += ((1.0 if c.k == TOM else 0.0) - c.fan) * min(1.0, dt * 2.5)
        return
    sc = scale_at(c.y)
    c.timer -= dt
    if c.corn >= 0 and not corn[c.corn]["on"]:
        c.corn = -1
        if c.m in (WALK, EAT):
            pick_next(c)
    if c.corn < 0 and c.k != FAWN:
        best, bd = -1, 230
        for n, f in enumerate(corn):
            if not f["on"] or f["by"] >= 0:
                continue
            d = abs(f["x"] - c.x) + abs(f["y"] - c.y)
            if d < bd:
                bd, best = d, n
        if best >= 0:
            c.corn = best
            corn[best]["by"] = i
            c.m = WALK
    v = 0.0
    if c.corn >= 0 and c.m != EAT:
        f = corn[c.corn]
        dx, dy = f["x"] - c.x, f["y"] - c.y
        reach = (38.0 if c.k <= FAWN else 22.0) * (0.92 if c.k == DOE else 0.85 if c.k == HEN else 0.62 if c.k == FAWN else 1.0)
        if abs(dx) > reach + 3:
            c.dir = 1 if dx >= 0 else -1
            v = c.spd * 1.5 * sc
            c.x += c.dir * v * dt
        c.y += clamp(dy, -1, 1) * 20 * dt
        if abs(dx) <= reach + 3 and abs(dy) < 6:
            c.m, c.timer, v = EAT, 2.5 + random.randrange(20) / 10, 0
    elif c.m == EAT:
        if c.timer <= 0:
            if c.corn >= 0:
                corn[c.corn]["on"] = False
            c.corn, c.m, c.timer = -1, GRAZE, 1.0
    elif c.k == FAWN:
        d = crit[FAWN_MOTHER]
        tx = d.x - d.dir * 34
        dx = tx - c.x
        if d.m == HIDDEN:
            c.m = GRAZE
        elif abs(dx) > 12:
            c.m, c.dir = WALK, 1 if dx >= 0 else -1
            v = c.spd * 1.2 * sc
            c.x += c.dir * v * dt
        else:
            c.m = GRAZE if d.m == WALK else d.m
            c.dir = d.dir
        c.y += clamp(d.y + 6 - c.y, -1, 1) * 12 * dt
    elif c.m == WALK:
        v = c.spd * sc
        c.x += c.dir * v * dt
        c.y += (c.ty - c.y) * 0.3 * dt
        if c.x < -45:
            c.dir = 1
        if c.x > VW + 45:
            c.dir = -1
        if c.timer <= 0:
            pick_next(c)
    elif c.timer <= 0:
        pick_next(c)
    c.y = clamp(c.y, GROUND_MIN, GROUND_MAX)
    c.ph += v * dt * 0.16
    down = c.m in (GRAZE, EAT)
    c.head += ((1.0 if down else 0.0) - c.head) * min(1.0, dt * 4)
    c.fan += ((1.0 if (c.m == LOOK and c.k == TOM) else 0.0) - c.fan) * min(1.0, dt * 2.5)


def update_world(dt, t):
    update_hunter(dt)
    for i in range(len(crit)):
        update_critter(i, dt)
    for c in corn:
        if c["on"]:
            c["life"] -= dt
            if c["life"] <= 0:
                c["on"] = False
    for l in leaves:
        l["y"] += l["vy"] * dt
        l["x"] += (l["vx"] + 9 * math.sin(t * 1.3 + l["ph"])) * dt
        if l["y"] > VH + 6 or l["x"] < -8 or l["x"] > VW + 8:
            l["y"], l["x"] = -6, random.randrange(VW)


# ------------------------------------------------------------ the hunter (behaviour; mirrors src/main.cpp)
H_OFF, H_RISE, H_AIM, H_SHOT, H_LOWER, H_RUN, H_DRAG = range(7)


class Hunter:
    st, t, since, side, x, y, sx, sy, dir, tgt, drag_dir, hit, force = H_OFF, 0.0, 99.0, 0, 46.0, 314.0, 0.0, 0.0, 1, -1, 1, False, -2


hunter = Hunter()
HUNT_X, HUNT_DIR = (46, 428), (1, -1)


def hunter_alert():
    return hunter.st == H_AIM or (hunter.st == H_SHOT and hunter.t < 0.12)


def start_hunter(side=-1, force=-2):
    if hunter.st != H_OFF:
        return
    hunter.side = random.randrange(2) if side < 0 else side
    hunter.x, hunter.y, hunter.dir = HUNT_X[hunter.side], 314.0, HUNT_DIR[hunter.side]
    hunter.tgt, hunter.hit, hunter.force, hunter.t, hunter.since = -1, False, force, 0.0, 99.0
    hunter.st = H_RISE


def on_screen(c):
    return c.m not in (HIDDEN, DOWN) and 12 < c.x < VW - 12


def hunter_fires():
    hs = 1.3 * scale_at(314)
    hunter.sx, hunter.sy, hunter.since = hunter.x + hunter.dir * 33 * hs, hunter.y - 32.5 * hs, 0.0
    roll = random.randrange(100)
    want = hunter.force if hunter.force != -2 else (-1 if roll < 40 else 0 if roll < 70 else 4)
    if want >= 0 and not on_screen(crit[want]):
        want = 4 if (want == 0 and on_screen(crit[4])) else 0 if (want == 4 and on_screen(crit[0])) else -1
    hunter.tgt, hunter.hit = want, want >= 0
    for i, c in enumerate(crit):
        if c.corn >= 0:
            corn[c.corn]["by"] = -1
            c.corn = -1
        if c.m == HIDDEN:
            continue
        if i == want:
            c.m, c.head = DOWN, 1.0
            continue
        c.m, c.dir, c.timer = FLEE, (1 if c.x >= hunter.x else -1), random.randrange(0, 45) / 100


def update_hunter(dt):
    h = hunter
    if h.st == H_OFF:
        return
    h.t += dt
    h.since += dt
    if h.st == H_RISE and h.t >= 1.3:
        h.st, h.t = H_AIM, 0.0
    elif h.st == H_AIM and h.t >= 1.6:
        h.st, h.t = H_SHOT, 0.0
        hunter_fires()
    elif h.st == H_SHOT and h.t >= 1.1:
        h.t = 0.0
        if h.hit:
            h.st = H_RUN
            h.drag_dir = -1 if crit[h.tgt].x < VW / 2 else 1
        else:
            h.st = H_LOWER
    elif h.st == H_LOWER and h.t >= 1.0:
        h.st, h.tgt = H_OFF, -1
    elif h.st == H_RUN:
        a = crit[h.tgt]
        gx = a.x + h.drag_dir * (36 if a.k <= FAWN else 22) * scale_at(a.y)
        dx, dy = gx - h.x, (a.y + 2) - h.y
        h.dir = 1 if dx >= 0 else -1
        if abs(dx) > 3:
            h.x += h.dir * min(95.0 * dt, abs(dx))
        h.y += clamp(dy, -1, 1) * 70 * dt
        if abs(dx) <= 3 and abs(dy) < 3:
            h.st, h.t, h.dir = H_DRAG, 0.0, h.drag_dir
    elif h.st == H_DRAG:
        a = crit[h.tgt]
        h.x += h.drag_dir * 34 * dt
        a.x = h.x - h.drag_dir * (36 if a.k <= FAWN else 22) * scale_at(a.y)
        a.y = h.y - 2
        if h.x < -60 or h.x > VW + 60:
            a.m, a.timer, a.dir = HIDDEN, 20 + random.randrange(15), h.drag_dir
            h.st, h.tgt = H_OFF, -1


# ------------------------------------------------------------ drawing
class Pose:
    def __init__(self, x, y, s, d):
        self.x, self.y, self.s, self.d = x, y, s, d

    def px(self, a):
        return self.x + self.d * a * self.s

    def py(self, a):
        return self.y + a * self.s


def leg(p, hx, hy, sw, lift, w, col, hoof):
    kx, ky = hx + sw * 0.35, hy + 13
    fx, fy = hx + sw, -lift
    cv.drawWideLine(p.px(hx), p.py(hy), p.px(kx), p.py(ky), w * p.s, col, col)
    cv.drawWideLine(p.px(kx), p.py(ky), p.px(fx), p.py(fy), w * p.s * 0.8, col, col)
    cv.fillCircle(p.px(fx), p.py(fy), 1.5 * p.s, hoof)


def draw_deer(c, t):
    buck, fawn = c.k == BUCK, c.k == FAWN
    p = Pose(c.x, c.y, scale_at(c.y) * (0.62 if fawn else 1.0 if buck else 0.92), c.dir)
    body = RGB(138, 90, 50) if buck else RGB(172, 118, 70) if fawn else RGB(160, 112, 68)
    dark = RGB(100, 64, 36) if buck else RGB(135, 90, 52) if fawn else RGB(122, 84, 50)
    belly, hoof = RGB(222, 200, 160), RGB(40, 30, 24)
    moving = c.m == WALK or (c.corn >= 0 and c.m != EAT)
    legs = [(-12, 3.14, True), (10, 0, True), (-17, 0, False), (15, 3.14, False)]

    def draw_leg(hx, off, far):
        sw = math.sin(c.ph + off) * 8 if moving else 0
        lift = max(0.0, math.cos(c.ph + off)) * 4 if moving else 0
        leg(p, hx, -26, sw, lift, 3.2, dark if far else body, hoof)

    for l in legs:
        if l[2]:
            draw_leg(*l)
    cv.fillEllipse(p.px(0), p.py(-34), 25 * p.s, 11 * p.s, body)
    cv.fillEllipse(p.px(12), p.py(-35), 14 * p.s, 11 * p.s, body)
    cv.fillEllipse(p.px(-14), p.py(-34), 13 * p.s, 11 * p.s, body)
    cv.fillEllipse(p.px(0), p.py(-27), 21 * p.s, 4 * p.s, belly)
    tw = math.sin(t * 14) * 3 if c.m == LOOK else 0
    cv.fillTriangle(p.px(-24), p.py(-43 + tw), p.px(-33), p.py(-39 + tw), p.px(-25), p.py(-31), body)
    cv.fillTriangle(p.px(-26), p.py(-38 + tw), p.px(-32), p.py(-37 + tw), p.px(-26), p.py(-32), RGB(240, 235, 225))
    if fawn:
        for sx, sy in ((-14, -40), (-6, -42), (2, -41), (10, -40), (-10, -35), (-2, -36), (6, -36), (14, -37)):
            cv.fillCircle(p.px(sx), p.py(sy), 1.3 * p.s, RGB(245, 235, 210))
    for l in legs:
        if not l[2]:
            draw_leg(*l)
    bob = math.sin(c.ph * 2) * 1.2 if moving else 0
    hx = 36 + (40 - 36) * c.head
    hy = -62 + (-9 + 62) * c.head + bob
    cv.drawWideLine(p.px(17), p.py(-40), p.px(hx - 4), p.py(hy + 1), 9 * p.s, body, body)
    ex = math.sin(t * 11) * 2 if c.m == LOOK else 0
    cv.fillTriangle(p.px(hx - 4), p.py(hy - 3), p.px(hx - 11 + ex), p.py(hy - 10), p.px(hx - 1), p.py(hy - 8), dark)
    cv.fillCircle(p.px(hx), p.py(hy), 5.5 * p.s, body)
    mx, my = hx + 6 - 4 * c.head, hy + 1 + 4 * c.head
    cv.fillEllipse(p.px(mx), p.py(my), 4.5 * p.s, 3.2 * p.s, RGB(196, 166, 126))
    cv.fillCircle(p.px(mx + 3.5), p.py(my - 0.5), 1.4 * p.s, RGB(25, 20, 18))
    cv.fillCircle(p.px(hx + 1.5), p.py(hy - 2), 1.2 * p.s, RGB(15, 12, 10))
    if buck:
        for pas in range(2):
            o = 3.5 if pas else 0
            a = RGB(168, 148, 108) if pas else RGB(214, 194, 150)
            bx, by = hx - 2 + o, hy - 4
            b1x, b1y, b2x, b2y = hx - 6 + o, hy - 14, hx + 2 + o, hy - 25
            w = 1.7 * p.s
            for (x0, y0, x1, y1) in ((bx, by, b1x, b1y), (b1x, b1y, b2x, b2y), (b1x, b1y, b1x - 8, b1y - 4),
                                     (b2x, b2y, b2x + 1, b2y - 7), (b2x, b2y, b2x + 7, b2y - 3)):
                cv.drawWideLine(p.px(x0), p.py(y0), p.px(x1), p.py(y1), w, a, a)


def draw_turkey(c, t):
    tom = c.k == TOM
    p = Pose(c.x, c.y, scale_at(c.y) * (1.0 if tom else 0.85), c.dir)
    body = RGB(86, 56, 36) if tom else RGB(124, 94, 64)
    wing = RGB(62, 40, 28) if tom else RGB(98, 72, 48)
    legc = RGB(205, 150, 110)
    moving = c.m == WALK or (c.corn >= 0 and c.m != EAT)
    if tom:
        op = 0.35 + 0.65 * c.fan
        N, a0, a1, R = 9, -0.45, 1.55, 30 * op + 8
        prev = (0, 0)
        for i in range(N + 1):
            a = a0 + (a1 - a0) * i / N
            ex, ey = -12 - R * math.sin(a), -22 - R * math.cos(a) * 0.9
            if i > 0:
                col = RGB(158, 88, 40) if i & 1 else RGB(206, 168, 108)
                cv.fillTriangle(p.px(-12), p.py(-22), p.px(prev[0]), p.py(prev[1]), p.px(ex), p.py(ey), col)
            prev = (ex, ey)
        cv.drawLine(p.px(-12), p.py(-22), p.px(prev[0]), p.py(prev[1]), RGB(120, 70, 36))
    else:
        cv.fillTriangle(p.px(-11), p.py(-22), p.px(-25), p.py(-26), p.px(-22), p.py(-11), wing)
    for n in range(2):
        off = 3.14 if n else 0
        sw = math.sin(c.ph * 1.2 + off) * 6 if moving else 0
        lift = max(0.0, math.cos(c.ph * 1.2 + off)) * 3 if moving else 0
        lc = legc if n else RGB(165, 118, 84)
        hx = 3 if n else -2
        cv.drawWideLine(p.px(hx), p.py(-9), p.px(hx + sw), p.py(-lift), 2.2 * p.s, lc, lc)
        cv.drawLine(p.px(hx + sw), p.py(-lift), p.px(hx + sw + 4), p.py(-lift), lc)
        cv.drawLine(p.px(hx + sw), p.py(-lift), p.px(hx + sw - 3), p.py(-lift + 1), lc)
    cv.fillEllipse(p.px(0), p.py(-18), 15 * p.s, 10 * p.s, body)
    cv.fillEllipse(p.px(7), p.py(-17), 9 * p.s, 9 * p.s, body)
    cv.fillEllipse(p.px(-3), p.py(-19), 11 * p.s, 6.5 * p.s, wing)
    for i in range(4):
        cv.drawLine(p.px(-9 + i * 4), p.py(-23), p.px(-10 + i * 4), p.py(-15), RGB(120, 84, 52) if tom else RGB(150, 118, 84))
    if tom:
        cv.drawLine(p.px(10), p.py(-17), p.px(11), p.py(-9), RGB(20, 14, 12))
    bob = math.sin(c.ph * 2.4) * 1.5 if moving else 0
    peck = (math.sin(t * 11) * 0.5 + 0.5) * 0.12 if c.m == EAT else 0
    h = min(1.0, c.head + peck)
    hx, hy = 17 + 6 * h, -37 + 35 * h + bob
    neck = RGB(120, 150, 190) if tom else RGB(168, 140, 118)
    cv.drawWideLine(p.px(10), p.py(-23), p.px(hx), p.py(hy), 4.2 * p.s, neck, neck)
    cv.fillCircle(p.px(hx), p.py(hy), 3.2 * p.s, neck)
    cv.fillTriangle(p.px(hx + 2), p.py(hy - 1.2), p.px(hx + 7), p.py(hy + 0.6), p.px(hx + 2), p.py(hy + 1.8), RGB(226, 196, 100))
    if tom:
        cv.fillEllipse(p.px(hx + 3), p.py(hy + 3.4), 1.6 * p.s, 3 * p.s, RGB(200, 40, 40))
        cv.fillEllipse(p.px(hx + 3.5), p.py(hy - 2.6), 1.3 * p.s, 1.8 * p.s, RGB(205, 50, 50))
    cv.fillCircle(p.px(hx + 0.8), p.py(hy - 0.8), 0.9 * p.s, RGB(15, 12, 10))


def draw_shadow(c):
    s = scale_at(c.y)
    rx = (17 if c.k == FAWN else 29) if c.k <= FAWN else (22 if c.k == TOM else 17)
    cv.fillEllipse(c.x, c.y + 1, rx * s, 3.6 * s, RGB(52, 44, 28))


def draw_trunk_front(x, w):
    cv.fillRect(x, 0, w, VH, RGB(44, 32, 24))
    cv.fillRect(x + w * 0.65, 0, w * 0.35, VH, RGB(30, 22, 17))
    for i in range(6):
        lx = x + w * (0.12 + 0.11 * i)
        for y in range(0, VH, 40):
            cv.drawLine(lx, y + 7 * (i & 1), lx + 1.5, y + 36 + 7 * (i & 1), RGB(66, 50, 36))


ROCKS = (
    (40, 314, 34, 22, 32, 305, 16, 8, RGB(92, 90, 94), RGB(128, 126, 130)),
    (434, 316, 32, 21, 426, 308, 14, 7, RGB(86, 84, 90), RGB(120, 118, 124)),
)


def draw_rock(i):
    cx, cy, rx, ry, hx, hy, hrx, hry, col, hcol = ROCKS[i]
    cv.fillEllipse(cx, cy, rx, ry, col)
    cv.fillEllipse(hx, hy, hrx, hry, hcol)


def crit_scale(c):
    return scale_at(c.y) * (0.62 if c.k == FAWN else 1.0 if c.k == BUCK else 0.92 if c.k == DOE else 1.0 if c.k == TOM else 0.85)


def draw_down(c):
    p = Pose(c.x, c.y, crit_scale(c), c.dir)
    X = RGB(25, 20, 18)
    if c.k <= FAWN:
        buck, fawn = c.k == BUCK, c.k == FAWN
        body = RGB(138, 90, 50) if buck else RGB(172, 118, 70) if fawn else RGB(160, 112, 68)
        dark = RGB(100, 64, 36) if buck else RGB(135, 90, 52) if fawn else RGB(122, 84, 50)
        for l in ((-14, -14, -19, -31), (-7, -15, -11, -32), (8, -15, 13, -32), (15, -14, 20, -31)):
            cv.drawWideLine(p.px(l[0]), p.py(l[1]), p.px(l[2]), p.py(l[3]), 3.0 * p.s, dark, dark)
            cv.fillCircle(p.px(l[2]), p.py(l[3]), 1.6 * p.s, RGB(40, 30, 24))
        cv.fillEllipse(p.px(0), p.py(-10), 26 * p.s, 9 * p.s, body)
        cv.fillEllipse(p.px(0), p.py(-13), 20 * p.s, 4 * p.s, RGB(222, 200, 160))
        cv.fillTriangle(p.px(-25), p.py(-14), p.px(-32), p.py(-9), p.px(-25), p.py(-6), RGB(240, 235, 225))
        cv.drawWideLine(p.px(20), p.py(-9), p.px(29), p.py(-6), 8 * p.s, body, body)
        cv.fillCircle(p.px(31), p.py(-6), 5.5 * p.s, body)
        cv.fillEllipse(p.px(37), p.py(-4), 4.5 * p.s, 3.2 * p.s, RGB(196, 166, 126))
        cv.fillTriangle(p.px(28), p.py(-9), p.px(22), p.py(-15), p.px(30), p.py(-12), dark)
        cv.drawLine(p.px(30), p.py(-9), p.px(34), p.py(-5), X)
        cv.drawLine(p.px(34), p.py(-9), p.px(30), p.py(-5), X)
        if buck:
            ant = RGB(214, 194, 150)
            cv.drawWideLine(p.px(30), p.py(-11), p.px(28), p.py(-26), 1.7 * p.s, ant, ant)
            cv.drawWideLine(p.px(28), p.py(-26), p.px(22), p.py(-30), 1.7 * p.s, ant, ant)
            cv.drawWideLine(p.px(29), p.py(-19), p.px(35), p.py(-24), 1.7 * p.s, ant, ant)
    else:
        tom = c.k == TOM
        body = RGB(86, 56, 36) if tom else RGB(124, 94, 64)
        wing = RGB(62, 40, 28) if tom else RGB(98, 72, 48)
        legc = RGB(205, 150, 110)
        neck = RGB(120, 150, 190) if tom else RGB(168, 140, 118)
        if tom:
            for i in range(7):
                a, a2 = 0.5 + 1.0 * i / 6, 0.5 + 1.0 * (i + 1) / 6
                cv.fillTriangle(p.px(-12), p.py(-9), p.px(-12 - 24 * math.sin(a)), p.py(-9 - 24 * math.cos(a) * 0.55),
                                p.px(-12 - 24 * math.sin(a2)), p.py(-9 - 24 * math.cos(a2) * 0.55),
                                RGB(158, 88, 40) if i & 1 else RGB(206, 168, 108))
        else:
            cv.fillTriangle(p.px(-11), p.py(-9), p.px(-25), p.py(-12), p.px(-22), p.py(-4), wing)
        cv.drawWideLine(p.px(2), p.py(-14), p.px(-2), p.py(-27), 2.2 * p.s, legc, legc)
        cv.drawWideLine(p.px(7), p.py(-14), p.px(5), p.py(-28), 2.2 * p.s, legc, legc)
        cv.drawLine(p.px(-2), p.py(-27), p.px(-6), p.py(-26), legc)
        cv.drawLine(p.px(5), p.py(-28), p.px(9), p.py(-27), legc)
        cv.fillEllipse(p.px(0), p.py(-8), 15 * p.s, 8 * p.s, body)
        cv.fillEllipse(p.px(-2), p.py(-11), 10 * p.s, 5 * p.s, wing)
        cv.drawWideLine(p.px(11), p.py(-8), p.px(21), p.py(-3), 4.2 * p.s, neck, neck)
        cv.fillCircle(p.px(22), p.py(-3), 3.2 * p.s, neck)
        cv.fillTriangle(p.px(24), p.py(-4), p.px(29), p.py(-2.5), p.px(24), p.py(-1.5), RGB(226, 196, 100))
        if tom:
            cv.fillEllipse(p.px(25), p.py(0), 1.6 * p.s, 2.5 * p.s, RGB(200, 40, 40))
        cv.drawLine(p.px(21), p.py(-5), p.px(23.5), p.py(-2), X)
        cv.drawLine(p.px(23.5), p.py(-5), p.px(21), p.py(-2), X)


def draw_hunter(t):
    h = hunter
    if h.st == H_OFF:
        return
    rise = min(1.0, h.t / 1.3) if h.st == H_RISE else max(0.0, 1.0 - h.t / 1.0) if h.st == H_LOWER else 1.0
    rise = rise * rise * (3 - 2 * rise)
    s = 1.3 * scale_at(clamp(h.y, GROUND_MIN, GROUND_MAX + 8))
    p = Pose(h.x, h.y + (1 - rise) * 50 * s, s, h.dir)
    vest, vest_d, pants, boot = RGB(238, 110, 22), RGB(190, 80, 16), RGB(128, 116, 66), RGB(36, 28, 22)
    skin, gun, wood = RGB(232, 188, 146), RGB(60, 62, 68), RGB(112, 72, 38)
    aiming, moving = h.st in (H_AIM, H_SHOT), h.st in (H_RUN, H_DRAG)
    gait = h.t * (13.0 if h.st == H_RUN else 7.0)
    for n in range(2):
        hip = 2.0 if n else -2.0
        sw = math.sin(gait + n * 3.14) * 7 if moving else 0
        fx = hip + sw
        cv.drawWideLine(p.px(hip), p.py(-17), p.px(fx), p.py(-1), 3.8 * p.s, pants, pants)
        cv.fillEllipse(p.px(fx + 1.5), p.py(-1), 3.2 * p.s, 1.8 * p.s, boot)
    cv.fillEllipse(p.px(0), p.py(-26), 7.2 * p.s, 10 * p.s, vest)
    cv.drawLine(p.px(-3), p.py(-32), p.px(-3), p.py(-19), vest_d)
    hx = hy = 0
    if aiming:
        k = 2.0 if (h.st == H_SHOT and h.since < 0.15) else 0.0
        cv.drawWideLine(p.px(-3 - k), p.py(-27), p.px(9 - k), p.py(-30), 3.4 * p.s, wood, wood)
        cv.drawWideLine(p.px(8 - k), p.py(-30), p.px(33 - k), p.py(-32.5), 2.2 * p.s, gun, gun)
        cv.drawWideLine(p.px(3), p.py(-29), p.px(18 - k), p.py(-31), 3.4 * p.s, vest_d, vest_d)
        cv.fillCircle(p.px(18 - k), p.py(-31), 1.8 * p.s, skin)
    elif h.st == H_DRAG:
        cv.drawWideLine(p.px(-5), p.py(-38), p.px(-1), p.py(-12), 2.4 * p.s, gun, gun)
        cv.drawWideLine(p.px(3), p.py(-27), p.px(11), p.py(-21), 3.4 * p.s, vest_d, vest_d)
        hx, hy = p.px(11), p.py(-21)
        cv.fillCircle(hx, hy, 1.8 * p.s, skin)
    else:
        cv.drawWideLine(p.px(0), p.py(-20), p.px(22), p.py(-33), 2.4 * p.s, gun, gun)
        cv.drawWideLine(p.px(3), p.py(-27), p.px(10), p.py(-23), 3.4 * p.s, vest_d, vest_d)
        cv.fillCircle(p.px(10), p.py(-23), 1.8 * p.s, skin)
    cv.fillCircle(p.px(3), p.py(-37), 4.8 * p.s, skin)
    cv.fillCircle(p.px(5.5), p.py(-37.5), 0.9 * p.s, RGB(15, 12, 10))
    cv.fillEllipse(p.px(3), p.py(-40.5), 5.4 * p.s, 3.2 * p.s, vest)
    cv.fillTriangle(p.px(6), p.py(-40), p.px(12), p.py(-38.5), p.px(6), p.py(-37.5), vest_d)
    if h.st == H_DRAG:
        a = crit[h.tgt]
        cv.drawWideLine(hx, hy, a.x + h.drag_dir * (22 if a.k <= FAWN else 12) * scale_at(a.y), a.y - 7, 1.4, RGB(150, 120, 70), RGB(150, 120, 70))
    near_rock = True if h.st not in (H_RUN, H_DRAG) else abs(h.x - ROCKS[h.side][0]) < ROCKS[h.side][2]
    if near_rock:
        draw_rock(h.side)
    if h.since < 2.4:
        if h.since < 0.14:
            cv.fillCircle(h.sx, h.sy, 7, RGB(255, 226, 120))
            cv.fillCircle(h.sx, h.sy, 4, RGB(255, 255, 235))
            for k in range(5):
                a = (k - 2) * 0.5
                cv.drawWideLine(h.sx, h.sy, h.sx + h.dir * 18 * math.cos(a), h.sy + 18 * math.sin(a), 2.5, RGB(255, 226, 120), RGB(255, 226, 120))
        for j in range(4):
            age = h.since - 0.06 * j
            if age <= 0 or age >= 2.4:
                continue
            cv.fillCircle(h.sx + h.dir * (age * 16 + j * 2), h.sy - age * 12, 3 + age * 7, mix(228, 228, 234, 150, 158, 176, age / 2.4))
        if h.since < 0.5:
            cv.drawString("BANG!", h.sx + h.dir * 30 + 1, h.sy - 22 + 1, RGB(60, 30, 10))
            cv.drawString("BANG!", h.sx + h.dir * 30, h.sy - 22, RGB(255, 236, 140))


def draw_scene(t):
    order = sorted(range(len(crit)), key=lambda i: crit[i].y)   # stable, like the insertion sort
    for pas in range(2):
        for f in corn:
            if not f["on"] or (f["z"] > 1.5) != (pas == 1):
                continue
            cv.fillEllipse(f["x"], f["y"] - f["z"], 1.7, 1.1, RGB(236, 190, 60))
            cv.drawPixel(f["x"] - 1, f["y"] - f["z"] - 1, RGB(255, 240, 170))
    for i in order:
        if crit[i].m != HIDDEN:
            draw_shadow(crit[i])
    for i in order:
        c = crit[i]
        if c.m == HIDDEN:
            continue
        if c.m == DOWN:
            draw_down(c)
        else:
            (draw_deer if c.k <= FAWN else draw_turkey)(c, t)
    draw_hunter(t)
    for l in leaves:
        s = math.sin(t * 3 + l["ph"])
        cv.fillEllipse(l["x"], l["y"], 2.6, 1.2 + 0.9 * abs(s), l["c"])
    draw_trunk_front(-6, 34)
    draw_trunk_front(VW - 28, 40)


def pine(x, base, h, col):
    for i in range(3):
        top = base - h + i * h * 0.28
        bot = top + h * 0.46
        hw = (0.13 + 0.07 * i) * h
        cv.fillTriangle(x, top, x - hw, bot, x + hw, bot, col)


PALS = [
    (RGB(120, 52, 26), RGB(190, 92, 34), RGB(228, 138, 52)),
    (RGB(150, 100, 28), RGB(214, 150, 44), RGB(244, 200, 84)),
    (RGB(110, 42, 28), RGB(160, 62, 34), RGB(204, 96, 48)),
    (RGB(70, 78, 36), RGB(110, 116, 48), RGB(150, 150, 64)),
]
seed = [11]


def rnd(n):
    seed[0] = (seed[0] * 1664525 + 1013904223) & 0xFFFFFFFF
    return (seed[0] >> 8) % n


def crown(x, y, r, pal):
    bl = []
    for _ in range(6):
        bx = x + rnd(int(r * 2)) - r
        by = y + rnd(int(r)) - r * 0.6
        bl.append((bx, by, r * 0.5 + rnd(int(r * 0.35))))
    for bx, by, br in bl:
        cv.fillEllipse(bx, by + 3, br, br * 0.75, pal[0])
    for bx, by, br in bl:
        cv.fillEllipse(bx, by, br * 0.92, br * 0.7, pal[1])
    for bx, by, br in bl:
        cv.fillEllipse(bx - br * 0.25, by - br * 0.25, br * 0.45, br * 0.3, pal[2])


def tree(x, base, top, w, cr, pal, bark, shade):
    cv.fillTriangle(x - w * 0.8, base, x + w * 0.8, base, x - w * 0.4, top, bark)
    cv.fillTriangle(x + w * 0.8, base, x + w * 0.4, top, x - w * 0.4, top, bark)
    cv.fillTriangle(x + w * 0.05, base, x + w * 0.8, base, x + w * 0.4, top, shade)
    cv.fillTriangle(x - w * 0.8, base, x - w * 1.8, base + 5, x - w * 0.5, base - 12, bark)
    cv.fillTriangle(x + w * 0.8, base, x + w * 1.8, base + 5, x + w * 0.5, base - 12, shade)
    by = top + (base - top) * 0.3
    cv.drawWideLine(x, by, x - cr * 0.8, top - cr * 0.05, w * 0.42, bark, bark)
    cv.drawWideLine(x, by + 6, x + cr * 0.85, top + cr * 0.1, w * 0.42, shade, shade)
    crown(x - cr * 0.8, top, cr * 0.55, pal)
    crown(x + cr * 0.85, top + cr * 0.15, cr * 0.55, pal)
    crown(x, top - cr * 0.3, cr, pal)


def render_background():
    seed[0] = 11
    for vy in range(VH):
        if vy < HORIZON:
            cv.hline(vy, mix(112, 148, 192, 248, 216, 164, vy / HORIZON))
    for k in range(8):
        cv.fillCircle(372, 78, 52 - k * 6.5, mix(214, 200, 176, 255, 242, 204, k / 7))
    x = -20
    while x < VW + 60:
        cv.fillEllipse(x, HORIZON + 8, 90, 34 + rnd(12), RGB(128, 134, 150))
        x += 76
    x = 4
    while x < VW:
        pine(x, HORIZON + 6, 44 + rnd(30), RGB(78, 100, 98))
        x += 13 + rnd(7)
    for vy in range(HORIZON, VH):
        cv.hline(vy, mix(116, 104, 58, 72, 52, 33, (vy - HORIZON) / (VH - HORIZON)))
    x = 20
    while x < VW:
        base = HORIZON + 12 + rnd(12)
        top = base - 78 - rnd(36)
        w = 4 + rnd(2)
        tree(x, base, top, w, 20 + rnd(8), PALS[rnd(4)], RGB(78, 58, 44), RGB(54, 40, 30))
        x += 44 + rnd(16)
    x = 0
    while x < VW:
        cv.fillEllipse(x, HORIZON + 16, 22 + rnd(14), 9 + rnd(5), mix(76, 82, 42, 106, 88, 40, rnd(100) / 100))
        x += 34 + rnd(24)
    for nx in (92, 196, 300, 392):
        x = nx + rnd(12)
        w = 6 + rnd(3)
        base = 206 + rnd(12)
        pal = PALS[rnd(3)]
        bark, shade = RGB(62, 46, 34), RGB(42, 30, 22)
        top = base - 150 - rnd(22)
        tree(x, base, top, w, 28, pal, bark, shade)
        by = top + 48 + rnd(20)
        dx = (1 if rnd(2) else -1) * (20 + rnd(10))
        cv.drawWideLine(x, by + 10, x + dx, by, w * 0.5, bark, bark)
        crown(x + dx, by - 2, 14 + rnd(5), pal)
        for k in range(5):
            lx = x - w * 0.4 + k * w * 0.25
            cv.drawLine(lx, top + 30 + rnd(40), lx, top + 70 + rnd(70), shade)
    cv.fillRoundRect(318, 209, 72, 13, 6, RGB(84, 60, 40))
    cv.fillRoundRect(318, 209, 72, 4, 2, RGB(106, 80, 54))
    cv.fillEllipse(318, 215.5, 4, 6.5, RGB(150, 118, 78))
    litter = [RGB(188, 96, 36), RGB(214, 150, 50), RGB(150, 64, 30), RGB(100, 78, 40)]
    for i in range(380):
        x, y = rnd(VW), HORIZON + 4 + rnd(VH - HORIZON - 4)
        if i % 3:
            cv.drawPixel(x, y, litter[rnd(4)])
        else:
            cv.fillEllipse(x, y, 2, 1, litter[rnd(4)])
    for _ in range(70):
        x, y = rnd(VW), 214 + rnd(104)
        g = mix(90, 96, 44, 124, 112, 52, rnd(100) / 100)
        hgt = 5 + (y - 214) * 0.05
        cv.drawLine(x, y, x - 2, y - hgt, g)
        cv.drawLine(x, y, x, y - hgt - 2, g)
        cv.drawLine(x, y, x + 2, y - hgt, g)
    draw_rock(0)
    draw_rock(1)


def hunt_sheet(outcome, side):
    """python design/preview.py hunt miss|buck|tom [0|1]  ->  design/hunt_<outcome>.png, a contact sheet of the event."""
    random.seed(11)
    leaves.clear()
    for _ in range(18):
        leaves.append(dict(x=random.randrange(VW), y=random.randrange(VH), vy=12 + random.randrange(14),
                           vx=random.randint(-4, 7), ph=random.randrange(628) / 100, c=random.choice(LEAF_COLS)))
    render_background()
    bg = cv.im.copy()
    crit[:] = make_world()
    force = {"miss": -1, "buck": 0, "tom": 4}[outcome]
    offs = [0.9, 2.3, 2.95 + 0.07, 3.1 + 0.4, 3.1 + 1.3, 3.1 + 3.0, 3.1 + 5.5, 3.1 + 9.0, 3.1 + 14.0, 3.1 + 22.0, 3.1 + 32.0, 3.1 + 42.0] if force >= 0 \
        else [0.9, 2.3, 2.95 + 0.07, 3.1 + 0.4, 3.1 + 1.0, 3.1 + 2.2, 3.1 + 4.0, 3.1 + 6.0, 3.1 + 9.0, 3.1 + 12.0, 3.1 + 18.0, 3.1 + 26.0]
    fps, start, dt = 20, 3.0, 1 / 20
    tiles, t = [], 0.0
    while t < start + max(offs) + dt:
        if abs(t - start) < dt / 2:
            start_hunter(side, force)
        update_world(dt, t)
        cv.im.paste(bg)
        draw_scene(t)
        for off in offs:
            if abs(t - (start + off)) < dt / 2:
                tile = cv.im.copy()
                ImageDraw.Draw(tile).text((6, 4), "+%.1f s" % off, fill=(255, 255, 255))
                tiles.append(tile)
        t += dt
    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * VW, rows * VH))
    for i, tl in enumerate(tiles):
        sheet.paste(tl, ((i % cols) * VW, (i // cols) * VH))
    path = os.path.join(OUT, "hunt_%s.png" % outcome)
    sheet.save(path)
    print(path, len(tiles), "frames")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "hunt":
        hunt_sheet(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
        return
    random.seed(4)
    for _ in range(18):
        leaves.append(dict(x=random.randrange(VW), y=random.randrange(VH), vy=12 + random.randrange(14),
                           vx=random.randint(-4, 7), ph=random.randrange(628) / 100, c=random.choice(LEAF_COLS)))
    render_background()
    bg = cv.im.copy()
    fps, secs, corn_at = 20, 40, 18.0
    frames_dir = os.path.join(OUT, "_frames")
    os.makedirs(frames_dir, exist_ok=True)
    stills = {2.0: "a", 14.0: "b", 19.0: "d", 24.0: "c"}
    dropped = False
    for f in range(int(secs * fps)):
        t = f / fps
        if not dropped and t >= corn_at:
            spawn_corn(260, 270)
            dropped = True
        update_world(1 / fps, t)
        cv.im.paste(bg)
        draw_scene(t)
        big = cv.im.resize((VW * 2, VH * 2), Image.NEAREST)
        big.save(os.path.join(frames_dir, "f%04d.png" % f))
        for st, name in stills.items():
            if abs(t - st) < 1 / fps / 2:
                big.save(os.path.join(OUT, "woods_%s.png" % name))
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(fps), "-i", os.path.join(frames_dir, "f%04d.png"),
                    "-c:v", "libx264", "-pix_fmt", "yuv420p", os.path.join(OUT, "woods.mp4")], check=False)
    print("done")


if __name__ == "__main__":
    main()
