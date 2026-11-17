"""Inspect stock SCX4 rigid vehicle meshes with a CPU top-view depth test.

This is geometry evidence, not a bgfx screenshot or an in-game regression test.
Only position/normal/UV rigid vertex layouts are accepted intentionally.
"""
import argparse
from pathlib import Path
import struct


def triangles(path):
    data = path.read_bytes()
    u32 = lambda offset: struct.unpack_from("<I", data, offset)[0]
    if u32(4) != 4:
        raise ValueError("expected SCX4")
    points, flags = [], 0
    for i in range(u32(8)):
        kind, offset = struct.unpack_from("<II", data, 12 + i * 8)
        if kind == 0:
            flags = u32(offset + 12)
        elif kind == 4:
            count, layout = u32(offset + 8), u32(offset + 12)
            if layout not in (0x41, 0x241):
                raise ValueError(f"unsupported diagnostic layout {layout:#x}")
            stride = 32 if layout == 0x241 else 24
            points = [struct.unpack_from("<fff", data, offset + 16 + j * stride) for j in range(count)]
        elif kind == 5:
            indices = struct.unpack_from("<" + "H" * u32(offset + 8), data, offset + 12)
            for j in range(0, len(indices) - 2, 3):
                yield [points[k] for k in indices[j:j + 3]], flags


def analyze(path):
    faces = list(triangles(path))
    points = [p for face, _ in faces for p in face]
    lo = [min(p[k] for p in points) for k in range(3)]
    hi = [max(p[k] for p in points) for k in range(3)]
    size = 256
    depth = [-float("inf")] * (size * size)
    back = [False] * (size * size)
    for face, flags in faces:
        q = [((p[0] - lo[0]) / (hi[0] - lo[0]) * (size - 1),
              (p[2] - lo[2]) / (hi[2] - lo[2]) * (size - 1)) for p in face]
        (ax, ay), (bx, by), (cx, cy) = q
        denominator = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(denominator) < 1e-8:
            continue
        # Positive projected area means a downward-facing triangle in X/Z.
        unwanted = denominator > 0 and not (flags & 0x02000000)
        for y in range(max(0, int(min(p[1] for p in q))), min(size, int(max(p[1] for p in q)) + 1)):
            for x in range(max(0, int(min(p[0] for p in q))), min(size, int(max(p[0] for p in q)) + 1)):
                a = ((by - cy) * (x + .5 - cx) + (cx - bx) * (y + .5 - cy)) / denominator
                b = ((cy - ay) * (x + .5 - cx) + (ax - cx) * (y + .5 - cy)) / denominator
                c = 1 - a - b
                if min(a, b, c) < 0:
                    continue
                height = a * face[0][1] + b * face[1][1] + c * face[2][1]
                pixel = y * size + x
                if height > depth[pixel]:
                    depth[pixel], back[pixel] = height, unwanted
    covered = sum(d > -float("inf") for d in depth)
    print(f"{path}: triangles={len(faces)} top-view pixels={covered} frontmost unwanted backfaces={sum(back)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path, default=[Path("game/cars/racers/Enula_data/meshes/trunk.scx"), Path("game/cars/racers/MC_data/meshes/chassis.scx")])
    for path in parser.parse_args().paths:
        analyze(path)
