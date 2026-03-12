load("render.star", "render", "canvas")
load("math.star", "math")

# Setup mode — Phone icon with animated screen, WiFi beacon with expanding
# signal rings, orbital spinner, and floating transfer dots.

PHONE_BORDER = "#212121"
PHONE_SCREEN = "#ffffff"
APP_BG = "#0d47a1"
WIFI_COLOR = "#00d4ff"
SPINNER_COLORS = ["#ffffff", "#dddddd", "#b9b9b9", "#959595", "#717171"]

# WiFi icon (3 arcs + dot, relative to center-bottom)
WIFI_PIXELS = [
    (0, 0),
    (-1, -2), (0, -2), (1, -2),
    (-2, -4), (-1, -3), (0, -4), (1, -3), (2, -4),
    (-4, -6), (-3, -5), (-2, -6), (0, -6), (2, -6), (3, -5), (4, -6),
]

# Spinner orbit path (32 positions)
SPINNER_PATH = [
    (7, 0), (7, -1), (7, -2), (7, -3), (6, -4), (6, -5), (5, -6), (4, -7),
    (3, -7), (2, -7), (1, -7), (0, -7), (-1, -7), (-2, -7), (-3, -7), (-4, -7),
    (-5, -6), (-6, -5), (-6, -4), (-7, -3), (-7, -2), (-7, -1), (-7, 0), (-7, 1),
    (-6, 2), (-5, 2), (-4, 2), (-3, 2), (-2, 2), (-1, 2), (0, 2), (6, 2),
]

# Transfer dot colors
DOT_COLORS = ["#00ffcc", "#ff44ff", "#ffff44", "#44aaff"]

# Sparkle positions (relative to WiFi center)
SPARKLES = [
    (-9, -9), (9, -8), (-10, 3), (10, -2), (-8, 4), (8, 5),
    (0, -10), (-11, -4), (11, -5), (-7, 6), (7, -10), (0, 5),
]

