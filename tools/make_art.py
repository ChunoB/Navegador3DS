"""Draws icon.png (48x48), banner.png (256x128) and banner.wav for Navegador 3DS.
Standard library only. Usage: python make_art.py <project dir>
"""
import math, struct, sys, wave, zlib

ROOT = sys.argv[1] if len(sys.argv) > 1 else ".."
SS = 4


class Canvas:
    def __init__(self, w, h, bg):
        self.w, self.h = w * SS, h * SS
        self.px = [list(bg) for _ in range(self.w * self.h)]

    def blend(self, x, y, c, a=1.0):
        if 0 <= x < self.w and 0 <= y < self.h:
            p = self.px[y * self.w + x]
            for i in range(3):
                p[i] = p[i] * (1 - a) + c[i] * a

    def poly(self, pts, c, a=1.0):
        pts = [(x * SS, y * SS) for x, y in pts]
        y0, y1 = int(min(p[1] for p in pts)), int(max(p[1] for p in pts)) + 1
        for y in range(max(0, y0), min(self.h, y1)):
            yc = y + 0.5
            xs = []
            for i in range(len(pts)):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % len(pts)]
                if (ya <= yc < yb) or (yb <= yc < ya):
                    xs.append(xa + (yc - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for k in range(0, len(xs) - 1, 2):
                for x in range(int(xs[k] + 0.5), int(xs[k + 1] + 0.5)):
                    self.blend(x, y, c, a)

    def rect(self, x, y, w, h, c, a=1.0):
        self.poly([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], c, a)

    def rrect(self, x, y, w, h, r, top, bot):
        for yy in range(int(y * SS), int((y + h) * SS)):
            t = (yy / SS - y) / h
            col = [top[i] * (1 - t) + bot[i] * t for i in range(3)]
            for xx in range(int(x * SS), int((x + w) * SS)):
                fx, fy = xx / SS, yy / SS
                cx = min(max(fx, x + r), x + w - r)
                cy = min(max(fy, y + r), y + h - r)
                if (fx - cx) ** 2 + (fy - cy) ** 2 <= r * r:
                    self.blend(xx, yy, col)

    def save_png(self, path):
        w, h = self.w // SS, self.h // SS
        raw = bytearray()
        for y in range(h):
            raw.append(0)
            for x in range(w):
                acc = [0, 0, 0]
                for sy in range(SS):
                    for sx in range(SS):
                        p = self.px[(y * SS + sy) * self.w + x * SS + sx]
                        for i in range(3):
                            acc[i] += p[i]
                raw += bytes(min(255, int(v / (SS * SS))) for v in acc) + b"\xff"

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
        open(path, "wb").write(png)


def globe(cv, cx, cy, r, th):
    """Wireframe globe: outline, meridians (ellipses) and parallels (lines)."""
    white = (255, 255, 255)

    def ellipse(rx, ry):
        steps = 900
        for k in range(steps):
            a = k * 2 * math.pi / steps
            x, y = cx + rx * math.cos(a), cy + ry * math.sin(a)
            cv.rect(x - th / 2, y - th / 2, th, th, white)

    ellipse(r, r)                    # outline
    ellipse(r * 0.45, r)             # meridians
    cv.rect(cx - th / 2, cy - r, th, 2 * r, white)
    cv.rect(cx - r, cy - th / 2, 2 * r, th, white)   # equator
    for f in (-0.55, 0.55):          # parallels
        half = r * math.sqrt(1 - f * f)
        cv.rect(cx - half, cy + f * r - th / 2, 2 * half, th, white)


FONT = {
    "C": ["0111", "1000", "1000", "1000", "1000", "1000", "0111"],
    "A": ["0110", "1001", "1001", "1111", "1001", "1001", "1001"],
    "N": ["1001", "1101", "1101", "1011", "1011", "1001", "1001"],
    "V": ["1001", "1001", "1001", "1001", "1001", "0110", "0110"],
    "S": ["0111", "1000", "1000", "0110", "0001", "0001", "1110"],
    "3": ["1110", "0001", "0001", "0110", "0001", "0001", "1110"],
    "D": ["1110", "1001", "1001", "1001", "1001", "1001", "1110"],
    "E": ["1111", "1000", "1000", "1110", "1000", "1000", "1111"],
    "G": ["0111", "1000", "1000", "1011", "1001", "1001", "0111"],
    "O": ["0110", "1001", "1001", "1001", "1001", "1001", "0110"],
    "R": ["1110", "1001", "1001", "1110", "1010", "1001", "1001"],
}


def text(cv, s, x, y, px, col):
    for ch in s:
        g = FONT[ch]
        for r, row in enumerate(g):
            for c, bit in enumerate(row):
                if bit == "1":
                    cv.rect(x + c * px, y + r * px, px, px, col)
        x += (len(g[0]) + 1) * px


def main():
    icon = Canvas(48, 48, (0, 0, 0))
    icon.rrect(1, 1, 46, 46, 9, (60, 140, 235), (30, 80, 190))
    globe(icon, 24, 24, 16, 1.6)
    icon.save_png(f"{ROOT}/icon.png")

    ban = Canvas(256, 128, (0, 0, 0))
    ban.rrect(0, 0, 256, 128, 1, (44, 60, 74), (24, 34, 44))
    ban.rrect(168, 22, 76, 76, 14, (60, 140, 235), (30, 80, 190))
    globe(ban, 206, 60, 26, 2.0)
    text(ban, "NAVEGADOR", 12, 40, 3, (255, 255, 255))
    text(ban, "3DS", 12, 72, 4, (110, 175, 250))
    ban.save_png(f"{ROOT}/banner.png")

    rate, out = 22050, bytearray()
    for i in range(int(rate * 1.4)):
        t = i / rate
        v = 0.0
        for start, f in ((0.0, 659.3), (0.12, 784.0), (0.24, 1046.5)):
            if t >= start:
                tt = t - start
                v += 0.25 * math.sin(2 * math.pi * f * tt) * math.exp(-tt * 3.5)
        out += struct.pack("<h", int(max(-1, min(1, v)) * 32767))
    with wave.open(f"{ROOT}/banner.wav", "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(out))
    print("ok")


main()
