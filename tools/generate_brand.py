"""Generate CRML's font-independent SVG assets with Python's standard library."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "docs" / "assets" / "brand"
PALETTES = {
    "dark": ("#edf2f8", "#a5c0e5", "#7898c3"),
    "light": ("#293342", "#315c8c", "#416c9c"),
    "mono-black": ("#000000",) * 3,
    "mono-white": ("#ffffff",) * 3,
}
# Geometric outlines keep the lettering identical without installed fonts.
LETTERS = {
    "A": "M0 24 8 0 16 24M3 16H13",
    "C": "M16 0H4L0 4V20L4 24H16",
    "D": "M0 0V24H10L16 18V6L10 0Z",
    "E": "M16 0H0V24H16M0 12H12",
    "L": "M0 0V24H16",
    "M": "M0 24V0L8 12 16 0V24",
    "N": "M0 24V0L16 24V0",
    "O": "M4 0H12L16 4V20L12 24H4L0 20V4Z",
    "R": "M0 24V0H12L16 4V8L12 12H0M8 12 16 24",
    "S": "M16 0H4L0 4V8L4 12H12L16 16V20L12 24H0",
    "T": "M0 0H16M8 0V24",
}


def lettering(text, x, y, scale, color):
    parts = []
    for i, char in enumerate(text):
        if char != " ":
            parts.append(f'<path transform="translate({i * 23} 0)" d="{LETTERS[char]}"/>')
    return (f'<g transform="translate({x} {y}) scale({scale})" fill="none" '
            f'stroke="{color}" stroke-width="2.5" stroke-linejoin="miter">'
            + "".join(parts) + '</g>')


def mark(ink, accent, echo):
    return (f'<path d="M40 11H25L10 26v12l15 15h15v-9H29L19 34v-4l10-10h11z" fill="{accent}"/>'
            f'<path d="m39 23 9 9-9 9-9-9z" fill="{ink}"/>'
            f'<path d="m49 21 11 11-11 11v-9l2-2-2-2z" fill="{echo}"/>')


def svg(layout, palette, tile=False):
    ink, accent, echo = PALETTES[palette]
    symbol = mark(ink, accent, echo)
    if layout == "icon":
        width, height = 64, 64
        content = ('<rect width="64" height="64" rx="12" fill="#20252e"/>' if tile else '') + symbol
    elif layout == "logo":
        width, height = 440, 112
        content = ('<g transform="translate(0 8) scale(1.5)">' + symbol + '</g>'
                   + lettering("CRML", 120, 15, 1.75, ink)
                   + lettering("CONTROL RESONANT", 121, 72, 0.75, accent)
                   + lettering("MOD LOADER", 121, 97, 0.38, ink))
    else:
        width, height = 320, 248
        content = ('<g transform="translate(90 0) scale(2)">' + symbol + '</g>'
                   + lettering("CRML", 86, 140, 1.75, ink)
                   + lettering("CONTROL RESONANT", 25, 204, 0.75, accent)
                   + lettering("MOD LOADER", 118, 232, 0.38, ink))
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}" role="img" aria-labelledby="title">\n'
            '<title id="title">CONTROL Resonant Mod Loader</title>\n'
            + content + '\n</svg>\n')


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for layout in ("icon", "logo", "stacked"):
        for palette in PALETTES:
            (OUTPUT / f"{layout}-{palette}.svg").write_text(svg(layout, palette), encoding="utf-8")
    favicon = svg("icon", "dark", tile=True)
    (OUTPUT / "favicon.svg").write_text(favicon, encoding="utf-8")
    # Preserve the original public URL used by the README and documentation.
    (OUTPUT.parent / "logo.svg").write_text(favicon, encoding="utf-8")
    print("Generated 13 brand assets and the logo.svg compatibility copy.")


if __name__ == "__main__":
    main()
