"""Synthetic, public-domain-by-construction fixtures; no game assets required.

Run: python -m unittest discover -s tests -p test_bake_slrr_fonts.py -v
Optional CFF runtime fixture: python tests/test_bake_slrr_fonts.py --write-otf PATH
Tool-only test dependencies: Pillow and fontTools.
"""

from io import BytesIO
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bake_slrr_fonts as bake

try:
    from PIL import Image
    from fontTools.ttLib import TTFont
except ImportError:
    Image = TTFont = None


def scx_fixture(quads=None, count=256, header=0x38, stride=44):
    """INVO v3 vertex stream, with optional codepoint -> four vertices mapping."""
    data = bytearray(8 + header + 4 + count * 4 * stride)
    struct.pack_into("<4sII", data, 0, b"INVO", 3, header)
    if header >= 0x40:
        struct.pack_into("<I", data, 8 + 0x3C, stride)
    struct.pack_into("<I", data, 8 + header, count * 4)
    for code, quad in (quads or {}).items():
        for i, (x, y, u, v) in enumerate(quad):
            offset = 8 + header + 4 + (code * 4 + i) * stride
            struct.pack_into("<ff", data, offset, x * 10, y * 10)
            struct.pack_into("<ff", data, offset + 24, u, v)
    return bytes(data)


def rectangle(x0=0, y0=0, x1=8, y1=8, u0=0, v0=0, u1=1, v1=1):
    return [(x0, y0, u0, v0), (x1, y0, u1, v0),
            (x0, y1, u0, v1), (x1, y1, u1, v1)]


def image_bytes(image, fmt="PNG"):
    stream = BytesIO()
    image.save(stream, format=fmt)
    return stream.getvalue()


def area(points):
    return sum(a[0] * b[1] - b[0] * a[1]
               for a, b in zip(points, points[1:] + points[:1])) / 2


def font_contours(font, name):
    glyph = font["glyf"][name]
    coordinates, ends, flags = glyph.getCoordinates(font["glyf"])
    contours = []
    start = 0
    for end in ends:
        contours.append(list(coordinates[start:end + 1]))
        start = end + 1
    return contours


def write_otf_fixture(path):
    """Independent CFF OTF for stb_truetype integration; A advances 12px at 20px em."""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    builder = FontBuilder(1000, isTTF=False)
    builder.setupGlyphOrder([".notdef", "A", "O", "B"])
    builder.setupCharacterMap({65: "A", 79: "O", 66: "B"})
    empty = T2CharStringPen(0, None).getCharString()
    pen = T2CharStringPen(600, None)
    pen.moveTo((50, 0))
    pen.lineTo((550, 0))
    pen.lineTo((300, 700))
    pen.closePath()
    a_charstring = pen.getCharString()
    pen = T2CharStringPen(600, None)

    def circle(radius, clockwise):
        # Four cubic Bezier quadrants, center (300,300). Outer is clockwise;
        # inner is counterclockwise, so the CFF nonzero fill keeps its hole.
        k = round(radius * 0.5522847498307936)
        sign = -1 if clockwise else 1
        pen.moveTo((300 + radius, 300))
        pen.curveTo((300 + radius, 300 + sign * k),
                    (300 + k, 300 + sign * radius), (300, 300 + sign * radius))
        pen.curveTo((300 - k, 300 + sign * radius),
                    (300 - radius, 300 + sign * k), (300 - radius, 300))
        pen.curveTo((300 - radius, 300 - sign * k),
                    (300 - k, 300 - sign * radius), (300, 300 - sign * radius))
        pen.curveTo((300 + k, 300 - sign * radius),
                    (300 + radius, 300 - sign * k), (300 + radius, 300))
        pen.closePath()

    circle(250, True)
    circle(125, False)
    o_charstring = pen.getCharString()
    pen = T2CharStringPen(600, None)
    for x0, y0, x1, y1 in [(0, 0, 400, 600), (225, 200, 600, 800)]:
        pen.moveTo((x0, y0))
        pen.lineTo((x0, y1))
        pen.lineTo((x1, y1))
        pen.lineTo((x1, y0))
        pen.closePath()
    builder.setupCFF("SyntheticCFF-Regular", {},
                     {".notdef": empty, "A": a_charstring, "O": o_charstring,
                      "B": pen.getCharString()}, {})
    builder.setupHorizontalMetrics({".notdef": (0, 0), "A": (600, 50), "O": (600, 50), "B": (600, 0)})
    builder.setupHorizontalHeader(ascent=800, descent=-200)
    builder.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    builder.setupNameTable({"familyName": "Synthetic CFF", "styleName": "Regular",
                            "uniqueFontIdentifier": "SyntheticCFF-Regular-1",
                            "fullName": "Synthetic CFF Regular", "psName": "SyntheticCFF-Regular",
                            "version": "Version 1.000"})
    builder.setupPost()
    builder.font["head"].created = bake.FIXED_TIMESTAMP
    builder.font["head"].modified = bake.FIXED_TIMESTAMP
    builder.font.recalcTimestamp = False
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    # Same no-overwrite policy as the actual baker.
    with path.open("xb") as stream:
        builder.font.save(stream)
    return path


