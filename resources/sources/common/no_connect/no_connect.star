load("render.star", "render", "canvas")
load("math.star", "math")

# No WiFi connection — large WiFi icon with X slash, pulsing.
# Closely matches matrx-resources setup.star WiFi arcs style.

WIFI_COLOR = "#fbbf24"
X_COLOR = "#f87171"

# WiFi arcs — matches the 3-arc + dot pattern from matrx-resources setup.star
# Relative to center-bottom dot of icon. Spans ~11 wide x 9 tall.
WIFI_PIXELS = [
    # Center dot
    (0, 0),
    # Inner arc (3 wide)
    (-1, -2), (0, -2), (1, -2),
    # Middle arc (5 wide)
    (-2, -4), (-1, -3), (0, -4), (1, -3), (2, -4),
    # Outer arc (9 wide)
    (-4, -6), (-3, -5), (-2, -6), (0, -6), (2, -6), (3, -5), (4, -6),
]

# Diagonal slash (top-right to bottom-left) across the WiFi icon
X_PIXELS = [
    (4, -7), (3, -6), (2, -5), (1, -4), (0, -3), (-1, -2), (-2, -1), (-3, 0),
    (4, -6), (3, -5), (2, -4), (1, -3), (0, -2), (-1, -1), (-2, 0), (-3, 1),
]

def to_hex(v):
    h = "0123456789abcdef"
    v = max(0, min(255, v))
    return h[v // 16] + h[v % 16]

def create_pixel(x, y, color, s):
    return render.Padding(
        pad = (x, y, 0, 0),
        child = render.Box(width = s, height = s, color = color),
    )

def main(config):
    scale = 2 if canvas.is2x() else 1
    w = canvas.width()
    h = canvas.height()

    font = "6x13" if scale == 1 else "10x20"
    char_w = 6 if scale == 1 else 10
    text_h = 13 if scale == 1 else 20

    text = "no wifi"
    text_x = (w - len(text) * char_w) // 2
    text_y = h - text_h - 2 * scale

    # Position WiFi icon center-dot in upper half, well above text
    icon_cx = w // 2
    icon_cy = (h - text_h - 2 * scale) // 2 + 4 * scale

    num_frames = 30
    frames = []

    for frame_idx in range(num_frames):
        elements = []

        # Pulse: dim and brighten the wifi icon
        pulse = (math.sin(frame_idx * 0.4) + 1) / 2
        brightness = int(100 + 155 * pulse)
        wifi_hex = "#" + to_hex(int(brightness * 0.98)) + to_hex(int(brightness * 0.75)) + to_hex(int(brightness * 0.14))

        # WiFi icon
        for dx, dy in WIFI_PIXELS:
            elements.append(create_pixel(icon_cx + dx * scale, icon_cy + dy * scale, wifi_hex, scale))

        # Red slash overlay (always full red, on top of wifi)
        for dx, dy in X_PIXELS:
            elements.append(create_pixel(icon_cx + dx * scale, icon_cy + dy * scale, X_COLOR, scale))

        # Text
        elements.append(
            render.Padding(
                pad = (text_x, text_y, 0, 0),
                child = render.Text(content = text, color = "#ffffff", font = font),
            ),
        )

        frames.append(render.Stack(children = elements))

    return render.Root(
        delay = 120 // scale,
        child = render.Animation(children = frames),
    )
