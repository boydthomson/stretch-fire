#!/usr/bin/env python3
"""Render assets/fire-demo.gif: a simulation of the strip running the Hearth
effect, with spark bursts when bookings arrive.

The effect maths mirrors esphome/fire_effects.h so the GIF shows what the real
strip does. Run from the repo root:  python3 tools/render_demo.py
"""
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

N = 60                      # LEDs
FPS_SIM = 60                # device frame rate
FPS_GIF = 25
SECONDS = 7.6
W, H = 720, 210
FONT_B = "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"
FONT_R = "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"

# (time in seconds, number of bookings, toast text)
BOOKINGS = [(1.6, 1, "New booking: Core Flow Pilates"),
            (4.6, 2, "2 new bookings: Traditional Hatha")]

rng = random.Random(7)
def rand8(lo=0, hi=256):
    return lo if hi <= lo else lo + rng.randrange(hi - lo)
def scale8(v, s):
    return (int(v) * int(s)) >> 8

def lattice(x, y):
    h = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return (h ^ (h >> 16)) & 0xFF

def noise8(x, y):
    xi, yi = x >> 8, y >> 8
    fx, fy = (x & 0xFF) / 256, (y & 0xFF) / 256
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = lattice(xi, yi), lattice(xi + 1, yi)
    c, d = lattice(xi, yi + 1), lattice(xi + 1, yi + 1)
    top, bot = a + (b - a) * fx, c + (d - c) * fx
    return int(top + (bot - top) * fy)

STOPS = [(0, 0, 0, 0), (50, 40, 1, 0), (110, 160, 15, 0),
         (170, 255, 60, 0), (220, 255, 130, 8), (255, 255, 180, 50)]
def palette(i):
    for p, q in zip(STOPS, STOPS[1:]):
        if i <= q[0]:
            t = (i - p[0]) / (q[0] - p[0])
            return [p[k] + (q[k] - p[k]) * t for k in (1, 2, 3)]
    return [255, 180, 50]

