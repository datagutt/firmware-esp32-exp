load("render.star", "render", "canvas")

def main(config):
    scale = 2 if canvas.is2x() else 1

    title_font = "10x20" if canvas.is2x() else "6x13"
    sub_font = "6x13" if canvas.is2x() else "tom-thumb"

    return render.Root(
        child = render.Box(
            color = "#000",
            child = render.Stack(
                children = [
                    render.Box(
                        width = canvas.width(),
                        height = 1 * scale,
                        color = "#fbbf24",
                    ),
                    render.Column(
                        expanded = True,
                        main_align = "center",
                        cross_align = "center",
                        children = [
                            render.Text("NO", color = "#fbbf24", font = sub_font),
                            render.Text("WIFI", color = "#fbbf24", font = title_font),
                        ],
                    ),
                ],
            ),
        ),
    )
