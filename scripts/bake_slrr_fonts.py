#!/usr/bin/env python3
"""Bake the nine stock SLRR SCX + PNG/TGA charsets into outline TrueType fonts.

Tool-only dependencies: python -m pip install Pillow fontTools
Usage: python scripts/bake_slrr_fonts.py --root /path/to/private/game-copy
       [--out /path/to/output/fonts]

Inputs are read-only; existing outputs are never overwritten. PNG takes precedence
over TGA, as in the stock loader. Do not use a pristine installation as --root.
The result uses direct byte codepoints 0..255 (not a Windows codepage), 1024 units
per em, and a top-origin baseline: TTF y = -SCX pixel y, hhea ascent = 0.
Runtime consumers must use stbtt_ScaleForMappingEmToPixels, not pixel-height scale.

The 0.5 interpolated-coverage silhouette is traced at quarter-pixel resolution
as connected boundary contours with holes, not one rectangle per pixel. Outlines
are polygonal, not fitted design curves. No game assets are bundled here.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from io import BytesIO
import math
from pathlib import Path
import struct
import sys
import warnings


UNITS_PER_EM = 1024
# Keep in sync with font_px_height_from_name in src/render/slrr_font.cpp.
FONT_HEIGHTS = {
    "simple20": 20, "simple40": 40, "console10": 10,
    "slii24": 24, "sl28": 28, "sl14": 14,
    "console5": 7, "slii11": 10, "slii17": 15,
}
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_IMAGE_DIMENSION = 4096
MAX_IMAGE_PIXELS = 16 * 1024 * 1024
MAX_GLYPH_PIXELS = 1024 * 1024
MAX_FONT_SAMPLED_PIXELS = 32 * 1024 * 1024
MAX_BOUNDARY_EDGES = 131072
MAX_GLYPH_POINTS = 32767
FIXED_TIMESTAMP = 2082844800  # 1970-01-01, in TrueType's 1904 epoch.


class BakeError(ValueError):
    """Invalid assets, unsupported data, or unsafe output destination."""


def dependencies():
    try:
        from PIL import Image
        from fontTools.fontBuilder import FontBuilder
        from fontTools.pens.ttGlyphPen import TTGlyphPen
    except ImportError as exc:
        raise BakeError(
            "Missing tool dependency; install with: python -m pip install Pillow fontTools"
        ) from exc
    return Image, FontBuilder, TTGlyphPen


@dataclass(frozen=True)
class Vertex:
    x: float
    y: float
    u: float
    v: float


@dataclass(frozen=True)
class Glyph:
    vertices: tuple[Vertex, ...]

    @property
    def advance(self):
        # This is deliberately NOT x1-x0: SLII quads have negative bearings.
        return self.vertices[1].x


def parse_scx(data: bytes) -> list[Glyph]:
    """Read only the bounded INVO v3 vertex stream needed by a charset."""
    if len(data) < 12 or len(data) > MAX_FILE_BYTES or data[:4] != b"INVO":
        raise BakeError("SCX: missing INVO header or invalid file size")
    version, header = struct.unpack_from("<II", data, 4)
    if version != 3 or not 0x2C <= header <= 0x1000:
        raise BakeError("SCX: expected INVO version 3 with a bounded header")
    if 8 + header + 4 > len(data):
        raise BakeError("SCX: truncated header/vertex count")
    stride = 44
    if header > 0x3C:
        if header < 0x40:
            raise BakeError("SCX: truncated extended stride field")
        stride = struct.unpack_from("<I", data, 8 + 0x3C)[0]
    count = struct.unpack_from("<I", data, 8 + header)[0]
    if not 32 <= stride <= 256 or not 4 <= count <= 256 * 4 or count % 4:
        raise BakeError("SCX: invalid vertex stride/count for a byte charset")
    start = 8 + header + 4
    if start + count * stride > len(data):
        raise BakeError("SCX: truncated vertex stream")
    result = []
    for g in range(count // 4):
        vertices = []
        for i in range(4):
            offset = start + (g * 4 + i) * stride
            x, y = struct.unpack_from("<ff", data, offset)
            u, v = struct.unpack_from("<ff", data, offset + 24)
            if not all(math.isfinite(f) for f in (x, y, u, v)):
                raise BakeError(f"SCX: non-finite vertex in glyph {g}")
            if abs(x) > 40960 or abs(y) > 40960:
                raise BakeError(f"SCX: excessive position in glyph {g}")
            if not (-1e-6 <= u <= 1 + 1e-6 and -1e-6 <= v <= 1 + 1e-6):
                raise BakeError(f"SCX: UV outside atlas in glyph {g}")
            vertices.append(Vertex(x * 0.1, y * 0.1,
                                   min(1.0, max(0.0, u)), min(1.0, max(0.0, v))))
        glyph = Glyph(tuple(vertices))
        if glyph.advance < 0:
            raise BakeError(f"SCX: negative advance in glyph {g}")
        result.append(glyph)
    return result


def read_bounded(path: Path) -> bytes:
    if not path.is_file():
        raise BakeError(f"Input is not a regular file: {path}")
    with path.open("rb") as stream:
        data = stream.read(MAX_FILE_BYTES + 1)
    if not data or len(data) > MAX_FILE_BYTES:
        raise BakeError(f"Empty or oversized input: {path}")
    return data


def load_coverage(data: bytes):
    """Decode bounded PNG/TGA and return white-fill coverage as an L image.

    Alpha * luminance handles white-alpha, opaque greyscale, and SLII's white
    fill + black outline/shadow uniformly, including paletted TGA/PNG images.
    """
    Image, _, _ = dependencies()
    if not data or len(data) > MAX_FILE_BYTES:
        raise BakeError("Atlas: empty or oversized image file")
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(BytesIO(data)) as source:
                if source.format not in ("PNG", "TGA"):
                    raise BakeError("Atlas: only PNG and TGA are supported")
                w, h = source.size
                if (w <= 0 or h <= 0 or max(w, h) > MAX_IMAGE_DIMENSION
                        or w * h > MAX_IMAGE_PIXELS):
                    raise BakeError("Atlas: invalid or excessive image dimensions")
                rgba = source.convert("RGBA")
                # Pillow's L conversion uses Rec.601 luminance. Integer product
                # is rounded once, and thresholded at >=128 (coverage >=0.5).
                luma = rgba.convert("RGB").convert("L").tobytes()
                alpha = rgba.getchannel("A").tobytes()
                coverage = bytes((l * a + 127) // 255 for l, a in zip(luma, alpha))
                return Image.frombytes("L", (w, h), coverage)
    except (OSError, SyntaxError, ValueError, Image.DecompressionBombWarning,
            Image.DecompressionBombError) as exc:
        if isinstance(exc, BakeError):
            raise
        raise BakeError(f"Atlas: cannot decode bounded PNG/TGA: {exc}") from exc


def simplify_contour(points):
    """Remove repeated points and cyclic collinear corners, preserving winding."""
    clean = []
    for p in points:
        if not clean or p != clean[-1]:
            clean.append(p)
    if len(clean) > 1 and clean[0] == clean[-1]:
        clean.pop()
    if len(clean) < 3:
        return []
    return [b for a, b, c in zip(clean[-1:] + clean[:-1], clean,
                                 clean[1:] + clean[:1])
            if (b[0] - a[0]) * (c[1] - b[1]) != (b[1] - a[1]) * (c[0] - b[0])]


def trace_mask(mask: bytes, width: int, height: int):
    """Trace exposed pixel edges, foreground on the right in image coordinates.

    At diagonal contacts turn right first, keeping the two pixel components
    separate. After y inversion this produces clockwise outer TTF contours and
    counterclockwise holes, without a nesting heuristic.
    """
    if (not 0 <= width <= MAX_IMAGE_DIMENSION or not 0 <= height <= MAX_IMAGE_DIMENSION
            or width * height > MAX_GLYPH_PIXELS):
        raise BakeError("Glyph: excessive sampled dimensions")
    if len(mask) != width * height:
        raise BakeError("Glyph: mask dimensions do not match its data")
    edges = {}
    edge_count = 0

    def add(a, b):
        nonlocal edge_count
        edges.setdefault(a, set()).add(b)
        edge_count += 1
        if edge_count > MAX_BOUNDARY_EDGES:
            raise BakeError("Glyph: excessive outline complexity")

    for y in range(height):
        for x in range(width):
            i = y * width + x
            if mask[i] < 128:
                continue
            if y == 0 or mask[i - width] < 128:
                add((x, y), (x + 1, y))
            if x + 1 == width or mask[i + 1] < 128:
                add((x + 1, y), (x + 1, y + 1))
            if y + 1 == height or mask[i + width] < 128:
                add((x + 1, y + 1), (x, y + 1))
            if x == 0 or mask[i - 1] < 128:
                add((x, y + 1), (x, y))

    def direction(a, b):
        return {(1, 0): 0, (0, 1): 1, (-1, 0): 2, (0, -1): 3}[(b[0] - a[0], b[1] - a[1])]

    contours = []
    for start in sorted(edges):
        while edges.get(start):
            a = start
            b = min(edges[a])
            contour = []
            while True:
                contour.append(a)
                edges[a].remove(b)
                previous, a = a, b
                if a == start:
                    break
                candidates = edges.get(a)
                if not candidates:
                    raise BakeError("Glyph: open boundary in traced mask")
                incoming = direction(previous, a)
                # Right, straight, left, reverse, with deterministic tie breaks.
                b = min(candidates, key=lambda end:
                        ({1: 0, 0: 1, 3: 2, 2: 3}[(direction(a, end) - incoming) % 4], end))
            contour = simplify_contour(contour)
            if len(contour) >= 3:
                contours.append(contour)
    return contours


def glyph_mapping(glyph: Glyph):
    """Return the UV -> mesh affine map, or None for a zero-area blank quad."""
    p, q, r, s = glyph.vertices
    for attr in ("x", "y", "u", "v"):
        if not math.isclose(getattr(s, attr), getattr(q, attr) + getattr(r, attr)
                            - getattr(p, attr), rel_tol=1e-5, abs_tol=1e-5):
            raise BakeError("SCX: glyph quad is not affine")
    du, dv = q.u - p.u, q.v - p.v
    eu, ev = r.u - p.u, r.v - p.v
    determinant = du * ev - dv * eu
    mesh_area = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x)
    if abs(mesh_area) <= 1e-8:
        return None
    if abs(determinant) < 1e-12:
        raise BakeError("SCX: nonblank mesh has a degenerate UV quad")
    # Stock glyphs use rectangular UV cells, possibly reflected or transposed.
    if not ((abs(dv) < 1e-7 and abs(eu) < 1e-7)
            or (abs(du) < 1e-7 and abs(ev) < 1e-7)):
        raise BakeError("SCX: glyph UV quad is not a rectangle")

    def mapping(u, v):
        a = ((u - p.u) * ev - (v - p.v) * eu) / determinant
        b = (du * (v - p.v) - dv * (u - p.u)) / determinant
        return (p.x + a * (q.x - p.x) + b * (r.x - p.x),
                p.y + a * (q.y - p.y) + b * (r.y - p.y))

    return mapping


def round_units(value):
    # Match OpenType's round-to-nearest, ties towards positive infinity.
    return math.floor(value + 0.5)


def glyph_contours(glyph: Glyph, coverage, scale: float):
    mapping = glyph_mapping(glyph)
    if mapping is None:
        return [], 0
    w, h = coverage.size
    left = min(p.u for p in glyph.vertices) * w
    right = max(p.u for p in glyph.vertices) * w
    top = min(p.v for p in glyph.vertices) * h
    bottom = max(p.v for p in glyph.vertices) * h
    # Clip partial pixel cells to the precise SCX UV rectangle after tracing.
    x0, y0, x1, y1 = math.floor(left), math.floor(top), math.ceil(right), math.ceil(bottom)
    # Trace the interpolated coverage at quarter-pixel resolution, rather than
    # quantizing antialiased diagonal edges to whole source-pixel stair steps.
    Image, _, _ = dependencies()
    sample_scale = 4
    sample_w, sample_h = (x1 - x0) * sample_scale, (y1 - y0) * sample_scale
    pixels = sample_w * sample_h
    if pixels > MAX_GLYPH_PIXELS:
        raise BakeError("Glyph: excessive atlas crop")
    mask = coverage.crop((x0, y0, x1, y1)).resize(
        (sample_w, sample_h), Image.Resampling.BILINEAR).tobytes()
    p, q, r, _ = glyph.vertices
    mesh_area = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x)
    uv_area = (q.u - p.u) * (r.v - p.v) - (q.v - p.v) * (r.u - p.u)
    contours = []
    for boundary in trace_mask(mask, sample_w, sample_h):
        if mesh_area * uv_area < 0:
            boundary.reverse()  # Reflections must not reverse TrueType winding.
        points = []
        for x, y in boundary:
            u = min(right, max(left, x / sample_scale + x0)) / w
            v = min(bottom, max(top, y / sample_scale + y0)) / h
            px, py = mapping(u, v)
            point = (round_units(px * scale), round_units(-py * scale))
            if any(not -32768 <= c <= 32767 for c in point):
                raise BakeError("Glyph: outline coordinate exceeds TrueType int16 range")
            points.append(point)
        points = simplify_contour(points)
        if len(points) >= 3:
            contours.append(points)
    if sum(map(len, contours)) > MAX_GLYPH_POINTS:
        raise BakeError("Glyph: too many TrueType outline points")
    return contours, pixels


def bake_font(scx: bytes, atlas: bytes, stem: str, px_height: int) -> bytes:
    """Produce a deterministic, unhinted outline TTF, without filesystem writes."""
    _, FontBuilder, TTGlyphPen = dependencies()
    if stem not in FONT_HEIGHTS or px_height != FONT_HEIGHTS[stem]:
        raise BakeError("Unsupported stock font stem/height")
    source = parse_scx(scx)
    coverage = load_coverage(atlas)
    scale = UNITS_PER_EM / px_height
    names = [".notdef"] + [f"byte{c:03d}" for c in range(256)]
    glyphs, metrics = {}, {}
    empty = TTGlyphPen(None).glyph()
    glyphs[".notdef"] = empty
    metrics[".notdef"] = (0, 0)
    min_y = max_y = sampled = 0
    for c, name in enumerate(names[1:]):
        pen = TTGlyphPen(None)
        if c < len(source):
            glyph = source[c]
            contours, pixels = glyph_contours(glyph, coverage, scale)
            sampled += pixels
            if sampled > MAX_FONT_SAMPLED_PIXELS:
                raise BakeError("Font: excessive total atlas sampling")
            for contour in contours:
                pen.moveTo(contour[0])
                for point in contour[1:]:
                    pen.lineTo(point)
                pen.closePath()
            advance = round_units(glyph.advance * scale)
            lsb = (min(x for contour in contours for x, _ in contour) if contours else
                   round_units(min(v.x for v in glyph.vertices) * scale))
            if not 0 <= advance <= 65535 or not -32768 <= lsb <= 32767:
                raise BakeError("Glyph: advance/bearing exceeds TrueType metric range")
            for contour in contours:
                min_y = min(min_y, min(y for _, y in contour))
                max_y = max(max_y, max(y for _, y in contour))
            metrics[name] = (advance, lsb)
        else:
            metrics[name] = (0, 0)
        glyphs[name] = pen.glyph()
    builder = FontBuilder(UNITS_PER_EM, isTTF=True)
    builder.setupGlyphOrder(names)
    builder.setupCharacterMap({c: names[c + 1] for c in range(256)})
    builder.setupGlyf(glyphs)
    builder.setupHorizontalMetrics(metrics)
    descent = min(-UNITS_PER_EM, min_y)
    builder.setupHorizontalHeader(ascent=0, descent=descent, lineGap=0)
    builder.setupOS2(sTypoAscender=0, sTypoDescender=descent, sTypoLineGap=0,
                     usWinAscent=max_y, usWinDescent=-min_y)
    builder.setupNameTable({"familyName": f"SLRR {stem} baked", "styleName": "Regular",
                            "uniqueFontIdentifier": f"SLRR-{stem}-outline-v1",
                            "fullName": f"SLRR {stem} baked Regular",
                            "psName": f"SLRR-{stem}-Baked", "version": "Version 1.000"})
    builder.setupPost()
    builder.setupMaxp()
    builder.font["head"].created = FIXED_TIMESTAMP
    builder.font["head"].modified = FIXED_TIMESTAMP
    builder.font.recalcTimestamp = False
    output = BytesIO()
    builder.font.save(output)
    return output.getvalue()


def case_child(parent: Path, name: str) -> Path:
    matches = [p for p in parent.iterdir() if p.name.casefold() == name.casefold()]
    if len(matches) != 1:
        raise BakeError(f"Missing or case-ambiguous asset: {parent / name}")
    return matches[0]


def pristine_path(path: Path) -> bool:
    normalized = str(path.resolve()).replace("\\", "/").casefold().rstrip("/")
    return any(normalized == root or normalized.startswith(root + "/") for root in (
        "c:/openslrr/open-sl/game", "/mnt/c/openslrr/open-sl/game"))


def bake_all(root: Path, out: Path | None = None) -> list[Path]:
    """Bake all supported stems; preflight every input/output before writing."""
    root = root.resolve()
    out = (out if out is not None else root / "frontend" / "fonts").resolve()
    if pristine_path(root) or pristine_path(out):
        raise BakeError("Refusing pristine C:\\openslrr\\open-SL\\game; use a private game copy")
    if not root.is_dir():
        raise BakeError(f"Private game root is not a directory: {root}")
    frontend = case_child(root, "frontend")
    meshes = case_child(case_child(frontend, "meshes"), "fonts")
    textures = case_child(case_child(frontend, "textures"), "fonts")
    targets = [out / f"{stem}.ttf" for stem in FONT_HEIGHTS]
    for target in targets:
        if target.exists() or target.is_symlink():
            raise BakeError(f"Refusing to overwrite existing output: {target}")
    jobs = []
    for stem, height in FONT_HEIGHTS.items():
        scx_path = case_child(meshes, stem + ".scx")
        pngs = [p for p in textures.iterdir() if p.name.casefold() == stem + ".png"]
        atlas_path = case_child(textures, stem + (".png" if pngs else ".tga"))
        jobs.append((stem, height, read_bounded(scx_path), read_bounded(atlas_path)))
    # Decode and validate every font before publishing any output.
    fonts = [bake_font(scx, atlas, stem, height) for stem, height, scx, atlas in jobs]
    out.mkdir(parents=True, exist_ok=True)
    for target, data in zip(targets, fonts):
        # Exclusive creation also rejects symlinks/races and never truncates inputs.
        with target.open("xb") as stream:
            stream.write(data)
    return targets


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, required=True, help="private game copy (read-only inputs)")
    parser.add_argument("--out", type=Path, help="output directory (default: <root>/frontend/fonts)")
    args = parser.parse_args(argv)
    try:
        dependencies()
        for path in bake_all(args.root, args.out):
            print(path)
    except (BakeError, OSError) as exc:
        print(f"Font bake failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
