load("render.star", "render", "canvas")

def main(config):
    scale = 2 if canvas.is2x() else 1

    title_font = "10x20" if canvas.is2x() else "6x13"
    sub_font = "6x13" if canvas.is2x() else "tom-thumb"

    frames = []
    for i in range(20):
        blink_on = (i % 4) < 2
        frames.append(
            render.Box(
                color = "#000",
                child = render.Column(
                    expanded = True,
                    main_align = "center",
                    cross_align = "center",
                    children = [
                        render.Text("404", color = "#f87171", font = title_font),
                        render.Box(width = 1, height = 2 * scale),
                        render.Text(
                            "NOT FOUND",
                            color = "#888" if blink_on else "#444",
                            font = sub_font,
                        ),
                    ],
                ),
            ),
        )

    return render.Root(
        delay = 150 // scale,
        child = render.Animation(children = frames),
    )
