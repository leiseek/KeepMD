"""Rasterize the geometric KeepMD and KeepPrompt marks into standard Windows icon sizes.

Both marks share one family: a rounded square, white strokes and the teal accent. KeepMD keeps its navy
"M + arrow" Markdown mark; KeepPrompt uses violet with a prompt chevron and cursor.
"""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]


def keepmd():
    image = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((8, 8, 248, 248), radius=48, fill='#243e63')
    draw.line([(50, 177), (50, 78), (92, 129), (134, 78), (134, 177)], fill='#f7fbff', width=18, joint='curve')
    draw.line([(190, 72), (190, 153)], fill='#72d4ce', width=16)
    draw.polygon([(162, 140), (218, 140), (190, 180)], fill='#72d4ce')
    return image


def keepprompt():
    image = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((8, 8, 248, 248), radius=48, fill='#4b3aa8')
    draw.line([(62, 80), (118, 128), (62, 176)], fill='#f7fbff', width=20, joint='curve')
    for x, y in ((62, 80), (118, 128), (62, 176)):
        draw.ellipse((x - 10, y - 10, x + 10, y + 10), fill='#f7fbff')
    draw.rounded_rectangle((136, 162, 198, 182), radius=10, fill='#72d4ce')
    return image


if __name__ == '__main__':
    keepmd().save(root / 'resources/keepmd/app.ico', sizes=sizes)
    keepprompt().save(root / 'resources/keepprompt/app.ico', sizes=sizes)
