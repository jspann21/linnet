"""Pack Linnet's licensed sans font for the native Boot choosers."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "sdk/assets/font-sans14.json"
COLOR320_TEXT = (
    "Choose startup", "Checking startup files", "Starting", "Linnet",
    "Rockbox", "Apple iPod software", "Wheel: choose", "Center: start",
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--color320", action="store_true", help="Pack only the 320-pixel chooser glyphs")
    args = parser.parse_args()
    font = json.loads(SOURCE.read_text(encoding="utf-8"))
    width, height = font["width"], font["height"]
    stride = (width * height + 3) // 4
    characters = sorted({ord(c) - 32 for text in COLOR320_TEXT for c in text}) if args.color320 else list(range(95))
    rows = []
    for glyph in characters:
        offset = glyph * width * height
        samples = font["alpha"][offset:offset + width * height]
        levels = [0 if a < 48 else 1 if a < 128 else 2 if a < 208 else 3
                  for a in samples]
        levels.extend([0] * (stride * 4 - len(levels)))
        rows.append([sum(levels[i + j] << (6 - j * 2) for j in range(4))
                     for i in range(0, stride * 4, 4)])
    lines = ["/* Generated from sdk/assets/font-sans14.json; see sdk/assets/FONT-LICENSE.txt. */",
             f"#define LINNET_BOOT_MENU_FONT_WIDTH {width}u",
             f"#define LINNET_BOOT_MENU_FONT_HEIGHT {height}u",
             f"#define LINNET_BOOT_MENU_FONT_STRIDE {stride}u",
             "static const uint8_t ln_boot_menu_advances[95] = {"]
    advances = font["advances"]
    lines.extend("    " + ", ".join(f"{n}u" for n in advances[i:i + 16]) + ","
                 for i in range(0, 95, 16))
    if args.color320:
        positions = {glyph: index for index, glyph in enumerate(characters)}
        lookup = [positions.get(glyph, 255) for glyph in range(95)]
        lines.append("};\nstatic const uint8_t ln_boot_menu_index[95] = {")
        lines.extend("    " + ", ".join(f"{n}u" for n in lookup[i:i + 16]) + ","
                     for i in range(0, 95, 16))
    lines.append(f"}};\nstatic const uint8_t ln_boot_menu_font[{len(rows)}][LINNET_BOOT_MENU_FONT_STRIDE] = {{")
    for row in rows:
        lines.append("    {" + ", ".join(f"0x{n:02x}u" for n in row) + "},")
    lines.append("};\n")
    args.output.write_text("\n".join(lines), encoding="ascii")
    print(f"{len(rows) * stride + len(advances)} packed font bytes")


if __name__ == "__main__":
    main()
