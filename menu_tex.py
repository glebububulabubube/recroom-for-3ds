# Конвертер menu PNG -> swizzled RGBA8 .bin для C3D_Tex (PICA200)
# Порядок тайлинга ровно как в tex3ds source/swizzle.cpp (эталон для GPU):
#  - изображение режется на блоки 8x8
#  - внутри каждого блока применяются перестановки таблица+Foursome и пары
# Арт кладётся в верхний левый угол канваса 512x256, остальное прозрачное.
from PIL import Image
import os

GFX = r'C:\Users\Пользователь\Documents\MultiTool\HomeChats\Chat-11\gfx'
OUT = r'C:\Users\Пользователь\Documents\MultiTool\HomeChats\Chat-11\data'

# Таблицы перестановок из tex3ds swizzle.cpp (в индексах 0..63 row-major 8x8)
FOURSOME_TABLE = (
    ( 2, 8,16, 4), ( 3, 9,17, 5), ( 6,10,24,20), ( 7,11,25,21),
    (14,26,28,22), (15,27,29,23), (34,40,48,36), (35,41,49,37),
    (38,42,56,52), (39,43,57,53), (46,58,60,54), (47,59,61,55),
)
PAIR_TABLE = ((12,18),(13,19),(44,50),(45,51))


def tex3ds_swizzle_8x8(pixel_bytes):
    """pixel_bytes: 64 пикселя RGBA8 row-major (y*8+x). Возвращает переставленные."""
    px = [pixel_bytes[i*4:(i+1)*4] for i in range(64)]
    # foursome
    for a, b, c, d in FOURSOME_TABLE:
        px[a], px[b], px[c], px[d] = px[b], px[c], px[d], px[a]
    # pairs
    for a, b in PAIR_TABLE:
        px[a], px[b] = px[b], px[a]
    return b''.join(px)


def swizzle_rgba8(raw, w, h):
    out = bytearray(len(raw))
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            # собрать tile row-major
            tile = bytearray()
            for yy in range(y, y + 8):
                start = (yy * w + x) * 4
                tile += raw[start:start + 8 * 4]
            sw = tex3ds_swizzle_8x8(bytes(tile))
            # уложить в выход в порядке блоков (block-major)
            bx, by = x // 8, y // 8
            dst_start = (by * (w // 8) + bx) * (8 * 8 * 4)
            out[dst_start:dst_start + len(sw)] = sw
    return bytes(out)


def pack(src, dst, W, H):
    im = Image.open(os.path.join(GFX, src)).convert('RGBA')
    im = im.resize((W, H), Image.Resampling.LANCZOS)
    canvas = Image.new('RGBA', (512, 256), (0, 0, 0, 0))
    canvas.paste(im, (0, 0))
    raw = canvas.tobytes()   # R,G,B,A per pixel
    # PICA200 RGBA8 stores A,B,G,R per pixel (tex3ds encode.cpp rgba8888)
    abgr = bytearray(len(raw))
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i], raw[i+1], raw[i+2], raw[i+3]
        abgr[i]   = a
        abgr[i+1] = b
        abgr[i+2] = g
        abgr[i+3] = r
    sw = swizzle_rgba8(bytes(abgr), 512, 256)
    path = os.path.join(OUT, dst)
    with open(path, 'wb') as f:
        f.write(sw)
    print(dst, len(sw), 'swizzled (tex3ds ABGR) ok', 'арт', W, 'x', H)


pack('menu-up.png', 'menu_top.bin', 400, 240)
pack('menu-down.png', 'menu_bottom.bin', 320, 240)