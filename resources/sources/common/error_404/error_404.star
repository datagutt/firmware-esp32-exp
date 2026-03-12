load("render.star", "render", "canvas")
load("math.star", "math")

# 404 Not Found — warning triangle with exclamation mark, flash animation.
# Triangle style matches matrx-resources factory_reset_hold.star warning icon.

WARN_COLOR = "#ff6600"

# Warning triangle outline (15 wide x 13 tall)
# Relative to top-left corner of icon bounding box
TRIANGLE_BORDER = [
    # Top point
    (7, 0),
    (6, 1), (7, 1), (8, 1),
    # Left edge
    (5, 2), (4, 3), (3, 4), (2, 5), (1, 6), (1, 7), (0, 8), (0, 9),
    (0, 10), (0, 11), (0, 12),
    # Right edge
    (9, 2), (10, 3), (11, 4), (12, 5), (13, 6), (13, 7), (14, 8), (14, 9),
    (14, 10), (14, 11), (14, 12),
    # Bottom edge
    (1, 12), (2, 12), (3, 12), (4, 12), (5, 12), (6, 12), (7, 12),
    (8, 12), (9, 12), (10, 12), (11, 12), (12, 12), (13, 12),
]

# Exclamation mark inside triangle (2px wide)
EXCLAIM_LINE = [
    (7, 4), (7, 5), (7, 6), (7, 7), (7, 8),
]
EXCLAIM_DOT = [
    (7, 10),
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

    text = "not found"
    text_x = (w - len(text) * char_w) // 2
    text_y = h - text_h - 2 * scale

    # Center the triangle icon in the space above the text
    icon_w = 15
    icon_h = 13
    icon_x = (w - icon_w * scale) // 2
    icon_y = (h - text_h - 2 * scale - icon_h * scale) // 2

    num_frames = 40
    frames = []

    for frame_idx in range(num_frames):
        elements = []

        # Flash effect: brief dim every ~20 frames (matches matrx style)
        flash = 1.0
        flash_frame = frame_idx % 20
        if flash_frame < 3:
            flash = 0.5 + 0.5 * (flash_frame / 3.0)

        brightness = int(255 * flash)
        color = "#" + to_hex(brightness) + to_hex(int(brightness * 0.4)) + to_hex(0)

        # Triangle border
        for dx, dy in TRIANGLE_BORDER:
            elements.append(create_pixel(icon_x + dx * scale, icon_y + dy * scale, color, scale))

        # Exclamation mark (white with flash)
        exc_b = int(255 * flash)
        exc_color = "#" + to_hex(exc_b) + to_hex(exc_b) + to_hex(exc_b)
        for dx, dy in EXCLAIM_LINE:
            elements.append(create_pixel(icon_x + dx * scale, icon_y + dy * scale, exc_color, scale))
        for dx, dy in EXCLAIM_DOT:
            elements.append(create_pixel(icon_x + dx * scale, icon_y + dy * scale, exc_color, scale))

        # Text
        elements.append(
            render.Padding(
                pad = (text_x, text_y, 0, 0),
                child = render.Text(content = text, color = "#ffffff", font = font),
            ),
        )

        frames.append(render.Stack(children = elements))

    return render.Root(
        delay = 80 // scale,
        child = render.Animation(children = frames),
    )
