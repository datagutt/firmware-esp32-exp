load("render.star", "render", "canvas")
load("math.star", "math")

# 404 Not Found — Floating neon ghost with chromatic aberration "404" text,
# scanlines, and ghostly trail. Ghost sways gently, always visible.

def to_hex(v):
    h = "0123456789abcdef"
    v = max(0, min(255, v))
    return h[v // 16] + h[v % 16]

def px(x, y, color, s):
    return render.Padding(pad = (x, y, 0, 0), child = render.Box(width = s, height = s, color = color))

# Ghost body (9 wide x 10 tall), relative to top-left
GHOST_BODY = [
    (3, 0), (4, 0), (5, 0),
    (2, 1), (3, 1), (4, 1), (5, 1), (6, 1),
    (1, 2), (2, 2), (3, 2), (4, 2), (5, 2), (6, 2), (7, 2),
    (0, 3), (1, 3), (2, 3), (3, 3), (4, 3), (5, 3), (6, 3), (7, 3), (8, 3),
    (0, 4), (1, 4), (4, 4), (7, 4), (8, 4),
    (0, 5), (1, 5), (4, 5), (7, 5), (8, 5),
    (0, 6), (1, 6), (2, 6), (3, 6), (4, 6), (5, 6), (6, 6), (7, 6), (8, 6),
    (0, 7), (1, 7), (2, 7), (3, 7), (4, 7), (5, 7), (6, 7), (7, 7), (8, 7),
    (0, 8), (1, 8), (2, 8), (3, 8), (4, 8), (5, 8), (6, 8), (7, 8), (8, 8),
    (0, 9), (2, 9), (3, 9), (5, 9), (6, 9), (8, 9),
]

# Eyes (dark on body) and pupils
GHOST_EYES = [
    (2, 4), (3, 4), (2, 5), (3, 5),
    (5, 4), (6, 4), (5, 5), (6, 5),
]
GHOST_PUPILS = [(3, 5), (6, 5)]

def ghost_color(phase):
    """Cycle through vivid pastel colors"""
    p = phase * 5.0
    i = int(p) % 5
    t = p - int(p)
    if i == 0:
        return (int(80 + 175 * t), int(220 - 80 * t), 255)
    elif i == 1:
        return (255, int(140 + 80 * t), int(255 - 155 * t))
    elif i == 2:
        return (int(255 - 120 * t), 220, int(100 + 100 * t))
    elif i == 3:
        return (int(135 - 55 * t), int(220 + 35 * t), int(200 + 55 * t))
    else:
        return (int(80), int(255 - 35 * t), int(255))

def draw_ghost(el, gx, gy, s, r, g, b, alpha):
    """Draw ghost with given color and alpha multiplier"""
    if alpha < 0.05:
        return
    cr = to_hex(int(r * alpha))
    cg = to_hex(int(g * alpha))
    cb = to_hex(int(b * alpha))
    body_c = "#" + cr + cg + cb
    eye_c = "#" + to_hex(int(30 * alpha)) + to_hex(int(15 * alpha)) + to_hex(int(50 * alpha))
    pupil_c = "#" + to_hex(int(10 * alpha)) + to_hex(int(5 * alpha)) + to_hex(int(15 * alpha))

    for dx, dy in GHOST_BODY:
        el.append(px(gx + dx * s, gy + dy * s, body_c, s))
    for dx, dy in GHOST_EYES:
        el.append(px(gx + dx * s, gy + dy * s, eye_c, s))
    for dx, dy in GHOST_PUPILS:
        el.append(px(gx + dx * s, gy + dy * s, pupil_c, s))

def main(config):
    s = 2 if canvas.is2x() else 1
    w = canvas.width()
    h = canvas.height()

    font = "6x13" if s == 1 else "10x20"
    char_w = 6 if s == 1 else 10
    text_h = 13 if s == 1 else 20

    ghost_w = 9 * s
    ghost_h = 10 * s

    text_404 = "404"
    text_x = (w - len(text_404) * char_w) // 2
    text_y = h - text_h - 2 * s

    # Ghost floats in the upper area, centered, swaying gently
    ghost_area_h = text_y - 2 * s
    ghost_center_x = (w - ghost_w) // 2
    ghost_base_y = (ghost_area_h - ghost_h) // 2

    frames = []
    for f in range(60):
        el = []

        # ── Background static/noise ──
        for i in range(15):
            sx = ((i * 7 + f * 3 + i * i) % 31) * w // 31
            sy = ((i * 11 + f * 5 + i * 3) % 23) * text_y // 23
            dim = 12 + ((i * 3 + f * 2) % 20)
            sc = "#" + to_hex(int(dim * 0.6)) + to_hex(int(dim * 0.4)) + to_hex(int(dim * 1.3))
            if sx >= 0 and sx < w and sy >= 0 and sy < text_y:
                el.append(px(sx, sy, sc, s))

        # ── Ghost position: sway side to side + bob up and down ──
        sway = int(8 * s * math.sin(f * 0.12))
        bob = int(2 * s * math.sin(f * 0.25))
        ghost_x = ghost_center_x + sway
        ghost_y = ghost_base_y + bob

        # Color cycle
        phase = (f % 60) / 60.0
        r, g, b = ghost_color(phase)

        # ── Ghost trail (3 fading copies offset behind movement) ──
        sway_dir = 1 if math.sin(f * 0.12 - 0.3) > math.sin(f * 0.12) else -1
        for trail in [3, 2, 1]:
            trail_x = ghost_x + sway_dir * trail * 2 * s
            trail_alpha = 0.08 + 0.07 * (3 - trail)
            draw_ghost(el, trail_x, ghost_y, s, r, g, b, trail_alpha)

        # ── Main ghost ──
        draw_ghost(el, ghost_x, ghost_y, s, r, g, b, 1.0)

        # ── Chromatic aberration "404" text ──
        offset = int(s * max(0, 1 + 1.5 * math.sin(f * 0.15)))

        # Red channel (shifted left)
        if offset > 0:
            el.append(render.Padding(
                pad = (max(0, text_x - offset), text_y, 0, 0),
                child = render.Text(content = text_404, color = "#cc0000", font = font),
            ))
            # Cyan channel (shifted right)
            el.append(render.Padding(
                pad = (text_x + offset, text_y, 0, 0),
                child = render.Text(content = text_404, color = "#00aaaa", font = font),
            ))

        # White main text (on top)
        el.append(render.Padding(
            pad = (text_x, text_y, 0, 0),
            child = render.Text(content = text_404, color = "#ffffff", font = font),
        ))

        # ── Scanline sweep ──
        scan_y = (f * 2 * s) % (h + 4 * s)
        if scan_y < h:
            el.append(render.Padding(
                pad = (0, scan_y, 0, 0),
                child = render.Box(width = w, height = s, color = "#0a0a18"),
            ))

        frames.append(render.Stack(children = el))

    return render.Root(delay = 100 // s, child = render.Animation(children = frames))
