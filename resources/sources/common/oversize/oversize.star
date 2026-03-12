load("render.star", "render", "canvas")
load("math.star", "math")

# Image too large — bouncing download arrow with X block indicator.
# Arrow style matches matrx-resources check_updates.star download arrow.

ARROW_COLOR = "#f87171"

# Arrow head — 5-point V shape pointing down (matches check_updates arrowhead)
ARROWHEAD = [
    (-4, -2), (-3, -1), (-2, 0), (-1, 1), (0, 2),
    (4, -2), (3, -1), (2, 0), (1, 1),
]

# X block indicator across the arrow shaft
X_BLOCK = [
    (-3, -5), (-2, -4), (2, -4), (3, -5),
    (-2, -6), (2, -6),
    (-3, -7), (3, -7),
]

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

    text = "too large"
    text_x = (w - len(text) * char_w) // 2
    text_y = h - text_h - 2 * scale

    arrow_cx = w // 2
    # Center the arrow in the space above the text
    arrow_cy = (h - text_h - 2 * scale) // 2 + 2 * scale

    num_frames = 40
    frames = []

    for frame_idx in range(num_frames):
        elements = []

        # Bounce animation (matches check_updates.star: 3 * sin(frame * 0.3))
        bounce = int(3 * scale * math.sin(frame_idx * 0.3))

        # Arrow shaft (8px tall vertical line)
        for i in range(8):
            elements.append(create_pixel(
                arrow_cx, arrow_cy - 8 * scale + i * scale + bounce,
                ARROW_COLOR, scale,
            ))

        # Arrow head (V shape pointing down)
        for dx, dy in ARROWHEAD:
            elements.append(create_pixel(
                arrow_cx + dx * scale, arrow_cy + dy * scale + bounce,
                ARROW_COLOR, scale,
            ))

        # X block indicator (white, shows "blocked/too large")
        for dx, dy in X_BLOCK:
            elements.append(create_pixel(
                arrow_cx + dx * scale, arrow_cy + dy * scale + bounce,
                "#ffffff", scale,
            ))

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
