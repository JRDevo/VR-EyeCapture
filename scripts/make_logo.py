#!/usr/bin/env python3
# make_logo.py -- developer script: regenerates the project art in docs\images from code, so every size stays
# consistent and nothing depends on a drawing program. Not in the zip. Needs Python 3 + Pillow (pip install pillow)
# and the Windows fonts (Segoe UI Bold). Run:  python scripts\make_logo.py
#
# The mark: VR goggles seen from the front. Left lens blue, right lens coral (the same two colours the social preview
# uses for L / R), a red recording dot top-right. The pupils look slightly inwards: two eyes, one picture.
#
# Files written (docs\images):
#   logo_mark_512.png / logo_mark_128.png   the mark alone, transparent background
#   logo_icon_800.png                       the mark on a dark rounded square (YouTube channel avatar, GitHub org avatar)
#   logo_wordmark.png                       mark + "OpenXR-EyeCapture", white text, transparent (for dark backgrounds)
#   logo_banner.png                         1280 x 320 README header, dark background
#   thumbnail_overlay.png                   1280 x 720 RGBA layer that scripts\make_thumbnail.ps1 puts over a frame of
#                                           the recording: "3D" badge top-left, logo top-right, dark band at the bottom
#                                           for the title
#   thumbnail_template.png                  the overlay over a placeholder picture with an example title, for the README
# logo_mark.svg and logo.svg next to them are the same drawing as vectors (hand-written, same coordinates).

import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'docs', 'images')
FONT_BOLD = r'C:\Windows\Fonts\segoeuib.ttf'
FONT_REG = r'C:\Windows\Fonts\segoeui.ttf'

BG = (14, 16, 23, 255)         # #0e1017 -- same dark as social_preview.png
BLUE = (59, 139, 255, 255)     # left eye
CORAL = (255, 107, 107, 255)   # right eye
RED = (255, 59, 59, 255)       # recording dot
WHITE = (240, 242, 246, 255)
GREY = (154, 163, 178, 255)
CLEAR = (0, 0, 0, 0)

SS = 4  # supersampling factor for the vector-ish drawing


def font(path, size):
    return ImageFont.truetype(path, size)


def draw_mark(draw, ox, oy, size):
    """Draw the mark into `draw` with its 256-unit design box scaled to `size` px, top-left at (ox, oy)."""
    s = size / 256.0

    def R(x0, y0, x1, y1):
        return (ox + x0 * s, oy + y0 * s, ox + x1 * s, oy + y1 * s)

    w = max(1, round(11 * s))
    # head strap stubs left and right of the goggles
    strap = max(1, round(9 * s))
    draw.line([(ox + 4 * s, oy + 128 * s), (ox + 24 * s, oy + 128 * s)], fill=GREY, width=strap)
    draw.line([(ox + 232 * s, oy + 128 * s), (ox + 252 * s, oy + 128 * s)], fill=GREY, width=strap)
    draw.ellipse(R(4 - 4.5, 128 - 4.5, 4 + 4.5, 128 + 4.5), fill=GREY)
    draw.ellipse(R(252 - 4.5, 128 - 4.5, 252 + 4.5, 128 + 4.5), fill=GREY)
    # bridge between the lenses
    draw.rounded_rectangle(R(112, 117, 144, 139), radius=6 * s, fill=GREY)
    # lenses: left blue, right coral
    draw.rounded_rectangle(R(24, 76, 118, 180), radius=26 * s, outline=BLUE, width=w)
    draw.rounded_rectangle(R(138, 76, 232, 180), radius=26 * s, outline=CORAL, width=w)
    # pupils, looking slightly inwards (two eyes, one picture)
    draw.ellipse(R(78 - 17, 128 - 17, 78 + 17, 128 + 17), fill=BLUE)
    draw.ellipse(R(178 - 17, 128 - 17, 178 + 17, 128 + 17), fill=CORAL)
    # recording dot
    draw.ellipse(R(222 - 18, 50 - 18, 222 + 18, 50 + 18), fill=WHITE)
    draw.ellipse(R(222 - 13, 50 - 13, 222 + 13, 50 + 13), fill=RED)


def mark_image(size, bg=None, radius_frac=0.0, pad_frac=0.0):
    """The mark as an RGBA image of size x size. bg: fill colour (rounded square) or None for transparent."""
    big = size * SS
    img = Image.new('RGBA', (big, big), CLEAR)
    d = ImageDraw.Draw(img)
    if bg is not None:
        d.rounded_rectangle((0, 0, big - 1, big - 1), radius=big * radius_frac, fill=bg)
    pad = big * pad_frac
    draw_mark(d, pad, pad, big - 2 * pad)
    return img.resize((size, size), Image.LANCZOS)


def wordmark_image(mark_px, text_px, text_color=WHITE, bg=None, pad=0):
    """mark + 'OpenXR-EyeCapture' side by side; returns an RGBA image."""
    f = font(FONT_BOLD, text_px)
    text = 'OpenXR-EyeCapture'
    bbox = f.getbbox(text)                       # (l, t, r, b) at baseline-relative coords
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    gap = round(mark_px * 0.18)
    W = pad * 2 + mark_px + gap + tw
    H = pad * 2 + mark_px
    img = Image.new('RGBA', (W, H), bg if bg is not None else CLEAR)
    img.alpha_composite(mark_image(mark_px), (pad, pad))
    d = ImageDraw.Draw(img)
    # centre the text's cap height on the mark's lens row (the lenses sit at 76..180 of 256 -> centre at 128)
    ty = pad + mark_px / 2 - (bbox[1] + bbox[3]) / 2
    d.text((pad + mark_px + gap - bbox[0], ty), text, font=f, fill=text_color)
    return img


