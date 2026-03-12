load("render.star", "render", "canvas")
load("math.star", "math")

# Image too large — Red download arrow descends, slams into barrier,
# explodes into rainbow particles with gravity. "too large" fades in.

def to_hex(v):
    h = "0123456789abcdef"
    v = max(0, min(255, v))
    return h[v // 16] + h[v % 16]

def px(x, y, color, s):
    return render.Padding(pad = (x, y, 0, 0), child = render.Box(width = s, height = s, color = color))

# Arrow pieces (relative to arrow tip = bottom center)
ARROW_SHAFT = [(0, -i) for i in range(1, 9)]
ARROWHEAD = [
    (-4, -2), (-3, -1), (-2, 0), (-1, 1), (0, 2),
    (4, -2), (3, -1), (2, 0), (1, 1),
]

# Explosion particles: (vx_x10, vy_x10, r, g, b)
PARTICLES = [
    (-30, -55, 255, 60, 60),
    (-18, -62, 255, 160, 0),
    (-8, -68, 255, 255, 0),
    (0, -72, 80, 255, 80),
    (8, -68, 0, 255, 220),
    (18, -62, 60, 140, 255),
    (30, -55, 220, 60, 255),
    (-38, -38, 255, 100, 150),
    (38, -38, 150, 255, 100),
    (-14, -52, 255, 200, 50),
    (14, -52, 50, 200, 255),
    (-42, -28, 255, 50, 200),
    (42, -28, 200, 255, 50),
    (0, -78, 255, 255, 255),
    (-6, -48, 255, 180, 80),
    (6, -48, 80, 180, 255),
    (-24, -32, 255, 120, 220),
    (24, -32, 220, 255, 120),
    (-3, -60, 255, 220, 160),
    (3, -60, 160, 220, 255),
]

IMPACT_FRAME = 12

def main(config):
    s = 2 if canvas.is2x() else 1
    w = canvas.width()
    h = canvas.height()

    font = "6x13" if s == 1 else "10x20"
    char_w = 6 if s == 1 else 10
    text_h = 13 if s == 1 else 20

    text = "too large"
    text_x = (w - len(text) * char_w) // 2
    text_y = h - text_h - 2 * s

    cx = w // 2
    # Impact point: center of icon area
    impact_y = text_y // 2 + 2 * s
    # Arrow starts at top of screen (visible from frame 0)
    arrow_start_y = 2 * s

    total_frames = 45
    frames = []

    for f in range(total_frames):
        el = []

        if f <= IMPACT_FRAME:
            # ── Phase 1: Arrow descends from top ──
            progress = f / max(1, IMPACT_FRAME)
            # Ease-in: accelerate as it falls
            eased = progress * progress
            arrow_tip_y = int(arrow_start_y + (impact_y - arrow_start_y) * eased)

            # Arrow color intensifies (redder as it speeds up)
            bright = int(180 + 75 * progress)
            ac = "#" + to_hex(bright) + to_hex(int(bright * 0.25)) + to_hex(int(bright * 0.2))

            # Arrow head
            for dx, dy in ARROWHEAD:
                py = arrow_tip_y + dy * s
                ppx = cx + dx * s
                if py >= 0 and py < h and ppx >= 0 and ppx < w:
                    el.append(px(ppx, py, ac, s))

            # Arrow shaft
            for dx, dy in ARROW_SHAFT:
                py = arrow_tip_y + dy * s
                if py >= 0 and py < h:
                    el.append(px(cx + dx * s, py, ac, s))

            # Speed lines (trailing streaks)
            num_lines = int(3 * progress)
            for i in range(num_lines):
                trail_y = arrow_tip_y - (10 + i * 4) * s
                if trail_y >= 0:
                    tb = int(60 * (1 - i / 3.0) * progress)
                    tc = "#" + to_hex(tb) + to_hex(int(tb * 0.2)) + to_hex(int(tb * 0.2))
                    el.append(px(cx, trail_y, tc, s))

        elif f <= IMPACT_FRAME + 2:
            # ── Phase 2: Impact flash ──
            flash_age = f - IMPACT_FRAME
            flash_r = (8 - flash_age * 2) * s
            flash_b_max = 255 if flash_age == 1 else 140
            for fy in range(max(0, impact_y - flash_r), min(text_y, impact_y + flash_r)):
                for fx in range(max(0, cx - flash_r), min(w, cx + flash_r)):
                    dist = abs(fx - cx) + abs(fy - impact_y)
                    if dist < flash_r:
                        fb = int(flash_b_max * (1.0 - dist / flash_r))
                        fc = "#" + to_hex(fb) + to_hex(fb) + to_hex(int(min(255, fb * 1.2)))
                        el.append(px(fx, fy, fc, s))

        # ── Phase 3: Particles with gravity ──
        if f > IMPACT_FRAME:
            t = f - IMPACT_FRAME
            gravity = 3.5

            for vx10, vy10, pr, pg, pb in PARTICLES:
                ppx = int(cx + vx10 * t * s / 100.0)
                ppy = int(impact_y + vy10 * t * s / 100.0 + gravity * t * t * s / 100.0)

                fade = max(0.0, 1.0 - t / 28.0)
                if fade <= 0 or ppy >= text_y or ppy < 0 or ppx < 0 or ppx >= w:
                    continue

                cr = to_hex(int(pr * fade))
                cg = to_hex(int(pg * fade))
                cb = to_hex(int(pb * fade))
                el.append(px(ppx, ppy, "#" + cr + cg + cb, s))

        # ── Barrier line at impact point (appears after impact) ──
        if f > IMPACT_FRAME and f < IMPACT_FRAME + 15:
            bar_fade = min(1.0, (f - IMPACT_FRAME) / 3.0) * max(0.0, 1.0 - (f - IMPACT_FRAME - 5) / 10.0)
            if bar_fade > 0:
                bb = int(80 * bar_fade)
                bc = "#" + to_hex(bb) + to_hex(int(bb * 0.3)) + to_hex(int(bb * 0.3))
                bar_w = min(w, 20 * s)
                bar_x = (w - bar_w) // 2
                for bx in range(bar_x, bar_x + bar_w):
                    el.append(px(bx, impact_y + 3 * s, bc, s))

        # ── Text fades in ──
        if f > IMPACT_FRAME + 3:
            text_fade = min(1.0, (f - IMPACT_FRAME - 3) / 10.0)
            tb = int(255 * text_fade)
            tc = "#" + to_hex(tb) + to_hex(tb) + to_hex(tb)
            el.append(render.Padding(
                pad = (text_x, text_y, 0, 0),
                child = render.Text(content = text, color = tc, font = font),
            ))

        frames.append(render.Stack(children = el))

    return render.Root(delay = 80 // s, child = render.Animation(children = frames))
