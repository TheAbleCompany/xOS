#!/usr/bin/env python3
"""
hov_assets.py — asset pipeline for the ArtyomX64 port of HoverTank3DClone.

Takes the ORIGINAL SFML assets and produces compact PNGs that the OS-side
userspace image loader (apps/imgload) can decode within ArtyomX64's RAM budget:

  soldier.png  (1536x1024 RGB, black color-key)
      -> 384x256 RGBA (scale 1/4, frames 48x128), black key baked into alpha,
         interior pinholes re-solidified
  enemy.png    (1911x1000 RGBA, real alpha, 2 walk frames in one row)
      -> 192x100 RGBA (2 frames 96x100, full-frame scale like SFML does,
         NO black key: sanitars have black outlines that a key would eat)
  chan.png     (2490x1992 RGBA, 5x4 grid of 498x498 frames, 3 rows used)
      -> 320x192 RGBA (3 rows only, frames 64x64); if the source has no real
         alpha, the black color-key is baked just like the original game does
         for soldier.png
  game_over.jpeg (1000x1000 progressive JPEG)
      -> 512x320 RGB PNG (same non-uniform stretch the original used for
         800x600); JPEG itself is NOT decoded in the OS

Usage:  hov_assets.py <src_dir> <out_dir>
"""
import sys
import os
import numpy as np
from PIL import Image

def black_key_to_alpha(img: Image.Image) -> Image.Image:
    """Pure black (0,0,0) -> alpha 0, exactly like SFML createMaskFromColor(Black, 0)."""
    rgba = img.convert("RGBA")
    a = np.array(rgba)
    black = (a[:, :, 0] == 0) & (a[:, :, 1] == 0) & (a[:, :, 2] == 0)
    a[:, :, 3] = np.where(black, 0, 255).astype(np.uint8)
    return Image.fromarray(a, "RGBA")

def fill_pinholes(img: Image.Image, alpha_thresh=16, neighbor_min=200):
    """Re-solidify transparent pixels whose 4 neighbours are all opaque.
    Removes interior 'holes' created by the black color-key inside the sprite
    body (same pixels SFML leaves transparent; filling them is strictly better)."""
    a = np.array(img)
    al = a[:, :, 3]
    transparent = al < alpha_thresh
    if not transparent.any():
        return img
    op = al >= neighbor_min
    # shift neighbours
    up    = np.roll(op,  1, axis=0); up[0, :] = False
    down  = np.roll(op, -1, axis=0); down[-1:, :] = False
    left  = np.roll(op,  1, axis=1); left[:, 0] = False
    right = np.roll(op, -1, axis=1); right[:, -1:] = False
    holes = transparent & up & down & left & right
    a[:, :, 3][holes] = 255
    return Image.fromarray(a, "RGBA")

def has_real_alpha(img: Image.Image) -> bool:
    a = np.array(img.convert("RGBA"))
    al = a[:, :, 3]
    return bool((al < 250).any())

FRONT_WIN = (60, 330)    # front sprite mass y 69..323 (title ends y33, numbers start y339)
BACK_WIN  = (415, 700)   # back sprite mass y 424..695 (title 379..392, numbers 714..728, art 766+)
CELL_W, CELL_H = 64, 96  # output frame cell; sheet = 8*64 x 2*96 = 512x192

