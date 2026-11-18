"""Draw the application icon (an original arched-door glyph, no Encarta art) and write it as .ico and .png.

  python3 tools/art/make_icon.py src/platform/win32/mindmaze.ico docs/icon.png

Standard library only. The shapes are evaluated per sub-pixel (4x4 samples), so every size is drawn directly.
"""
import struct
import sys
import zlib

NAVY, STONE, STONE_DARK, WOOD, SEAM, GOLD = (31, 42, 68), (222, 205, 166), (176, 158, 120), (126, 78, 40), (70, 42, 20), (242, 196, 64)


def colour_at(x, y):
    """Colour (r, g, b) or None (transparent) at a point of the unit square."""
    # rounded square background
    r = 0.18
    cx, cy = min(max(x, r), 1 - r), min(max(y, r), 1 - r)
    if (x - cx) ** 2 + (y - cy) ** 2 > r * r:
        return None

    def arch(half, top, bottom):
        return (abs(x - 0.5) <= half and top <= y <= bottom) or ((x - 0.5) ** 2 + (y - top) ** 2 <= half * half and y <= top)

    if arch(0.20, 0.45, 0.86):                               # the door
        if (x - 0.44) ** 2 + (y - 0.66) ** 2 < 0.026 ** 2 or (x - 0.56) ** 2 + (y - 0.66) ** 2 < 0.026 ** 2:
            return GOLD
        if abs(x - 0.5) < 0.012 or abs(y - 0.45) < 0.008:
            return SEAM
        return WOOD
    if arch(0.29, 0.45, 0.86):                               # the stone arch around it
        # joints between the stones: along the legs every 0.1, around the arch every 30 degrees
        if y > 0.45:
            return STONE_DARK if (y - 0.45) % 0.1 < 0.012 else STONE
        import math
        a = math.degrees(math.atan2(0.45 - y, x - 0.5))
        return STONE_DARK if a % 30 < 3.2 else STONE
    if 0.86 < y <= 0.90 and abs(x - 0.5) <= 0.36:            # threshold
        return STONE_DARK
    return NAVY


def render(size, ss=4):
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0
            for sy in range(ss):
                for sx in range(ss):
                    c = colour_at((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    if c:
                        r += c[0]
                        g += c[1]
                        b += c[2]
                        a += 1
            n = ss * ss
            row += bytes((r // a, g // a, b // a, 255 * a // n)) if a else bytes(4)
        rows.append(bytes(row))
    return rows


def png(rows):
    size = len(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = b"".join(b"\0" + r for r in rows)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def ico_bmp(rows):
    """32-bit icon bitmap: header with doubled height, BGRA rows bottom-up, then an empty AND mask."""
    size = len(rows)
    body = b"".join(bytes(b for i in range(0, len(r), 4) for b in (r[i + 2], r[i + 1], r[i], r[i + 3])) for r in reversed(rows))
    mask = bytes(((size + 31) // 32 * 4) * size)
    return struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(body) + len(mask), 0, 0, 0, 0) + body + mask


def main():
    ico_path, png_path = sys.argv[1], sys.argv[2]
    images = []
    for size in (16, 24, 32, 48, 64, 256):
        rows = render(size)
        images.append((size, png(rows) if size == 256 else ico_bmp(rows)))
        if size == 256:
            with open(png_path, "wb") as f:
                f.write(png(rows))
    offset = 6 + 16 * len(images)
    head = struct.pack("<HHH", 0, 1, len(images))
    for size, data in images:
        head += struct.pack("<BBBBHHII", size & 255, size & 255, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(ico_path, "wb") as f:
        f.write(head + b"".join(d for _, d in images))
    print("icon: %d sizes -> %s, %s" % (len(images), ico_path, png_path))


if __name__ == "__main__":
    main()
