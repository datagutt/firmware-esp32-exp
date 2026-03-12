load("render.star", "render", "canvas")
load("math.star", "math")

# Setup mode screen — phone icon + WiFi symbol with rotating spinner.
# Closely matches matrx-resources setup.star layout and visual style.

ACCENT = "#00d4ff"
PHONE_BORDER = "#212121"
PHONE_SCREEN = "#ffffff"
APP_BG = "#0d47a1"
WIFI_COLOR = "#00d4ff"

SPINNER_COLORS = ["#ffffff", "#dddddd", "#b9b9b9", "#959595", "#717171"]

# WiFi icon: 3 arcs + center dot (matches matrx-resources setup BT_COLOR layout)
# Relative to center-bottom of icon
WIFI_PIXELS = [
    # Center dot
    (0, 0),
    # Inner arc
    (-1, -2), (0, -2), (1, -2),
    # Middle arc
    (-2, -4), (-1, -3), (0, -4), (1, -3), (2, -4),
    # Outer arc
    (-4, -6), (-3, -5), (-2, -6), (0, -6), (2, -6), (3, -5), (4, -6),
]

# Spinner path around WiFi symbol — matches matrx-resources setup.star SPINNER_PATH_RELATIVE
# but scaled for the WiFi icon size (smaller orbit than the BT version)
SPINNER_PATH = [
    (7, 0), (7, -1), (7, -2), (7, -3), (6, -4), (6, -5), (5, -6), (4, -7),
    (3, -7), (2, -7), (1, -7), (0, -7), (-1, -7), (-2, -7), (-3, -7), (-4, -7),
    (-5, -6), (-6, -5), (-6, -4), (-7, -3), (-7, -2), (-7, -1), (-7, 0), (-7, 1),
    (-6, 2), (-5, 2), (-4, 2), (-3, 2), (-2, 2), (-1, 2), (0, 2), (6, 2),
]

def create_pixel(x, y, color, s):
    return render.Padding(
        pad = (x, y, 0, 0),
        child = render.Box(width = s, height = s, color = color),
    )

