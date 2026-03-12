load("render.star", "render", "canvas")

# Party Parrot boot animation — pixel art parrot that bobs and cycles rainbow colors.

RAINBOW = [
    "#ff0000",  # red
    "#ff8000",  # orange
    "#ffff00",  # yellow
    "#00ff00",  # green
    "#00ffff",  # cyan
    "#0080ff",  # blue
    "#8000ff",  # purple
    "#ff00ff",  # magenta
    "#ff0080",  # pink
    "#ff4040",  # light red
]

# Parrot sprite: 15 wide x 15 tall
# B=body, E=eye(white), P=pupil(black), K=beak(orange), W=wing(darker), 0=transparent
PARROT = [
    "000000BBB000000",
    "00000BBBBB00000",
    "0000BBBBBBB0000",
    "000BBBBEBBBB000",
    "000BBBPEBBBKK00",
    "00BBBBBBBBBKKK0",
    "00BBBBBBBBBBKK0",
    "0BBBBBBBBBBBBB0",
    "0BBBBBBBBBBBB00",
    "0BWWWBBBBBBB000",
    "00WWWWBBBBBB000",
    "000WWWWBBBBB000",
    "0000WWWWBBBB000",
    "00000BBBBB00000",
    "000000BBB000000",
]

BOBS = [0, -1, -2, -1, 0, 1, 2, 1, 0, -1]

def make_pixel(x, y, color, px_size):
    return render.Padding(
        pad = (x, y, 0, 0),
        child = render.Box(width = px_size, height = px_size, color = color),
    )

def make_frame(color_idx, bob, px_size, offset_x, offset_y):
    body = RAINBOW[color_idx % len(RAINBOW)]
    wing = RAINBOW[(color_idx + 5) % len(RAINBOW)]
    beak = "#ff8c00"
    eye_white = "#ffffff"
    pupil = "#000000"

    pixels = []
    for row_idx in range(len(PARROT)):
        row = PARROT[row_idx]
        for col_idx in range(len(row)):
            ch = row[col_idx]
            if ch == "0":
                continue
            x = offset_x + col_idx * px_size
            y = offset_y + row_idx * px_size + bob * px_size
            if ch == "B":
                pixels.append(make_pixel(x, y, body, px_size))
            elif ch == "E":
                pixels.append(make_pixel(x, y, eye_white, px_size))
            elif ch == "P":
                pixels.append(make_pixel(x, y, pupil, px_size))
            elif ch == "K":
                pixels.append(make_pixel(x, y, beak, px_size))
            elif ch == "W":
                pixels.append(make_pixel(x, y, wing, px_size))

    return render.Stack(
        children = [
            render.Box(color = "#000"),
        ] + pixels,
    )

def main(config):
    scale = 2 if canvas.is2x() else 1
    px_size = 2 * scale

    w = canvas.width()
    h = canvas.height()

    sprite_w = 15 * px_size
    sprite_h = 15 * px_size
    offset_x = (w - sprite_w) // 2
    offset_y = (h - sprite_h) // 2

    frames = []
    for i in range(len(RAINBOW)):
        bob = BOBS[i % len(BOBS)]
        frames.append(make_frame(i, bob, px_size, offset_x, offset_y))

    return render.Root(
        delay = 120 // scale,
        child = render.Animation(children = frames),
    )