class BoundaryTests(unittest.TestCase):
    def test_solid_shape_is_one_contour_not_pixel_rectangles(self):
        contours = bake.trace_mask(bytes([255] * 48), 8, 6)
        self.assertEqual(len(contours), 1)
        self.assertEqual(len(contours[0]), 4)
        self.assertEqual(area(contours[0]), 48)

    def test_hole_opposite_winding_and_collinear_removal(self):
        mask = bytes(255 if x in (0, 4) or y in (0, 4) else 0
                     for y in range(5) for x in range(5))
        contours = bake.trace_mask(mask, 5, 5)
        self.assertEqual(sorted(map(len, contours)), [4, 4])
        self.assertEqual(sorted(map(area, contours)), [-9, 25])

    def test_diagonal_components_do_not_join_at_a_corner(self):
        contours = bake.trace_mask(bytes([255, 0, 0, 255]), 2, 2)
        self.assertEqual(len(contours), 2)
        self.assertEqual([area(c) for c in contours], [1, 1])
        self.assertEqual([len(c) for c in contours], [4, 4])

    def test_nested_hole_and_island(self):
        mask = bytes(255 if x in (0, 6) or y in (0, 6) or (x, y) == (3, 3) else 0
                     for y in range(7) for x in range(7))
        self.assertEqual(sorted(map(area, bake.trace_mask(mask, 7, 7))), [-25, 1, 49])

    def test_every_small_mask_preserves_signed_ink_area(self):
        for bits in range(1 << 9):
            mask = bytes(255 if bits & (1 << i) else 0 for i in range(9))
            contours = bake.trace_mask(mask, 3, 3)
            self.assertEqual(sum(map(area, contours)), bits.bit_count(), bits)

    def test_threshold_is_half_coverage(self):
        contours = bake.trace_mask(bytes([127, 128]), 2, 1)
        self.assertEqual(len(contours), 1)
        self.assertEqual(set(contours[0]), {(1, 0), (2, 0), (2, 1), (1, 1)})

    def test_mask_bounds(self):
        for mask, width, height in [(b"", -1, 0), (b"", 1, 1),
                                     (b"", 0, 10**12), (b"", 4096, 4096)]:
            with self.subTest(width=width, height=height):
                with self.assertRaises(bake.BakeError):
                    bake.trace_mask(mask, width, height)