def to_hex(v):
    h = "0123456789abcdef"
    v = max(0, min(255, v))
    return h[v // 16] + h[v % 16]

def px(x, y, color, s):
    return render.Padding(pad = (x, y, 0, 0), child = render.Box(width = s, height = s, color = color))

def cos(x):
    return math.sin(x + 1.5708)

def draw_phone(el, px_x, py_y, s, f):
    """Phone icon (18x29 at 1x) with animated screen content"""

    # ── Phone shell (simplified from matrx setup.star) ──
    # Top rounded edge
    for x in range(2, 16):
        el.append(px(px_x + x * s, py_y + 0 * s, PHONE_BORDER, s))

    # Row 2 - border + white
    el.append(px(px_x + 1 * s, py_y + 1 * s, PHONE_BORDER, s))
    for x in range(2, 16):
        el.append(px(px_x + x * s, py_y + 1 * s, PHONE_SCREEN, s))
    el.append(px(px_x + 16 * s, py_y + 1 * s, PHONE_BORDER, s))

    # Row 3 - speaker notch
    el.append(px(px_x + 0 * s, py_y + 2 * s, PHONE_BORDER, s))
    for x in range(1, 7):
        el.append(px(px_x + x * s, py_y + 2 * s, PHONE_SCREEN, s))
    for x in range(7, 11):
        el.append(px(px_x + x * s, py_y + 2 * s, PHONE_BORDER, s))
    for x in range(11, 17):
        el.append(px(px_x + x * s, py_y + 2 * s, PHONE_SCREEN, s))
    el.append(px(px_x + 17 * s, py_y + 2 * s, PHONE_BORDER, s))

    # Row 4 - full white
    el.append(px(px_x + 0 * s, py_y + 3 * s, PHONE_BORDER, s))
    for x in range(1, 17):
        el.append(px(px_x + x * s, py_y + 3 * s, PHONE_SCREEN, s))
    el.append(px(px_x + 17 * s, py_y + 3 * s, PHONE_BORDER, s))

    # Row 5 - screen border top
    el.append(px(px_x + 0 * s, py_y + 4 * s, PHONE_BORDER, s))
    el.append(px(px_x + 1 * s, py_y + 4 * s, PHONE_SCREEN, s))
    for x in range(2, 16):
        el.append(px(px_x + x * s, py_y + 4 * s, PHONE_BORDER, s))
    el.append(px(px_x + 16 * s, py_y + 4 * s, PHONE_SCREEN, s))
    el.append(px(px_x + 17 * s, py_y + 4 * s, PHONE_BORDER, s))

    # Rows 6-22: app screen with animated color
    app_phase = f * 0.15
    for row in range(5, 22):
        el.append(px(px_x + 0 * s, py_y + row * s, PHONE_BORDER, s))
        el.append(px(px_x + 1 * s, py_y + row * s, PHONE_SCREEN, s))
        el.append(px(px_x + 2 * s, py_y + row * s, PHONE_BORDER, s))
        for x in range(3, 15):
            # Animated gradient on phone screen
            gradient = (math.sin(app_phase + row * 0.3) + 1) / 2
            r = int(13 + 30 * gradient)
            g = int(71 + 40 * gradient)
            b = int(161 + 40 * gradient)
            sc = "#" + to_hex(r) + to_hex(g) + to_hex(b)
            el.append(px(px_x + x * s, py_y + row * s, sc, s))
        el.append(px(px_x + 15 * s, py_y + row * s, PHONE_BORDER, s))
        el.append(px(px_x + 16 * s, py_y + row * s, PHONE_SCREEN, s))
        el.append(px(px_x + 17 * s, py_y + row * s, PHONE_BORDER, s))

    # Screen border bottom
    el.append(px(px_x + 0 * s, py_y + 22 * s, PHONE_BORDER, s))
    el.append(px(px_x + 1 * s, py_y + 22 * s, PHONE_SCREEN, s))
    for x in range(2, 16):
        el.append(px(px_x + x * s, py_y + 22 * s, PHONE_BORDER, s))
    el.append(px(px_x + 16 * s, py_y + 22 * s, PHONE_SCREEN, s))
    el.append(px(px_x + 17 * s, py_y + 22 * s, PHONE_BORDER, s))

    # White row below screen
    el.append(px(px_x + 0 * s, py_y + 23 * s, PHONE_BORDER, s))
    for x in range(1, 17):
        el.append(px(px_x + x * s, py_y + 23 * s, PHONE_SCREEN, s))
    el.append(px(px_x + 17 * s, py_y + 23 * s, PHONE_BORDER, s))

    # Home button rows
    for row in [24, 25]:
        el.append(px(px_x + 0 * s, py_y + row * s, PHONE_BORDER, s))
        for x in range(1, 7):
            el.append(px(px_x + x * s, py_y + row * s, PHONE_SCREEN, s))
        el.append(px(px_x + 7 * s, py_y + row * s, PHONE_BORDER, s))
        el.append(px(px_x + 8 * s, py_y + row * s, PHONE_SCREEN, s))
        el.append(px(px_x + 9 * s, py_y + row * s, PHONE_SCREEN, s))
        el.append(px(px_x + 10 * s, py_y + row * s, PHONE_BORDER, s))
        for x in range(11, 17):
            el.append(px(px_x + x * s, py_y + row * s, PHONE_SCREEN, s))
        el.append(px(px_x + 17 * s, py_y + row * s, PHONE_BORDER, s))

    # Bottom rounded edge
    el.append(px(px_x + 1 * s, py_y + 26 * s, PHONE_BORDER, s))
    for x in range(2, 16):
        el.append(px(px_x + x * s, py_y + 26 * s, PHONE_SCREEN, s))
    el.append(px(px_x + 16 * s, py_y + 26 * s, PHONE_BORDER, s))
    for x in range(2, 16):
        el.append(px(px_x + x * s, py_y + 27 * s, PHONE_BORDER, s))

def main(config):
    s = 2 if canvas.is2x() else 1
    w = canvas.width()
    h = canvas.height()

    num_frames = len(SPINNER_PATH)
    frames = []

    # Layout
    content_w = 51 * s
    offset_x = (w - content_w) // 2
    phone_x = offset_x
    phone_y = (h - 28 * s) // 2
    phone_right = phone_x + 18 * s

    wifi_cx = offset_x + 40 * s
    wifi_cy = h // 2 - 2 * s

    for f in range(num_frames):
        el = []

        # ── Phone with animated screen ──
        draw_phone(el, phone_x, phone_y, s, f)

        # ── Expanding signal rings from WiFi center ──
        ring_interval = 8
        max_radius = 12
        for ring_age_offset in range(0, num_frames, ring_interval):
            ring_age = (f - ring_age_offset) % num_frames
            if ring_age < 0 or ring_age > max_radius * 2:
                continue
            radius = ring_age * 0.6
            if radius < 1 or radius > max_radius:
                continue
            fade = max(0.0, 1.0 - radius / max_radius)
            bright = int(60 * fade)
            if bright < 5:
                continue
            rc = "#" + to_hex(0) + to_hex(int(bright * 0.85)) + to_hex(bright)
            # Draw arc (upper semicircle only — WiFi style)
            num_pts = max(12, int(radius * 6))
            for i in range(num_pts):
                angle = -3.14 + i * 3.14 / num_pts  # -180 to 0 degrees (upper half)
                rx = int(wifi_cx + radius * s * cos(angle))
                ry = int(wifi_cy + radius * s * math.sin(angle))
                if rx >= 0 and rx < w and ry >= 0 and ry < h:
                    el.append(px(rx, ry, rc, s))

        # ── WiFi icon ──
        for dx, dy in WIFI_PIXELS:
            el.append(px(wifi_cx + dx * s, wifi_cy + dy * s, WIFI_COLOR, s))

        # ── Orbital spinner ──
        n = len(SPINNER_PATH)
        for i in range(len(SPINNER_COLORS)):
            pos_idx = (f - i) % n
            dx, dy = SPINNER_PATH[pos_idx]
            el.append(px(wifi_cx + dx * s, wifi_cy + dy * s, SPINNER_COLORS[i], s))

        # ── Transfer dots (phone → WiFi) ──
        for di, dc in enumerate(DOT_COLORS):
            dot_phase = ((f * 3 + di * num_frames // len(DOT_COLORS)) % (num_frames * 2))
            progress = dot_phase / (num_frames * 2.0)
            if progress > 1.0:
                continue
            dot_x = int(phone_right + (wifi_cx - phone_right) * progress)
            dot_y_base = (phone_y + 14 * s + wifi_cy) // 2
            dot_y = int(dot_y_base + 3 * s * math.sin(progress * 6.283 * 2))
            if dot_x >= phone_right and dot_x < wifi_cx and progress > 0.05 and progress < 0.95:
                el.append(px(dot_x, dot_y, dc, s))

        # ── Sparkles ──
        for si, (sx, sy) in enumerate(SPARKLES):
            # Each sparkle twinkles at its own phase
            sparkle_phase = (f + si * 5) % 12
            if sparkle_phase < 2:
                bright = 180 if sparkle_phase == 0 else 100
                sc = "#" + to_hex(bright) + to_hex(bright) + to_hex(bright)
                spx = wifi_cx + sx * s
                spy = wifi_cy + sy * s
                if spx >= 0 and spx < w and spy >= 0 and spy < h:
                    el.append(px(spx, spy, sc, s))

        frames.append(render.Stack(children = el))

    return render.Root(delay = 50 // s, child = render.Animation(children = frames))
