"""Rasterize the simple geometric app mark into standard Windows icon sizes."""
from pathlib import Path
from PIL import Image,ImageDraw
root=Path(__file__).resolve().parents[1]
image=Image.new('RGBA',(256,256),(0,0,0,0));draw=ImageDraw.Draw(image)
draw.rounded_rectangle((8,8,248,248),radius=48,fill='#243e63')
draw.line([(50,177),(50,78),(92,129),(134,78),(134,177)],fill='#f7fbff',width=18,joint='curve')
draw.line([(190,72),(190,153)],fill='#72d4ce',width=16)
draw.polygon([(162,140),(218,140),(190,180)],fill='#72d4ce')
image.save(root/'resources/app.ico',sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