def extract_soldier_frames(im_rgb):
    """Crop per-frame windows from the design sheet, black-key -> RGBA,
    scale to fit the cell, paste bottom-center (feet on floor line)."""
    W, H = im_rgb.size
    fw = W // 8  # 192
    out = Image.new("RGBA", (CELL_W * 8, CELL_H * 2), (0, 0, 0, 0))
    for half, (y0, y1) in enumerate([FRONT_WIN, BACK_WIN]):
        for i in range(8):
            win = im_rgb.crop((i * fw, y0, (i + 1) * fw, y1))
            keyed = black_key_to_alpha(win)
            # scale keeping aspect: fit inside CELL_W x CELL_H
            w, h = keyed.size
            s = min(CELL_W / w, CELL_H / h)
            nw, nh = max(1, int(w * s)), max(1, int(h * s))
            small = keyed.resize((nw, nh), Image.BOX)
            cell = Image.new("RGBA", (CELL_W, CELL_H), (0, 0, 0, 0))
            cell.alpha_composite(small, ((CELL_W - nw) // 2, CELL_H - nh))
            out.alpha_composite(cell, (i * CELL_W, half * CELL_H))
    return out

def process_soldier(src, out):
    im = Image.open(src).convert("RGB")
    print(f"soldier: source {im.size[0]}x{im.size[1]}")
    sheet = extract_soldier_frames(im)
    sheet = fill_pinholes(sheet)
    out_path = os.path.join(out, "soldier.png")
    sheet.save(out_path, "PNG", optimize=True)
    print(f"soldier: -> {sheet.size[0]}x{sheet.size[1]} RGBA (8x2 frames {CELL_W}x{CELL_H}, top=front/bottom=back) -> {out_path} ({os.path.getsize(out_path)} bytes)")

# Sanitar (enemy.png): 2 frames in one row, full-frame scale -- SFML maps the
# whole frame (padding included) onto the sprite quad, so we must NOT trim the
# content or the sanitars would look bigger on the OS than in the original.
SAN_CELL_W, SAN_CELL_H = 96, 100

def process_enemy(src, out):
    """enemy.png -> sanitar.png: 2 frames 96x100, real alpha kept as is."""
    im = Image.open(src).convert("RGBA")
    print(f"enemy: source {im.size[0]}x{im.size[1]} real_alpha={has_real_alpha(im)}")
    if not has_real_alpha(im):
        im = black_key_to_alpha(im)
    fw = im.size[0] // 2
    sheet = Image.new("RGBA", (SAN_CELL_W * 2, SAN_CELL_H), (0, 0, 0, 0))
    for i in range(2):
        x0 = i * fw
        x1 = (i + 1) * fw if i == 0 else im.size[0]
        fr = im.crop((x0, 0, x1, im.size[1]))
        small = fr.resize((SAN_CELL_W, SAN_CELL_H), Image.BOX)
        sheet.alpha_composite(small, (i * SAN_CELL_W, 0))
    out_path = os.path.join(out, "sanitar.png")
    sheet.save(out_path, "PNG", optimize=True)
    print(f"enemy: -> {sheet.size[0]}x{sheet.size[1]} RGBA (2 frames {SAN_CELL_W}x{SAN_CELL_H}) -> {out_path} ({os.path.getsize(out_path)} bytes)")

def process_chan(src, out):
    im = Image.open(src).convert("RGBA")
    print(f"chan: source {im.size[0]}x{im.size[1]} real_alpha={has_real_alpha(im)}")
    if not has_real_alpha(im):
        im = black_key_to_alpha(im)
    # keep only the 3 top frame rows: 2490 x 1494 (3 * 498)
    im3 = im.crop((0, 0, im.size[0], 1494))
    small = im3.resize((320, 192), Image.BOX)  # frames 64x64
    out_path = os.path.join(out, "chan.png")
    small.save(out_path, "PNG", optimize=True)
    print(f"chan: -> {small.size[0]}x{small.size[1]} RGBA (3 rows, frames 64x64) -> {out_path} ({os.path.getsize(out_path)} bytes)")

def process_gameover(src, out):
    im = Image.open(src).convert("RGB")
    print(f"game_over: source {im.size[0]}x{im.size[1]} mode={im.mode}")
    small = im.resize((512, 320), Image.BOX)
    out_path = os.path.join(out, "gameover.png")
    small.save(out_path, "PNG", optimize=True)
    print(f"game_over: -> {small.size[0]}x{small.size[1]} RGB -> {out_path} ({os.path.getsize(out_path)} bytes)")

def make_bmp_samples(out):
    """Tiny 24bpp BMP for the imgload unit test (not packed into initrd)."""
    img = Image.new("RGB", (16, 16))
    px = img.load()
    for y in range(16):
        for x in range(16):
            px[x, y] = (x * 16, y * 16, (x * y) & 0xFF)
    img.save(os.path.join(out, "test24.bmp"), "BMP")

if __name__ == "__main__":
    src_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    process_soldier(os.path.join(src_dir, "soldier.png"), out_dir)
    process_enemy(os.path.join(src_dir, "enemy.png"), out_dir)
    process_chan(os.path.join(src_dir, "chan.png"), out_dir)
    process_gameover(os.path.join(src_dir, "game_over.jpeg"), out_dir)
    make_bmp_samples(out_dir)
    print("done")
