#!/usr/bin/env python3
"""Wimsy's colours for the Gatus UI: the two custom-css lines of gatus.yaml.in, made from ScarletNX's
dist/wimsy.svg. Gatus draws with shadcn's variables, "H S% L%"; only the primary colour (buttons, links,
the focus ring) and the text on it are Wimsy's, the rest stays Gatus's own.

Light: the darkest petal (#0c8694) under white text. Dark: the head (#3ae8ef) under black text. A colour
whose text would read under WCAG's 4.5:1 is darkened (light) or lightened (dark) a tenth of a point of
lightness at a time until it reads, and stderr says so.

    colours.py WIMSY_SVG            prints the two lines
    colours.py WIMSY_SVG --check    fails when gatus.yaml.in holds other lines
    colours.py WIMSY_SVG --update   writes them into gatus.yaml.in (then ./render.sh)
"""
import colorsys
import pathlib
import re
import sys

LIGHT_FILL, DARK_FILL = "#0c8694", "#3ae8ef"
WHITE, BLACK = (1.0, 1.0, 1.0), (0.0, 0.0, 0.0)
TEMPLATE = pathlib.Path(__file__).with_name("gatus.yaml.in")
LINE = re.compile(r"^    html:root(\.dark)?\{--primary:.*\}$", re.M)


def rgb(hex6):
    return tuple(int(hex6[i:i + 2], 16) / 255 for i in (1, 3, 5))


def luminance(c):
    lin = [v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in c]
    return 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2]


def contrast(a, b):
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def readable(hex6, text, step):
    """The colour as (h, s, l) in 0..1, its lightness moved by step until text on it reads at 4.5:1."""
    h, l, s = colorsys.rgb_to_hls(*rgb(hex6))
    l0 = l
    while contrast(colorsys.hls_to_rgb(h, l, s), text) < 4.5:
        l = round(l + step, 4)
    if l != l0:
        print(f"{hex6}: lightness {l0 * 100:.1f}% -> {l * 100:.1f}%, for 4.5:1 with its text", file=sys.stderr)
    return h, s, l


def hsl(h, s, l):
    return f"{h * 360:.1f} {s * 100:.1f}% {l * 100:.1f}%"


def lines(svg):
    fills = set(f.lower() for f in re.findall(r"fill:(#[0-9a-fA-F]{6})", svg.read_text()))
    for fill in (LIGHT_FILL, DARK_FILL):
        if fill not in fills:
            sys.exit(f"{svg} has no fill {fill}: Wimsy changed, choose her colours again")
    light = hsl(*readable(LIGHT_FILL, WHITE, -0.001))
    dark = hsl(*readable(DARK_FILL, BLACK, +0.001))
    return [
        f"    html:root{{--primary:{light};--primary-foreground:0.0 0.0% 100.0%;--ring:{light}}}",
        f"    html:root.dark{{--primary:{dark};--primary-foreground:0.0 0.0% 0.0%;--ring:{dark}}}",
    ]


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] not in ("--check", "--update")):
        sys.exit(__doc__)
    want = lines(pathlib.Path(sys.argv[1]))
    if len(sys.argv) == 2:
        print("\n".join(want))
        return
    text = TEMPLATE.read_text()
    have = [m.group(0) for m in LINE.finditer(text)]
    if have == want:
        return
    if sys.argv[2] == "--check":
        sys.exit("gatus.yaml.in's colour lines are not Wimsy's: colours.py WIMSY_SVG --update, then ./render.sh\n"
                 + "\n".join(["have:"] + (have or ["(none)"]) + ["want:"] + want))
    if len(have) != 2:
        sys.exit("gatus.yaml.in has no two html:root{--primary:...} lines to replace")
    for old, new in zip(have, want):
        text = text.replace(old, new)
    TEMPLATE.write_text(text)


if __name__ == "__main__":
    main()