class SCXTests(unittest.TestCase):
    def test_normal_and_extended_stride(self):
        for header, stride in [(0x38, 44), (0x40, 32), (0x40, 64)]:
            with self.subTest(header=header, stride=stride):
                glyph = bake.parse_scx(scx_fixture({65: rectangle(-2, 1, 6, 9)},
                                                  header=header, stride=stride))[65]
                self.assertEqual(glyph.advance, 6)
                self.assertEqual(glyph.vertices[0].x, -2)

    def test_malformed_headers_streams_and_vertices(self):
        good = scx_fixture({65: rectangle()})
        bad = [b"", b"NOPE" + good[4:], good[:10], good[:70], good[:-1]]
        for offset, fmt, value in [(4, "I", 4), (8, "I", 0), (8, "I", 0x1001),
                                    (8, "I", 0x3D), (64, "I", 0), (64, "I", 5),
                                    (64, "I", 1028), (68, "f", float("nan")),
                                    (68, "f", float("inf")), (68, "f", 50000),
                                    (68 + 24, "f", -0.1), (68 + 28, "f", 1.1),
                                    (68 + 44, "f", -10)]:
            data = bytearray(good)
            struct.pack_into("<" + fmt, data, offset, value)
            bad.append(bytes(data))
        extended = bytearray(scx_fixture(header=0x40, stride=44))
        for stride in (0, 31, 257):
            data = extended.copy()
            struct.pack_into("<I", data, 8 + 0x3C, stride)
            bad.append(bytes(data))
        for i, data in enumerate(bad):
            with self.subTest(case=i):
                with self.assertRaises(bake.BakeError):
                    bake.parse_scx(data)

    def test_nonaffine_and_degenerate_quads_rejected(self):
        cases = [rectangle(), rectangle(u1=0),
                 [(0, 0, 0, 0), (8, 0, .8, .1), (0, 8, .1, .8), (8, 8, .9, .9)]]
        cases[0][3] = (9, 8, 1, 1)
        for quad in cases:
            with self.subTest(quad=quad):
                with self.assertRaises(bake.BakeError):
                    glyph = bake.parse_scx(scx_fixture({0: quad}))[0]
                    bake.glyph_mapping(glyph)

    def test_blank_quad_retains_space_advance(self):
        glyph = bake.parse_scx(scx_fixture({32: rectangle(0, 0, 3.25, 0)}))[32]
        self.assertEqual(glyph.advance, 3.25)
        self.assertIsNone(bake.glyph_mapping(glyph))


