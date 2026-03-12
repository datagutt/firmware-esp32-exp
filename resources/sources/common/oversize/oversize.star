load("render.star", "render", "canvas")

def main(config):
    scale = 2 if canvas.is2x() else 1

    title_font = "10x20" if canvas.is2x() else "6x13"
    sub_font = "6x13" if canvas.is2x() else "tom-thumb"

    colors = [
        "#f87171", "#e85858", "#d84040", "#c82828",
        "#d84040", "#e85858", "#f87171", "#ff8a8a",
        "#f87171", "#e85858",
    ]
    frames = []
    for i in range(len(colors)):
        frames.append(
            render.Box(
                color = "#000",
                child = render.Column(
                    expanded = True,
                    main_align = "center",
                    cross_align = "center",
                    children = [
                        render.Text("TOO", color = colors[i], font = sub_font),
                        render.Text("LARGE", color = colors[i], font = title_font),
                    ],
                ),
            ),
        )

    return render.Root(
        delay = 200 // scale,
        child = render.Animation(children = frames),
    )
