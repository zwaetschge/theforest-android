#!/usr/bin/env python3
"""Original launcher icon for The Forest (pure Python, no Pillow needed).
Misty dusk sky, glowing sun, layered pine silhouettes, and survival plane axe.
usage: make_icon.py <res dir>"""
import math, os, struct, sys, zlib

res = sys.argv[1]
sizes = {'mdpi': 48, 'hdpi': 72, 'xhdpi': 96, 'xxhdpi': 144, 'xxxhdpi': 192}
S = 512

def in_tri(x, y, x1, y1, x2, y2, x3, y3):
    d1 = (x - x2) * (y1 - y2) - (x1 - x2) * (y - y2)
    d2 = (x - x3) * (y2 - y3) - (x2 - x3) * (y - y3)
    d3 = (x - x1) * (y3 - y1) - (x3 - x1) * (y - y1)
    return not (((d1 < 0) or (d2 < 0) or (d3 < 0)) and ((d1 > 0) or (d2 > 0) or (d3 > 0)))

def in_poly(x, y, pts):
    inside = False
    n = len(pts)
    j = n - 1
    for i in range(n):
        xi, yi = pts[i]; xj, yj = pts[j]
        if ((yi > y) != (yj > y)) and (x < (xj - xi) * (y - yi) / (yj - yi + 1e-9) + xi):
            inside = not inside
        j = i
    return inside

def in_pine(x, y, cx, top, h, w):
    if y < top or y > top + h or abs(x - cx) > w * 0.75: return False
    return (in_tri(x, y, cx, top, cx - w * 0.42, top + h * 0.36, cx + w * 0.42, top + h * 0.36)
         or in_tri(x, y, cx, top + h * 0.22, cx - w * 0.56, top + h * 0.66, cx + w * 0.56, top + h * 0.66)
         or in_tri(x, y, cx, top + h * 0.48, cx - w * 0.72, top + h, cx + w * 0.72, top + h))

def rounded_alpha(x, y, s, m=16, r=110):
    if x < m or x >= s - m or y < m or y >= s - m: return 0
    dx = max(0, max(m + r - x, x - (s - m - r - 1)))
    dy = max(0, max(m + r - y, y - (s - m - r - 1)))
    if dx == 0 or dy == 0: return 255
    d = math.hypot(dx, dy)
    if d <= r - 1: return 255
    if d >= r + 1: return 0
    return int(255 * (r + 1 - d) / 2.0)

back_pines = [(84, 196, 210, 108), (168, 172, 234, 116), (344, 178, 228, 114), (428, 200, 206, 104)]
front_pines = [(54, 235, 260, 128), (132, 222, 275, 132), (382, 226, 270, 130), (462, 240, 255, 125)]
handle_r = [(246, 232), (272, 232), (264, 434), (242, 434)]
handle_l = [(246, 232), (259, 232), (253, 434), (242, 434)]
head_main = [(218, 216), (318, 194), (326, 254), (314, 294), (222, 268)]
head_edge = [(304, 197), (318, 194), (326, 254), (314, 294), (300, 290)]
head_back = [(218, 216), (252, 220), (250, 268), (222, 268)]

buf = bytearray(S * S * 4)
for y in range(S):
    t = y / (S - 1)
    br = max(14, min(145, int(18 + 115 * max(0.0, 1.0 - abs(t - 0.44) * 2.2))))
    bg = max(24, min(95,  int(26 + 55  * max(0.0, 1.0 - abs(t - 0.46) * 2.2))))
    bb = max(22, min(72,  int(30 + 36  * max(0.0, 1.0 - abs(t - 0.35) * 2.2))))
    mist = max(0.0, 1.0 - abs(y - 328) / 44.0) * 0.28 if 285 <= y <= 370 else 0.0
    row = y * S * 4
    for x in range(S):
        a = rounded_alpha(x, y, S)
        if a == 0: continue
        r, g, b = br, bg, bb
        ds = math.hypot(x - 256, y - 178)
        if ds < 80:
            r, g, b = (240, 165, 98) if ds < 62 else (215, 118, 72)
        for p in back_pines:
            if in_pine(x, y, *p):
                r, g, b = 24, 42, 34; break
        if mist > 0:
            r = int(r * (1 - mist) + 185 * mist)
            g = int(g * (1 - mist) + 205 * mist)
            b = int(b * (1 - mist) + 190 * mist)
        if y >= 412:
            r, g, b = 9, 17, 14
        for p in front_pines:
            if in_pine(x, y, *p):
                r, g, b = 10, 20, 16; break
        if in_poly(x, y, handle_r):
            r, g, b = (150, 48, 32) if in_poly(x, y, handle_l) else (195, 72, 48)
        if in_poly(x, y, head_main):
            if in_poly(x, y, head_edge): r, g, b = 242, 248, 250
            elif in_poly(x, y, head_back): r, g, b = 138, 150, 156
            else: r, g, b = 212, 222, 226
        o = row + x * 4
        buf[o] = r; buf[o + 1] = g; buf[o + 2] = b; buf[o + 3] = a

def save_png(path, w, h, rgba):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    def ck(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + ck(b'IDAT', zlib.compress(bytes(raw), 6)) + ck(b'IEND', b'')
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    open(path, 'wb').write(png)

def downsample(src, sw, dw):
    out = bytearray(dw * dw * 4)
    scale = sw / dw
    for dy in range(dw):
        y0 = int(dy * scale); y1 = max(y0 + 1, int((dy + 1) * scale))
        for dx in range(dw):
            x0 = int(dx * scale); x1 = max(x0 + 1, int((dx + 1) * scale))
            sr = sg = sb = sa = n = 0
            for yy in range(y0, min(y1, sw)):
                row = yy * sw * 4
                for xx in range(x0, min(x1, sw)):
                    o = row + xx * 4
                    sr += src[o]; sg += src[o + 1]; sb += src[o + 2]; sa += src[o + 3]; n += 1
            o = (dy * dw + dx) * 4
            out[o] = sr // n; out[o + 1] = sg // n; out[o + 2] = sb // n; out[o + 3] = sa // n
    return out

save_png('docs/icon_512.png', S, S, buf)
for k, sz in sizes.items():
    save_png(f'{res}/mipmap-{k}/ic_launcher.png', sz, sz, downsample(buf, S, sz))
print('The Forest icons generated cleanly!')
