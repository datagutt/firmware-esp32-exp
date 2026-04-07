load("render.star", "render", "canvas")
load("math.star", "math")

# No WiFi — Rain storm with failing WiFi signal, lightning flashes, and sparks.

def to_hex(v):
    h = "0123456789abcdef"
    v = max(0, min(255, v))
    return h[v // 16] + h[v % 16]

def px(x, y, color, s):
    return render.Padding(pad = (x, y, 0, 0), child = render.Box(width = s, height = s, color = color))

# WiFi icon layers (relative to center-bottom dot)
WIFI_DOT = [(0, 0)]
WIFI_ARC1 = [(-1, -2), (0, -2), (1, -2)]
WIFI_ARC2 = [(-2, -4), (-1, -3), (0, -4), (1, -3), (2, -4)]
WIFI_ARC3 = [(-4, -6), (-3, -5), (-2, -6), (0, -6), (2, -6), (3, -5), (4, -6)]

# Red diagonal slash (2px thick)
SLASH = [
    (4, -7), (3, -6), (2, -5), (1, -4), (0, -3), (-1, -2), (-2, -1), (-3, 0),
    (4, -6), (3, -5), (2, -4), (1, -3), (0, -2), (-1, -1), (-2, 0), (-3, 1),
]

# Rain drop columns: (x_1x, speed, phase)
RAIN = [
    (2, 1.8, 0), (7, 2.3, 4), (12, 1.5, 9), (18, 2.6, 2),
    (23, 1.2, 7), (29, 2.0, 11), (34, 1.7, 3), (40, 2.4, 8),
    (45, 1.4, 13), (51, 2.1, 1), (56, 1.6, 6), (61, 1.9, 10),
    (5, 2.5, 14), (15, 1.1, 5), (26, 2.7, 12), (37, 1.3, 15),
    (48, 2.2, 9), (10, 1.8, 3), (42, 2.0, 7), (20, 1.5, 11),
]

# Spark positions around WiFi icon
SPARKS = [(-5, -3), (5, -4), (-4, 1), (6, -1), (-6, -7), (6, -8), (-7, -5), (7, -3)]

def main(config):
    s = 2 if canvas.is2x() else 1
    w = canvas.width()
    h = canvas.height()

    font = "6x13" if s == 1 else "10x20"
    char_w = 6 if s == 1 else 10
    text_h = 13 if s == 1 else 20

    text = "no wifi"
    text_x = (w - len(text) * char_w) // 2
    text_y = h - text_h - 2 * s

    cx = w // 2
    cy = (h - text_h - 2 * s) // 2 + 4 * s

    frames = []
    for f in range(60):
        el = []

        # ── Rain ──
        for rx, rspeed, rphase in RAIN:
            rain_x = int(rx * s * w / 64)
            if rain_x >= w:
                continue
            drop_len = 3 * s
            travel = h + drop_len + 5 * s
            base_y = int((f * rspeed * s + rphase * 4 * s) % travel) - drop_len
            for dy in range(drop_len):
                py = base_y + dy
                if py >= 0 and py < text_y:
                    t = dy / max(1, drop_len - 1)
                    bright = int(25 + 55 * t)
                    c = "#" + to_hex(int(bright * 0.25)) + to_hex(int(bright * 0.55)) + to_hex(bright)
                    el.append(px(rain_x, py, c, s))

        # ── Lightning flash (every ~30 frames, 2 frame burst) ──
        is_flash = (f % 30) < 2
        if is_flash:
            fb = 30 if (f % 30) == 0 else 14
            for lx in range(0, w, 3 * s):
                for ly in range(0, text_y, 4 * s):
                    fh = to_hex(fb)
                    el.append(px(lx, ly, "#" + fh + fh + fh, s))

        # ── WiFi icon — arcs fail sequentially, then flash back ──
        cycle = f % 40
        show3 = cycle < 10 or cycle >= 34
        show2 = cycle < 18 or cycle >= 32
        show1 = cycle < 24 or cycle >= 30

        pulse = (math.sin(f * 0.25) + 1) / 2
        wb = int(130 + 125 * pulse)
        wc = "#" + to_hex(int(wb * 0.98)) + to_hex(int(wb * 0.75)) + to_hex(int(wb * 0.14))

        # Recovery flash — bright white burst
        if cycle >= 30 and cycle < 34:
            fb = int(255 * (34 - cycle) / 4.0)
            wc = "#" + to_hex(fb) + to_hex(int(fb * 0.9)) + to_hex(int(fb * 0.3))

        for dx, dy in WIFI_DOT:
            el.append(px(cx + dx * s, cy + dy * s, wc, s))
        if show1:
            for dx, dy in WIFI_ARC1:
                el.append(px(cx + dx * s, cy + dy * s, wc, s))
        if show2:
            for dx, dy in WIFI_ARC2:
                el.append(px(cx + dx * s, cy + dy * s, wc, s))
        if show3:
            for dx, dy in WIFI_ARC3:
                el.append(px(cx + dx * s, cy + dy * s, wc, s))

        # Red slash
        for dx, dy in SLASH:
            el.append(px(cx + dx * s, cy + dy * s, "#ff4444", s))

        # ── Yellow sparks during lightning ──
        if is_flash:
            for i, (sx, sy) in enumerate(SPARKS):
                if (f + i) % 2 == 0:
                    el.append(px(cx + sx * s, cy + sy * s, "#ffff00", s))

        # ── Text ──
        tc = "#ccddff" if is_flash else "#ffffff"
        el.append(render.Padding(
            pad = (text_x, text_y, 0, 0),
            child = render.Text(content = text, color = tc, font = font),
        ))

        frames.append(render.Stack(children = el))

    return render.Root(delay = 80 // s, child = render.Animation(children = frames))