class Fire:
    def __init__(self):
        self.t = rng.randrange(1 << 20)
        self.gust = 0
        self.flare = 0
        self.pending = 0
        self.sparks = []

    def frame(self):
        self.t += 9
        if rand8() < 6:
            dip = rand8(70 // 3, 70)
            self.gust = max(self.gust, dip)
        self.gust = scale8(self.gust, 235)
        px = []
        for i in range(N):
            slow = noise8(i * 40, self.t)
            fast = noise8(i * 120 + 10000, self.t * 3)
            h = min(255, scale8(slow, 190) + scale8(fast, 90))
            h = 0 if h < 60 else min(255, (h - 60) * 3 // 2)
            h = max(max(0, h - self.gust), 25)
            px.append(palette(scale8(h, 240)))
        self.overlay(px)
        return np.array(px, dtype=float)

    def overlay(self, px):
        if self.pending and self.flare < 40:
            self.pending -= 1
            self.flare = 255
            for _ in range(rand8(8, 14)):
                self.sparks.append([float(rand8(0, N)), 0.15 + rand8(0, 40) / 100, rand8(200, 255)])
        if self.flare:
            amt = scale8(self.flare, 150)
            for c in px:
                c[0] = min(255, c[0] + scale8(255 - c[0], amt))
                c[1] = min(255, c[1] + scale8(200 - min(c[1], 200), amt))
                c[2] = min(255, c[2] + scale8(60 - min(c[2], 60), amt))
            self.flare = scale8(self.flare, 240)
        for s in self.sparks:
            p = int(s[0])
            if 0 <= p < N:
                life = s[2]
                c = px[p]
                c[0] = max(c[0], life)
                c[1] = max(c[1], scale8(life, 230))
                c[2] = max(c[2], scale8(scale8(life, life), 160))
            s[0] += s[1]
            s[2] = max(0, s[2] - rand8(3, 8))
        self.sparks = [s for s in self.sparks if s[2] > 0 and s[0] < N]

def render():
    fire = Fire()
    font_b = ImageFont.truetype(FONT_B, 21)
    font_s = ImageFont.truetype(FONT_R, 14)
    margin = 40
    pitch = (W - 2 * margin) / (N - 1)
    ys = 140
    xs = [margin + i * pitch for i in range(N)]
    frames, booked = [], list(BOOKINGS)
    toast_text, toast_start = None, -10.0
    steps = int(SECONDS * FPS_SIM)
    for step in range(steps):
        t = step / FPS_SIM
        while booked and t >= booked[0][0]:
            _, n, text = booked.pop(0)
            fire.pending += n
            toast_text, toast_start = text, t
        leds = fire.frame()
        if int(t * FPS_GIF) < len(frames):
            continue  # only keep one simulated frame per GIF frame
        # Display: colour correction as on the device, then a screen gamma so LEDs read as light.
        disp = leds * np.array([1.0, 0.69, 0.94])
        disp = 255 * (disp / 255) ** (1 / 1.8)

        glow = Image.new("RGB", (W, H), (0, 0, 0))
        core = Image.new("RGB", (W, H), (0, 0, 0))
        dg, dc = ImageDraw.Draw(glow), ImageDraw.Draw(core)
        for x, c in zip(xs, disp):
            col = tuple(int(v) for v in c)
            dg.ellipse([x - 7, ys - 7, x + 7, ys + 7], fill=col)
            dc.ellipse([x - 3.6, ys - 3.6, x + 3.6, ys + 3.6], fill=col)
        wash = glow.filter(ImageFilter.GaussianBlur(34))
        bloom = glow.filter(ImageFilter.GaussianBlur(9))
        base = np.zeros((H, W, 3))
        base[:] = (14, 9, 7)
        img = base + 1.6 * np.asarray(wash, float) + 0.9 * np.asarray(bloom, float) + np.asarray(core, float)
        # strip body
        im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
        d = ImageDraw.Draw(im, "RGBA")
        d.rounded_rectangle([margin - 14, ys - 9, W - margin + 14, ys + 9], 9, outline=(255, 255, 255, 28), width=1)
        for x, c in zip(xs, disp):
            col = tuple(int(min(255, v * 1.05 + 30)) for v in c)
            d.ellipse([x - 2.6, ys - 2.6, x + 2.6, ys + 2.6], fill=col)

        # booking toast
        age = t - toast_start
        if toast_text and age < 2.6:
            a = min(1, age / 0.25, (2.6 - age) / 0.4)
            slide = int((1 - min(1, age / 0.25)) * 12)
            tw = d.textlength(toast_text, font=font_b)
            bw, bh = tw + 76, 48
            x0, y0 = (W - bw) / 2, 30 - slide
            d.rounded_rectangle([x0, y0, x0 + bw, y0 + bh], 24, fill=(255, 250, 240, int(235 * a)))
            d.ellipse([x0 + 16, y0 + 14, x0 + 36, y0 + 34], fill=(232, 93, 26, int(255 * a)))
            d.text((x0 + 48, y0 + 9), toast_text, font=font_b, fill=(40, 24, 16, int(255 * a)))
        d.text((W - 16, H - 12), "simulated from the real effect code", font=font_s,
               fill=(255, 220, 190, 70), anchor="rs")
        frames.append(im)

    # One shared palette so the flicker doesn't shimmer between frames.
    pal_src = Image.new("RGB", (W, H * 5))
    for k, f in enumerate([frames[10], frames[45], frames[int(1.7 * FPS_GIF)], frames[int(4.7 * FPS_GIF)], frames[-5]]):
        pal_src.paste(f, (0, H * k))
    pal = pal_src.quantize(colors=255, method=Image.Quantize.MEDIANCUT)
    q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    out = Path(__file__).resolve().parent.parent / "assets" / "fire-demo.gif"
    q[0].save(out, save_all=True, append_images=q[1:], duration=1000 // FPS_GIF, loop=0, optimize=True)
    print(out, f"{out.stat().st_size / 1e6:.2f} MB", len(frames), "frames")

if __name__ == "__main__":
    render()
