#!/usr/bin/env python3
# builds the LiveArea and boot images from your own copy of the apk (copy template.xml from livearea/ next to them)
# usage: make_livearea.py <game.apk> <out dir>
import io, sys, zipfile
from PIL import Image

apk = zipfile.ZipFile(sys.argv[1])
out = sys.argv[2]

def load(name):
    return Image.open(io.BytesIO(apk.read(name))).convert('RGB')

def fit(img, w, h):
    # crop to the target aspect, then scale
    sw, sh = img.size
    if sw * h > sh * w:
        nw = sh * w // h
        img = img.crop(((sw - nw) // 2, 0, (sw - nw) // 2 + nw, sh))
    else:
        nh = sw * h // w
        img = img.crop((0, (sh - nh) // 2, sw, (sh - nh) // 2 + nh))
    return img.resize((w, h), Image.LANCZOS)

def save(img, name):
    # livearea wants 8-bit indexed pngs
    img.quantize(256, dither=Image.Dither.FLOYDSTEINBERG).save(f'{out}/{name}', optimize=True)

icon = load('res/drawable-sw800dp-hdpi/icon.png')
splash = load('res/drawable/splash.jpg')
save(icon.resize((128, 128), Image.LANCZOS), 'icon0.png')
save(fit(splash, 840, 500), 'bg0.png')
save(fit(splash, 960, 544), 'pic0.png')

# startup.png is the launch button on the right of the livearea (see template.xml): the icon on a
# transparent 400x179 canvas. swap in your own logo png if you have one
button = Image.new('RGBA', (400, 179), (0, 0, 0, 0))
button.paste(icon.resize((179, 179), Image.LANCZOS), (110, 0))
button.save(f'{out}/startup.png', optimize=True)