def create_phone_icon(px, py, s):
    """Phone icon closely matching matrx-resources setup.star (18x30 at 1x)"""
    elements = []

    # Phone dimensions at 1x: 18 wide, 30 tall
    w = 18
    h = 30

    # Top rounded edge
    for x in range(2, 16):
        elements.append(create_pixel(px + x * s, py + 0 * s, PHONE_BORDER, s))

    # Row 2 - white inset
    elements.append(create_pixel(px + 1 * s, py + 1 * s, PHONE_BORDER, s))
    for x in range(2, 16):
        elements.append(create_pixel(px + x * s, py + 1 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 16 * s, py + 1 * s, PHONE_BORDER, s))

    # Row 3 - speaker notch
    elements.append(create_pixel(px + 0 * s, py + 2 * s, PHONE_BORDER, s))
    elements.append(create_pixel(px + 1 * s, py + 2 * s, PHONE_SCREEN, s))
    for x in range(2, 7):
        elements.append(create_pixel(px + x * s, py + 2 * s, PHONE_SCREEN, s))
    for x in range(7, 11):
        elements.append(create_pixel(px + x * s, py + 2 * s, PHONE_BORDER, s))
    for x in range(11, 16):
        elements.append(create_pixel(px + x * s, py + 2 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 16 * s, py + 2 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 17 * s, py + 2 * s, PHONE_BORDER, s))

    # Row 4 - full white
    elements.append(create_pixel(px + 0 * s, py + 3 * s, PHONE_BORDER, s))
    for x in range(1, 17):
        elements.append(create_pixel(px + x * s, py + 3 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 17 * s, py + 3 * s, PHONE_BORDER, s))

    # Row 5 - screen border top
    elements.append(create_pixel(px + 0 * s, py + 4 * s, PHONE_BORDER, s))
    elements.append(create_pixel(px + 1 * s, py + 4 * s, PHONE_SCREEN, s))
    for x in range(2, 16):
        elements.append(create_pixel(px + x * s, py + 4 * s, PHONE_BORDER, s))
    elements.append(create_pixel(px + 16 * s, py + 4 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 17 * s, py + 4 * s, PHONE_BORDER, s))

    # Rows 6-23: app screen area (blue background)
    for row in range(5, 23):
        elements.append(create_pixel(px + 0 * s, py + row * s, PHONE_BORDER, s))
        elements.append(create_pixel(px + 1 * s, py + row * s, PHONE_SCREEN, s))
        elements.append(create_pixel(px + 2 * s, py + row * s, PHONE_BORDER, s))
        for x in range(3, 15):
            elements.append(create_pixel(px + x * s, py + row * s, APP_BG, s))
        elements.append(create_pixel(px + 15 * s, py + row * s, PHONE_BORDER, s))
        elements.append(create_pixel(px + 16 * s, py + row * s, PHONE_SCREEN, s))
        elements.append(create_pixel(px + 17 * s, py + row * s, PHONE_BORDER, s))

    # Screen border bottom
    elements.append(create_pixel(px + 0 * s, py + 23 * s, PHONE_BORDER, s))
    elements.append(create_pixel(px + 1 * s, py + 23 * s, PHONE_SCREEN, s))
    for x in range(2, 16):
        elements.append(create_pixel(px + x * s, py + 23 * s, PHONE_BORDER, s))
    elements.append(create_pixel(px + 16 * s, py + 23 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 17 * s, py + 23 * s, PHONE_BORDER, s))

    # Row 25 - white
    elements.append(create_pixel(px + 0 * s, py + 24 * s, PHONE_BORDER, s))
    for x in range(1, 17):
        elements.append(create_pixel(px + x * s, py + 24 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 17 * s, py + 24 * s, PHONE_BORDER, s))

    # Rows 26-27 - home button
    for row in range(25, 27):
        elements.append(create_pixel(px + 0 * s, py + row * s, PHONE_BORDER, s))
        for x in range(1, 7):
            elements.append(create_pixel(px + x * s, py + row * s, PHONE_SCREEN, s))
        elements.append(create_pixel(px + 7 * s, py + row * s, PHONE_BORDER, s))
        elements.append(create_pixel(px + 8 * s, py + row * s, PHONE_SCREEN, s))
        elements.append(create_pixel(px + 9 * s, py + row * s, PHONE_SCREEN, s))
        elements.append(create_pixel(px + 10 * s, py + row * s, PHONE_BORDER, s))
        for x in range(11, 17):
            elements.append(create_pixel(px + x * s, py + row * s, PHONE_SCREEN, s))
        elements.append(create_pixel(px + 17 * s, py + row * s, PHONE_BORDER, s))

    # Bottom rows
    elements.append(create_pixel(px + 1 * s, py + 27 * s, PHONE_BORDER, s))
    for x in range(2, 16):
        elements.append(create_pixel(px + x * s, py + 27 * s, PHONE_SCREEN, s))
    elements.append(create_pixel(px + 16 * s, py + 27 * s, PHONE_BORDER, s))

    # Bottom border
    for x in range(2, 16):
        elements.append(create_pixel(px + x * s, py + 28 * s, PHONE_BORDER, s))

    return elements

def create_wifi_icon(cx, cy, s):
    """WiFi icon at center position"""
    elements = []
    for dx, dy in WIFI_PIXELS:
        elements.append(create_pixel(cx + dx * s, cy + dy * s, WIFI_COLOR, s))
    return elements

def create_spinner(frame_idx, cx, cy, s):
    """Rotating 5-pixel gradient spinner — matches matrx-resources"""
    elements = []
    n = len(SPINNER_PATH)
    for i in range(len(SPINNER_COLORS)):
        pos_idx = (frame_idx - i) % n
        dx, dy = SPINNER_PATH[pos_idx]
        elements.append(create_pixel(cx + dx * s, cy + dy * s, SPINNER_COLORS[i], s))
    return elements

def main(config):
    scale = 2 if canvas.is2x() else 1
    w = canvas.width()
    h = canvas.height()

    num_frames = len(SPINNER_PATH)
    frames = []

    # Layout: phone on left, wifi + spinner on right (matches matrx-resources setup.star)
    # Center the content vertically and horizontally
    content_w = 51 * scale  # phone (18) + gap + wifi orbit (15) roughly
    offset_x = (w - content_w) // 2
    phone_x = offset_x
    phone_y = (h - 29 * scale) // 2

    wifi_cx = offset_x + 40 * scale
    wifi_cy = h // 2 - 2 * scale

    for frame_idx in range(num_frames):
        elements = []
        elements.extend(create_phone_icon(phone_x, phone_y, scale))
        elements.extend(create_wifi_icon(wifi_cx, wifi_cy, scale))
        elements.extend(create_spinner(frame_idx, wifi_cx, wifi_cy, scale))
        frames.append(render.Stack(children = elements))

    return render.Root(
        delay = 50 // scale,
        child = render.Animation(children = frames),
    )
