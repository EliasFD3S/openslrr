"""Run with python -m unittest discover -s tests -p test_native_coverage.py."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("native_coverage", Path(__file__).resolve().parents[1] / "scripts/render_native_coverage.py")
coverage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coverage)


def row(signature="int probe()", parameters="()"):
    return {"class": "java.lang.Probe", "method": "probe", "typed_signature": signature, "descriptor": parameters}


def classify(signature, registrations, parameters="()"):
    return coverage.classify_rows([row(signature, parameters)], {
        ("java.lang.Probe", "probe", descriptor): kind for descriptor, kind in registrations.items()
    })[0][2]


class DescriptorTests(unittest.TestCase):
    def test_exact_return_and_noop(self):
        self.assertEqual(classify("int probe()", {"()I": "body"}), "body")
        self.assertEqual(classify("void probe()", {"()V": "noop"}), "noop")

    def test_only_void_may_be_omitted(self):
        self.assertEqual(classify("void probe()", {"()": "body"}), "body")
        for actual in ["()", "()V", "()F", "()Ljava.lang.Object;"]:
            with self.subTest(actual=actual), self.assertRaisesRegex(SystemExit, "expected \\(\\)I"):
                classify("int probe()", {actual: "body"})

    def test_dual_registrations_keep_exact_priority(self):
        self.assertEqual(classify("void probe()", {"()": "body", "()V": "noop"}), "noop")
        self.assertEqual(classify("float probe()", {"()V": "noop", "()F": "body"}), "body")

    def test_object_and_array_returns(self):
        self.assertEqual(classify("String probe()", {"()Ljava.lang.String;": "body"}), "body")
        self.assertEqual(classify("Vector3[] probe()", {"()[Ljava.lang.Vector3;": "body"}), "body")
        with self.assertRaisesRegex(SystemExit, "return descriptor mismatch"):
            classify("Object probe()", {"()Ljava.lang.String;": "body"})

    def test_missing_and_distinct_overload(self):
        self.assertEqual(classify("int probe()", {}), "missing")
        self.assertEqual(classify("int probe()", {"(I)F": "body"}), "missing")

    def test_descriptor_syntax_and_slashes(self):
        for valid in ["()", "()V", "(IF)I", "([I[[Ljava.lang.Object;)Ljava.lang.String;"]:
            coverage.validate_runtime_descriptor(valid)
        for invalid in ["(Ljava/lang/Object;)V", "()Ljava/lang/String;", "(V)V", "()[V", "(I", "()junk"]:
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                coverage.validate_runtime_descriptor(invalid)

    def test_overloads_do_not_collapse(self):
        rows = [row("float probe(float value)", "(F)"), row("int probe(int value)", "(I)")]
        host = {("java.lang.Probe", "probe", "(F)F"): "body", ("java.lang.Probe", "probe", "(I)I"): "noop"}
        self.assertEqual([kind for _, _, kind in coverage.classify_rows(rows, host)], ["body", "noop"])

    def test_source_scanner_reports_slash_location(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src"
            source.mkdir()
            (source / "probe.cpp").write_text('r.register_method("java.lang.Probe", "probe", "(Ljava/lang/Object;)V", native_noop);', encoding="utf-8")
            with patch.object(coverage, "ROOT", root), patch.object(coverage, "SOURCE_DIR", source):
                with self.assertRaisesRegex(SystemExit, "src/probe.cpp:1: slash-form"):
                    coverage.host_registrations()

    def test_check_fails_before_writing_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src"
            source.mkdir()
            (source / "probe.cpp").write_text('r.register_method("java.game.parts.bodypart.Chassis", "getTorque", "(FF)I", native_noop);', encoding="utf-8")
            output = root / "coverage.json"
            output.write_text("{}", encoding="utf-8")
            with patch.object(coverage, "ROOT", root), patch.object(coverage, "SOURCE_DIR", source), patch.object(coverage, "JSON_PATH", output), patch("sys.argv", ["render_native_coverage.py", "--check"]):
                with self.assertRaisesRegex(SystemExit, "expected \\(FF\\)F, registered \\(FF\\)I"):
                    coverage.main()
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")), {})


if __name__ == "__main__":
    unittest.main()
