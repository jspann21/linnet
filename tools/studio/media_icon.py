"""Generate the shared Windows icon from Linnet's existing boot artwork."""
from pathlib import Path

from PIL import Image, ImageDraw

from tools.studio.targets import ROOT


def generate(output=None):
    output = Path(output) if output is not None else ROOT / 'assets/linnet.ico'
    with Image.open(ROOT / 'assets/boot-screen.png') as source:
        mark = source.convert('RGB').crop((594, 180, 904, 625))
    mark.thumbnail((208, 208), Image.Resampling.LANCZOS)
    pixels = []
    for red, green, blue in mark.get_flattened_data():
        peak = max(red, green, blue)
        alpha = max(0, (peak - 24) * 255 // 231)
        pixels.append((red * 255 // peak, green * 255 // peak, blue * 255 // peak, alpha)
                      if alpha else (0, 0, 0, 0))
    foreground = Image.new('RGBA', mark.size)
    foreground.putdata(pixels)
    # A dark badge keeps the silver artwork visible on light and dark shells.
    badge = Image.new('RGBA', (256, 256))
    ImageDraw.Draw(badge).rounded_rectangle((0, 0, 255, 255), radius=40, fill='#102b32')
    badge.alpha_composite(foreground, ((256 - mark.width) // 2, (256 - mark.height) // 2))
    badge.save(output.with_name('linnet-icon.png'))
    wizard = Image.new('RGBA', (328, 628), '#102b32')
    wizard.alpha_composite(badge, (36, 186))
    wizard.convert('RGB').save(output.with_name('linnet-setup.png'))
    badge.save(output, format='ICO', sizes=[(size, size) for size in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
    return output


if __name__ == '__main__':
    print(generate())
