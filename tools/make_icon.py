# Draws the game's icon: a copper trace lighting a red LED on the dark green board, in the colours of
# src/app/draw.cpp. Writes assets/icone/bancada.ico (every size Windows asks for) and bancada-256.png.
#
#   python3 tools/make_icon.py
#
# Every shape is a signed distance, so each size is drawn on its own and stays sharp at 16 pixels.
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'assets', 'icone')
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

BOARD = (12, 28, 24)
BOARD_EDGE = (62, 84, 78)
COPPER = (255, 190, 92)
PIN = (94, 222, 190)
LED = (255, 70, 62)
SHINE = (255, 214, 208)


def rounded_square(x, y, half, radius):
    qx = abs(x - 0.5) - half + radius
    qy = abs(y - 0.5) - half + radius
    outside = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return outside + min(max(qx, qy), 0.0) - radius


def circle(x, y, cx, cy, r):
    return math.hypot(x - cx, y - cy) - r


def segment(x, y, ax, ay, bx, by, r):
    px, py, dx, dy = x - ax, y - ay, bx - ax, by - ay
    t = max(0.0, min(1.0, (px * dx + py * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - dx * t, py - dy * t) - r


def render(size):
    # Small icons get thicker strokes in pixels, so the trace never fades below one pixel.
    stroke = max(0.075, 1.6 / size)
    edge = max(0.022, 1.0 / size)
    layers = [
        (lambda x, y: rounded_square(x, y, 0.47, 0.13), BOARD_EDGE, 1.0),
        (lambda x, y: rounded_square(x, y, 0.47 - edge, 0.13 - edge), BOARD, 1.0),
        (lambda x, y: circle(x, y, 0.62, 0.40, 0.27), LED, 0.2),
        (lambda x, y: segment(x, y, 0.23, 0.28, 0.23, 0.74, stroke / 2), COPPER, 1.0),
        (lambda x, y: segment(x, y, 0.23, 0.74, 0.62, 0.74, stroke / 2), COPPER, 1.0),
        (lambda x, y: segment(x, y, 0.62, 0.74, 0.62, 0.52, stroke / 2), COPPER, 1.0),
        (lambda x, y: circle(x, y, 0.23, 0.28, stroke * 0.95), PIN, 1.0),
        (lambda x, y: circle(x, y, 0.62, 0.40, 0.18), LED, 1.0),
    ]
    if size >= 24:
        layers.append((lambda x, y: circle(x, y, 0.565, 0.345, 0.05), SHINE, 0.9))
    pixels = bytearray()
    for row in range(size):
        pixels.append(0)  # PNG filter: none
        for col in range(size):
            x, y = (col + 0.5) / size, (row + 0.5) / size
            r = g = b = a = 0.0
            for distance, colour, opacity in layers:
                cover = max(0.0, min(1.0, 0.5 - distance(x, y) * size)) * opacity
                if cover <= 0.0:
                    continue
                out = cover + a * (1.0 - cover)
                r = (colour[0] * cover + r * a * (1.0 - cover)) / out
                g = (colour[1] * cover + g * a * (1.0 - cover)) / out
                b = (colour[2] * cover + b * a * (1.0 - cover)) / out
                a = out
            pixels += bytes((round(r), round(g), round(b), round(a * 255)))
    return png(size, bytes(pixels))


def png(size, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    header = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')


def ico(images):
    # Windows reads PNG inside .ico at every size since Vista.
    directory = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    body = b''
    for size, data in images:
        side = 0 if size >= 256 else size
        directory += struct.pack('<BBBBHHII', side, side, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        body += data
    return directory + body


def main():
    os.makedirs(OUT, exist_ok=True)
    images = [(size, render(size)) for size in SIZES]
    with open(os.path.join(OUT, 'bancada.ico'), 'wb') as f:
        f.write(ico(images))
    with open(os.path.join(OUT, 'bancada-256.png'), 'wb') as f:
        f.write(images[-1][1])
    print('icone: %d tamanhos' % len(images))


main()