def text_with_shadow(img, xy, text, f, fill, shadow=(0, 0, 0, 170), offset=4, blur=6):
    layer = Image.new('RGBA', img.size, CLEAR)
    ImageDraw.Draw(layer).text((xy[0] + offset, xy[1] + offset), text, font=f, fill=shadow)
    layer = layer.filter(ImageFilter.GaussianBlur(blur))
    img.alpha_composite(layer)
    ImageDraw.Draw(img).text(xy, text, font=f, fill=fill)


def badge_3d():
    """The '3D' badge: white '3D' with a blue copy offset left and a coral copy offset right (the two eyes), on a dark pill."""
    f = font(FONT_BOLD, 86)
    text = '3D'
    bbox = f.getbbox(text)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    padx, pady = 30, 18
    W, H = tw + padx * 2 + 12, th + pady * 2
    img = Image.new('RGBA', (W, H), CLEAR)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, W - 1, H - 1), radius=H // 2, fill=(14, 16, 23, 225), outline=(255, 255, 255, 60), width=2)
    x = padx + 6 - bbox[0]
    y = pady - bbox[1]
    d.text((x - 7, y), text, font=f, fill=BLUE)
    d.text((x + 7, y), text, font=f, fill=CORAL)
    d.text((x, y), text, font=f, fill=WHITE)
    return img


def thumbnail_overlay():
    W, H = 1280, 720
    img = Image.new('RGBA', (W, H), CLEAR)
    # dark band at the bottom for the title: transparent at y=360, nearly solid at the bottom edge
    band = Image.new('RGBA', (W, H), CLEAR)
    bd = ImageDraw.Draw(band)
    y0 = 360
    for y in range(y0, H):
        t = (y - y0) / (H - y0)
        a = int(235 * (t ** 1.6))
        bd.line([(0, y), (W, y)], fill=(BG[0], BG[1], BG[2], a))
    img.alpha_composite(band)
    # soft dark corner behind the logo so it reads over bright game frames
    corner = Image.new('RGBA', (W, H), CLEAR)
    cd = ImageDraw.Draw(corner)
    cd.ellipse((W - 760, -300, W + 260, 300), fill=(BG[0], BG[1], BG[2], 150))
    corner = corner.filter(ImageFilter.GaussianBlur(70))
    img.alpha_composite(corner)
    # logo top-right
    wm = wordmark_image(88, 44)
    img.alpha_composite(wm, (W - 40 - wm.width, 36))
    # 3D badge top-left
    b = badge_3d()
    img.alpha_composite(b, (40, 36))
    return img


def placeholder_frame():
    """A stand-in for a game frame: dark blue-to-coral gradient with a perspective grid, so the template shows the layout."""
    W, H = 1280, 720
    img = Image.new('RGBA', (W, H), BG)
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / H
        c = (int(18 + 30 * t), int(22 + 20 * t), int(40 + 60 * (1 - t)), 255)
        d.line([(0, y), (W, y)], fill=c)
    horizon = 330
    # floor grid converging to the centre
    for i in range(-14, 15):
        d.line([(W / 2 + i * 220, H), (W / 2 + i * 10, horizon)], fill=(59, 139, 255, 70), width=2)
    for k in range(1, 12):
        y = horizon + (H - horizon) * (k / 12) ** 2.2
        d.line([(0, y), (W, y)], fill=(59, 139, 255, 70), width=2)
    # a warm glow near the horizon
    glow = Image.new('RGBA', (W, H), CLEAR)
    ImageDraw.Draw(glow).ellipse((W / 2 - 420, horizon - 120, W / 2 + 420, horizon + 120), fill=(255, 107, 107, 120))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(80)))
    return img


def thumbnail_template(overlay):
    img = placeholder_frame()
    img.alpha_composite(overlay)
    text_with_shadow(img, (48, 548), 'Your game title', font(FONT_BOLD, 92), WHITE)
    text_with_shadow(img, (50, 660 - 4), 'Side-by-side 3D  |  recorded with OpenXR-EyeCapture', font(FONT_REG, 36), GREY)
    return img


def banner():
    W, H = 1280, 320
    img = Image.new('RGBA', (W, H), BG)
    wm = wordmark_image(200, 104)
    img.alpha_composite(wm, ((W - wm.width) // 2, 40))
    d = ImageDraw.Draw(img)
    f = font(FONT_REG, 36)
    sub = 'Stereo 3D recorder for any OpenXR game'
    bb = f.getbbox(sub)
    d.text(((W - (bb[2] - bb[0])) / 2 - bb[0], 256), sub, font=f, fill=GREY)
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    mark_image(512).save(os.path.join(OUT, 'logo_mark_512.png'))
    mark_image(128).save(os.path.join(OUT, 'logo_mark_128.png'))
    mark_image(800, bg=BG, radius_frac=0.2, pad_frac=0.1).save(os.path.join(OUT, 'logo_icon_800.png'))
    wordmark_image(256, 132, pad=16).save(os.path.join(OUT, 'logo_wordmark.png'))
    banner().convert('RGB').save(os.path.join(OUT, 'logo_banner.png'), optimize=True)
    ov = thumbnail_overlay()
    ov.save(os.path.join(OUT, 'thumbnail_overlay.png'))
    thumbnail_template(ov).convert('RGB').save(os.path.join(OUT, 'thumbnail_template.png'), optimize=True)
    print('written to', OUT)


if __name__ == '__main__':
    main()
