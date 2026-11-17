"""Estimate foliage coverage from stock BC3 alpha, without running a GPU.

Run from the repository root. This diagnostic uses neighbouring texel pairs
as derivative quads; it is not a screenshot or a GPU rendering test.
"""
import argparse
from pathlib import Path
import struct


def bc3_alpha(data, width, height):
    blocks_w = max(1, (width + 3) // 4)
    blocks_h = max(1, (height + 3) // 4)
    pixels = [0.0] * (width * height)
    for by in range(blocks_h):
        for bx in range(blocks_w):
            offset = (by * blocks_w + bx) * 16
            a0, a1 = data[offset:offset + 2]
            table = [a0, a1]
            if a0 > a1:
                table += [((7 - j) * a0 + j * a1) / 7 for j in range(1, 7)]
            else:
                table += [((5 - j) * a0 + j * a1) / 5 for j in range(1, 5)] + [0, 255]
            bits = int.from_bytes(data[offset + 2:offset + 8], "little")
            for j in range(16):
                x, y = bx * 4 + j % 4, by * 4 + j // 4
                if x < width and y < height:
                    pixels[y * width + x] = table[(bits >> (j * 3)) & 7] / 255
    return pixels


def clamp(value):
    return max(0.0, min(1.0, value))


def analyze(path, reference):
    data = path.read_bytes()
    if data[:4] != b"DDS " or data[84:88] != b"DXT5":
        raise ValueError(f"{path}: expected legacy BC3 DDS")
    height, width = struct.unpack_from("<II", data, 12)
    levels = struct.unpack_from("<I", data, 28)[0] or 1
    offset = 4 + struct.unpack_from("<I", data, 4)[0]
    cutoff = reference / 255
    for mip in range(levels):
        size = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 16
        if offset + size > len(data):
            raise ValueError(f"{path}: truncated mip {mip}")
        alpha = bc3_alpha(data[offset:offset + size], width, height)
        old, centered = 0.0, 0.0
        for y in range(height):
            for x in range(width):
                a = alpha[y * width + x]
                dx = abs(a - alpha[y * width + min(x ^ 1, width - 1)])
                dy = abs(a - alpha[min(y ^ 1, height - 1) * width + x])
                t = clamp((a - cutoff) / max(dx + dy, 0.06))
                old += t * t * (3 - 2 * t) if a > cutoff else 0
                centered += clamp((a - cutoff) / max(dx + dy, 1 / 255) + 0.5) if a > 0 else 0
        count = len(alpha)
        hard = sum(a > cutoff for a in alpha) / count
        print(f"{path.name},{mip},{width},{height},{hard:.6f},{old/count:.6f},{centered/count:.6f}")
        offset += size
        width, height = max(1, width // 2), max(1, height // 2)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path,
                        default=[Path("game/maps/textures/Lomb3.dds"), Path("game/maps/textures/Lomb4.dds")])
    parser.add_argument("--reference", type=int, default=0x38)
    args = parser.parse_args()
    if not 0 <= args.reference <= 255:
        parser.error("reference must be an 8-bit value")
    print("texture,mip,width,height,hard_cutout,old_estimate,centered_estimate")
    for path in args.paths:
        analyze(path, args.reference)
