"""Generate compact, native-resolution Disk Mode images from licensed fonts."""

from __future__ import annotations

import argparse
import json
from math import hypot, sqrt
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
PAPER = (249, 251, 252)
HEADER = (232, 239, 243)
INK = (34, 49, 60)
MUTED = (91, 109, 120)
ACCENT = (172, 74, 69)


def rgb565(red: int, green: int, blue: int) -> int:
    return (red >> 3) << 11 | (green >> 2) << 5 | (blue >> 3)


def components(value: int) -> tuple[int, int, int]:
    return (((value >> 11) & 31) * 255 // 31,
            ((value >> 5) & 63) * 255 // 63,
            (value & 31) * 255 // 31)


def blend(background: int, foreground: tuple[int, int, int], alpha: int) -> int:
    return rgb565(*(b + ((f - b) * alpha + 127) // 255
                    for b, f in zip(components(background), foreground)))


def font(name: str) -> dict:
    return json.loads((ROOT / "sdk" / "assets" / name).read_text(encoding="utf-8"))


def draw_text(pixels: list[int], width: int, height: int, label: str, glyphs: dict,
              top: int, scale: float, color: tuple[int, int, int]) -> None:
    advances = glyphs["advances"]
    glyph_width, glyph_height = glyphs["width"], glyphs["height"]
    native_width = sum(advances[ord(char) - 32] for char in label)
    mask = Image.new("L", (native_width, glyph_height))
    left = 0
    for char in label:
        index = ord(char) - 32
        offset = index * glyph_width * glyph_height
        glyph = Image.frombytes("L", (glyph_width, glyph_height),
                                bytes(glyphs["alpha"][offset:offset + glyph_width * glyph_height]))
        mask.paste(glyph, (left, 0))
        left += advances[index]
    if scale != 1:
        mask = mask.resize((round(mask.width * scale), round(mask.height * scale)), Image.Resampling.LANCZOS)
    left = (width - mask.width) // 2
    for y in range(mask.height):
        if top + y >= height:
            break
        for x in range(mask.width):
            if 0 <= left + x < width:
                alpha = mask.getpixel((x, y))
                step = 64 if width == 220 else 16
                alpha = (alpha + step // 2) // step * step if alpha < 256 - step // 2 else 255
                if alpha:
                    at = (top + y) * width + left + x
                    pixels[at] = blend(pixels[at], color, min(alpha, 255))


def draw_symbol(pixels: list[int], width: int, height: int, center_y: int,
                radius: int, stroke: float) -> None:
    center_x = width // 2
    extent = radius + 2
    for y in range(center_y - extent, center_y + extent + 1):
        for x in range(center_x - extent, center_x + extent + 1):
            coverage = 0
            for sub_y in (0.125, 0.375, 0.625, 0.875):
                for sub_x in (0.125, 0.375, 0.625, 0.875):
                    dx, dy = x + sub_x - center_x, y + sub_y - center_y
                    distance = hypot(dx, dy)
                    ring = radius - stroke <= distance <= radius
                    slash = (distance < radius - stroke and
                             abs(dx + dy) / sqrt(2) <= stroke / 2)
                    coverage += ring or slash
            if coverage and 0 <= x < width and 0 <= y < height:
                alpha = (coverage + 2) // 4 * 64
                at = y * width + x
                pixels[at] = blend(pixels[at], ACCENT, min(alpha, 255))


def image(width: int, height: int) -> list[int]:
    if (width, height) not in ((220, 176), (320, 240)):
        raise ValueError("Unsupported Disk Mode display size")
    photo = width == 220
    header_height = 32 if photo else 42
    pixels = [rgb565(*PAPER)] * (width * height)
    for y in range(header_height):
        color = rgb565(*(HEADER if y < header_height - 1 else (203, 215, 223)))
        pixels[y * width:(y + 1) * width] = [color] * width
    draw_text(pixels, width, height, "Disk Mode", font("font-sans13-bold.json"),
              7 if photo else 9, 1 if photo else 1.45, INK)
    if not photo:
        draw_text(pixels, width, height, "LINNET OS", font("font-sans12.json"),
                  50, 1.05, MUTED)
    draw_symbol(pixels, width, height, 91 if photo else 121,
                34 if photo else 47, 3.2 if photo else 4.0)
    draw_text(pixels, width, height, "Do not disconnect", font("font-sans14.json"),
              138 if photo else 180, 1 if photo else 1.25, INK)
    if not photo:
        draw_text(pixels, width, height, "Eject safely when finished", font("font-sans12.json"),
                  210, 1.05, MUTED)
    return pixels


def encode_photo(pixels: list[int]) -> tuple[list[int], list[int]]:
    palette: list[int] = []
    positions: dict[int, int] = {}
    runs: list[int] = []
    for i in range(0, len(pixels), 2):
        first, second = pixels[i:i + 2]
        first = ((first << 8) | (first >> 8)) & 0xffff
        second = ((second << 8) | (second >> 8)) & 0xffff
        pair = first | second << 16
        if pair not in positions:
            positions[pair] = len(palette)
            palette.append(pair)
        index = positions[pair]
        if runs and runs[-1] >> 8 == index and runs[-1] & 255 < 255:
            runs[-1] += 1
        else:
            runs.append(index << 8 | 1)
    if len(palette) > 256 or sum(run & 255 for run in runs) != len(pixels) // 2:
        raise ValueError("Photo Disk Mode image exceeds its LCD stream format")
    return palette, runs


def encode_color(pixels: list[int]) -> tuple[list[int], list[int]]:
    palette: list[int] = []
    positions: dict[int, int] = {}
    runs: list[int] = []
    for color in pixels:
        if color not in positions:
            positions[color] = len(palette)
            palette.append(color)
        index = positions[color]
        if runs and runs[-1] & 255 == index and runs[-1] >> 8 < 255:
            runs[-1] += 256
        else:
            runs.append(256 | index)
    if len(palette) > 256 or sum(run >> 8 for run in runs) != len(pixels):
        raise ValueError("Color Disk Mode image exceeds its LCD stream format")
    return palette, runs


def header(palette: list[int], runs: list[int], *, photo: bool) -> str:
    if photo:
        palette_type, palette_name = "uint32_t", "ln_disk_palette"
        run_name, palette_width = "ln_disk_runs", 8
    else:
        palette_type, palette_name = "uint16_t", "ln_color_disk_palette"
        run_name, palette_width = "ln_color_disk_runs", 4
    lines = ["/* Generated by tools/studio/disk_screen_asset.py from the licensed DejaVu font assets.",
             " * See sdk/assets/FONT-LICENSE.txt. */",
             f"static const {palette_type} {palette_name}[] = {{"]
    lines.extend("    " + ", ".join(f"0x{item:0{palette_width}x}u" for item in palette[i:i + 8]) + ","
                 for i in range(0, len(palette), 8))
    lines.append(f"}};\nstatic const uint16_t {run_name}[] = {{")
    lines.extend("    " + ", ".join(f"0x{item:04x}u" for item in runs[i:i + 12]) + ","
                 for i in range(0, len(runs), 12))
    lines.append("};\n")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=("photo4g-p98", "color320"), default="photo4g-p98")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--preview", type=Path)
    args = parser.parse_args()
    photo = args.target == "photo4g-p98"
    width, height = (220, 176) if photo else (320, 240)
    pixels = image(width, height)
    palette, runs = (encode_photo if photo else encode_color)(pixels)
    args.output.write_text(header(palette, runs, photo=photo), encoding="ascii")
    if args.preview:
        preview = Image.new("RGB", (width, height))
        preview.putdata([components(value) for value in pixels])
        preview.save(args.preview)
    print(f"{len(palette)} palette entries, {len(runs)} runs, "
          f"{len(runs) * 2 + len(palette) * (4 if photo else 2)} bytes")


if __name__ == "__main__":
    main()