@unittest.skipUnless(Image is not None and TTFont is not None, "Requires Pillow and fontTools")
class BakingTests(unittest.TestCase):
    def test_white_fill_excludes_black_shadow(self):
        atlas = Image.new("RGBA", (5, 1))
        atlas.putdata([(255, 255, 255, 255), (0, 0, 0, 255), (255, 255, 255, 0),
                       (255, 255, 255, 128), (128, 128, 128, 128)])
        coverage = bake.load_coverage(image_bytes(atlas))
        self.assertEqual(list(coverage.tobytes()), [255, 0, 0, 128, 64])

    def test_opaque_grayscale_and_white_alpha_and_tga(self):
        for mode, values, expected in [
            ("L", [0, 127, 128, 255], [0, 127, 128, 255]),
            ("RGBA", [(255, 255, 255, a) for a in (0, 127, 128, 255)], [0, 127, 128, 255]),
        ]:
            for fmt in ("PNG", "TGA"):
                with self.subTest(mode=mode, fmt=fmt):
                    atlas = Image.new(mode, (2, 2))
                    atlas.putdata(values)
                    self.assertEqual(list(bake.load_coverage(image_bytes(atlas, fmt)).tobytes()), expected)
        atlas = Image.new("P", (2, 1))
        atlas.putpalette([0, 0, 0, 255, 255, 255] + [0] * (768 - 6))
        atlas.putdata([0, 1])
        self.assertEqual(list(bake.load_coverage(image_bytes(atlas, "TGA")).tobytes()), [0, 255])

    def test_image_bounds_and_decode_errors(self):
        for data in (b"", b"not an image", image_bytes(Image.new("L", (1, 1)), "GIF")):
            with self.assertRaises(bake.BakeError):
                bake.load_coverage(data)
        with patch.object(bake, "MAX_IMAGE_DIMENSION", 4):
            with self.assertRaisesRegex(bake.BakeError, "dimensions"):
                bake.load_coverage(image_bytes(Image.new("L", (5, 1))))
        with patch.object(bake, "MAX_IMAGE_PIXELS", 3):
            with self.assertRaisesRegex(bake.BakeError, "dimensions"):
                bake.load_coverage(image_bytes(Image.new("L", (2, 2))))

    def test_outline_holes_affine_metrics_blanks_and_determinism(self):
        atlas = Image.new("L", (8, 8))
        for y in range(2, 6):
            for x in range(2, 6):
                if x in (2, 5) or y in (2, 5):
                    atlas.putpixel((x, y), 255)
        quad = [(-2, 1, .25, .25), (6, 2, .75, .25),
                (-1, 5, .25, .75), (7, 6, .75, .75)]
        scx = scx_fixture({32: rectangle(0, 0, 3.25, 0), 65: quad, 255: quad})
        png = image_bytes(atlas)
        data = bake.bake_font(scx, png, "simple20", 20)
        self.assertEqual(data, bake.bake_font(scx, png, "simple20", 20))
        font = TTFont(BytesIO(data))
        self.assertEqual(font["head"].unitsPerEm, 1024)
        self.assertEqual(font["head"].created, bake.FIXED_TIMESTAMP)
        self.assertEqual(font["hhea"].ascent, 0)
        self.assertEqual(font["OS/2"].sTypoAscender, 0)
        self.assertEqual(set(font.getBestCmap()), set(range(256)))
        self.assertEqual(font.getGlyphOrder()[0], ".notdef")
        self.assertEqual(font["maxp"].numGlyphs, 257)
        self.assertEqual(font["glyf"][".notdef"].numberOfContours, 0)
        self.assertEqual(font["glyf"]["byte032"].numberOfContours, 0)
        self.assertEqual(font["glyf"]["byte000"].numberOfContours, 0)
        self.assertEqual(font["hmtx"]["byte032"], (166, 0))
        self.assertEqual(font["hmtx"]["byte065"], (307, -102))
        # At em=20px the stock advance is 6px, NOT quad width 8px.
        self.assertAlmostEqual(font["hmtx"]["byte065"][0] * 20 / 1024, 6, delta=20 / 2048)
        glyph = font["glyf"]["byte065"]
        self.assertEqual((glyph.xMin, glyph.yMin, glyph.xMax, glyph.yMax), (-102, -307, 358, -51))
        contours = font_contours(font, "byte065")
        # Interpolated coverage rounds the hole corners at quarter-pixel scale;
        # the outside contour remains clipped to the exact four UV corners.
        self.assertEqual(sorted(map(len, contours)), [4, 12])
        self.assertLess(area(contours[0]), 0)  # Clockwise outer in TTF coordinates.
        self.assertGreater(area(contours[1]), 0)
        for table in ("EBDT", "EBLC", "CBDT", "CBLC", "sbix", "SVG "):
            self.assertNotIn(table, font)

    def test_antialiased_edges_keep_subpixel_contour_positions(self):
        atlas = Image.new("L", (4, 4))
        atlas.putdata([0, 80, 240, 255] * 4)
        data = bake.bake_font(scx_fixture({65: rectangle(0, 0, 4, 4)}),
                              image_bytes(atlas), "simple20", 20)
        glyph = TTFont(BytesIO(data))["glyf"]["byte065"]
        # The 50% crossing is near x=1.8, not the whole-pixel threshold x=2.
        self.assertAlmostEqual(glyph.xMin * 20 / 1024, 1.75, delta=.02)

    def test_partial_uv_cell_mapping_is_clipped_to_exact_quad(self):
        scx = scx_fixture({65: rectangle(-1, -2, 7, 6, .125, .125, .875, .875)})
        data = bake.bake_font(scx, image_bytes(Image.new("L", (4, 4), 255)), "simple20", 20)
        font = TTFont(BytesIO(data))
        g = font["glyf"]["byte065"]
        self.assertEqual((g.xMin, g.yMin, g.xMax, g.yMax), (-51, -307, 358, 102))

    def test_reflected_uv_preserves_outline_winding(self):
        scx = scx_fixture({65: rectangle(-2, 0, 6, 8, 1, 0, 0, 1)})
        data = bake.bake_font(scx, image_bytes(Image.new("L", (4, 4), 255)), "simple20", 20)
        font = TTFont(BytesIO(data))
        contours = font_contours(font, "byte065")
        self.assertEqual(len(contours), 1)
        self.assertLess(area(contours[0]), 0)
        self.assertEqual(font["hmtx"]["byte065"], (307, -102))

    def test_short_charset_pads_missing_byte_glyphs(self):
        data = bake.bake_font(scx_fixture({0: rectangle()}, count=1),
                              image_bytes(Image.new("L", (2, 2), 255)), "simple20", 20)
        font = TTFont(BytesIO(data))
        self.assertEqual(font["glyf"]["byte255"].numberOfContours, 0)
        self.assertEqual(font["hmtx"]["byte255"], (0, 0))
        self.assertEqual(font.getBestCmap()[255], "byte255")

    def test_outline_and_work_limits_fail_cleanly(self):
        scx = scx_fixture({0: rectangle(), 1: rectangle()})
        png = image_bytes(Image.new("L", (4, 4), 255))
        for constant, value in [("MAX_GLYPH_PIXELS", 8), ("MAX_FONT_SAMPLED_PIXELS", 20),
                                 ("MAX_BOUNDARY_EDGES", 4), ("MAX_GLYPH_POINTS", 3)]:
            with self.subTest(constant=constant), patch.object(bake, constant, value):
                with self.assertRaises(bake.BakeError):
                    bake.bake_font(scx, png, "simple20", 20)
        with self.assertRaisesRegex(bake.BakeError, "int16"):
            bake.bake_font(scx_fixture({0: rectangle(0, 0, 4000, 4000)}), png, "simple20", 20)
        with self.assertRaises(bake.BakeError):
            bake.bake_font(scx, png, "slii18", 18)

    def make_private_root(self, base):
        root = base / "private fixture game"
        meshes = root / "frontend/meshes/Fonts"
        textures = root / "frontend/textures/Fonts"
        meshes.mkdir(parents=True)
        textures.mkdir(parents=True)
        scx = scx_fixture({32: rectangle(0, 0, 3, 0), 65: rectangle()})
        png = image_bytes(Image.new("L", (4, 4), 255))
        for stem in bake.FONT_HEIGHTS:
            (meshes / (stem + ".scx")).write_bytes(scx)
            # Exercise case-insensitive matching, PNG precedence, and TGA fallback.
            if stem == "console5":
                (textures / (stem + ".tga")).write_bytes(image_bytes(Image.new("L", (4, 4), 255), "TGA"))
            else:
                (textures / (stem.upper() + ".PNG")).write_bytes(png)
                (textures / (stem + ".tga")).write_bytes(b"not the charset")
        return root

    def test_cli_all_nine_default_override_and_no_input_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="bake-font-tests-") as tmp:
            base = Path(tmp)
            root = self.make_private_root(base)
            before = {p: p.read_bytes() for p in root.rglob("*") if p.is_file()}
            command = [sys.executable, str(Path(bake.__file__)), "--root", str(root)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            default = root / "frontend/fonts"
            self.assertEqual({p.stem for p in default.glob("*.ttf")}, set(bake.FONT_HEIGHTS))
            for stem, height in bake.FONT_HEIGHTS.items():
                font = TTFont(default / (stem + ".ttf"))
                self.assertAlmostEqual(font["hmtx"]["byte065"][0] * height / 1024, 8, delta=height / 2048)
            again = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(again.returncode, 0)
            self.assertIn("Refusing to overwrite", again.stderr)
            override = base / "separate output"
            result = subprocess.run(command + ["--out", str(override)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for target in default.glob("*.ttf"):
                self.assertEqual(target.read_bytes(), (override / target.name).read_bytes())
            for path, data in before.items():
                self.assertEqual(path.read_bytes(), data)

    def test_preflight_failure_does_not_publish_partial_outputs(self):
        with tempfile.TemporaryDirectory(prefix="bake-font-tests-") as tmp:
            root = self.make_private_root(Path(tmp))
            # Malformed final font must fail before even the first TTF is created.
            (root / "frontend/meshes/Fonts/slii17.scx").write_bytes(b"bad")
            out = Path(tmp) / "outputs"
            with self.assertRaises(bake.BakeError):
                bake.bake_all(root, out)
            self.assertFalse(out.exists())

    def test_output_symlink_does_not_truncate_input(self):
        with tempfile.TemporaryDirectory(prefix="bake-font-tests-") as tmp:
            root = self.make_private_root(Path(tmp))
            out = Path(tmp) / "outputs"
            out.mkdir()
            source = root / "frontend/meshes/Fonts/simple20.scx"
            original = source.read_bytes()
            try:
                (out / "simple20.ttf").symlink_to(source)
            except OSError:
                self.skipTest("Creating symlinks is unavailable")
            with self.assertRaises(bake.BakeError):
                bake.bake_all(root, out)
            self.assertEqual(source.read_bytes(), original)

    def test_synthetic_cff_fixture(self):
        with tempfile.TemporaryDirectory(prefix="bake-cff-tests-") as tmp:
            path = write_otf_fixture(Path(tmp) / "test.otf")
            font = TTFont(path)
            self.assertEqual(font.sfntVersion, "OTTO")
            self.assertIn("CFF ", font)
            self.assertNotIn("glyf", font)
            self.assertEqual(font.getBestCmap()[65], "A")
            self.assertEqual(font["head"].unitsPerEm, 1000)
            self.assertEqual(font["hmtx"]["A"], (600, 50))
            from fontTools.pens.boundsPen import BoundsPen
            pen = BoundsPen(font.getGlyphSet())
            font.getGlyphSet()["A"].draw(pen)
            self.assertEqual(pen.bounds, (50, 0, 550, 700))
            self.assertAlmostEqual(font["hmtx"]["A"][0] * 20 / 1000, 12)
            self.assertEqual(font.getBestCmap()[79], "O")
            pen = BoundsPen(font.getGlyphSet())
            font.getGlyphSet()["O"].draw(pen)
            self.assertEqual(pen.bounds, (50, 50, 550, 550))
            from fontTools.pens.recordingPen import RecordingPen
            pen = RecordingPen()
            font.getGlyphSet()["O"].draw(pen)
            self.assertEqual(sum(op == "curveTo" for op, _ in pen.value), 8)
            self.assertEqual(sum(op == "closePath" for op, _ in pen.value), 2)
            endpoint_loops = []
            for op, points in pen.value:
                if op == "moveTo":
                    endpoint_loops.append([points[0]])
                elif op == "curveTo":
                    endpoint_loops[-1].append(points[-1])
            self.assertLess(area(endpoint_loops[0]), 0)
            self.assertGreater(area(endpoint_loops[1]), 0)
            self.assertEqual(set(endpoint_loops[1]), {(425, 300), (300, 425), (175, 300), (300, 175)})
            duplicate = write_otf_fixture(Path(tmp) / "second.otf")
            self.assertEqual(path.read_bytes(), duplicate.read_bytes())


class CLISafetyTests(unittest.TestCase):
    def test_missing_dependencies_clear_error_and_help_still_works(self):
        # -S disables site-packages even in the dependency-equipped test venv.
        command = [sys.executable, "-S", str(Path(bake.__file__))]
        help_result = subprocess.run(command + ["--help"], capture_output=True, text=True)
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        result = subprocess.run(command + ["--root", "."], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("python -m pip install Pillow fontTools", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_pristine_output_and_root_rejected_without_asset_reads(self):
        for path in (Path("/mnt/c/openslrr/open-SL/game"),
                     Path("/mnt/c/openslrr/open-SL/game/frontend/fonts")):
            self.assertTrue(bake.pristine_path(path))
        with patch.object(Path, "iterdir", side_effect=AssertionError("must not inspect pristine assets")):
            with self.assertRaisesRegex(bake.BakeError, "Refusing pristine"):
                bake.bake_all(Path("/mnt/c/openslrr/open-SL/game"))
            with self.assertRaisesRegex(bake.BakeError, "Refusing pristine"):
                bake.bake_all(Path("."), Path("/mnt/c/openslrr/open-SL/game/frontend/fonts"))


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--write-otf":
        print(write_otf_fixture(sys.argv[2]))
    else:
        unittest.main()
