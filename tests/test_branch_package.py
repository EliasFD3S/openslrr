"""Offline producer contract tests. Run with Python 3.11+ and Git."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import branch_package as producer


class Identity(unittest.TestCase):
    def test_exact_utf8_tag(self):
        commit = "a" * 40
        producer.validate_inputs("feature/Éngine", commit)
        self.assertEqual(producer.release_tag("feature/Éngine", commit),
                         "branch-0677f0dd220e2a4725cac7601b53c86354f8df86d3af44418e135d0ade2dcb0b-" + commit)

    def test_reject_invalid_inputs(self):
        for branch, commit in [("refs/pull/2/head", "a" * 40), ("a:b", "a" * 40),
                               ("feature/*", "a" * 40), ("main", "a" * 7), ("main", "A" * 40)]:
            with self.subTest(branch=branch, commit=commit), self.assertRaises((ValueError, subprocess.CalledProcessError)):
                producer.validate_inputs(branch, commit)

    def test_exact_remote_ref_and_sha(self):
        commit, branch = "1" * 40, "feature/topic"
        response = f"{commit}\trefs/heads/{branch}\n".encode()
        with patch.object(subprocess, "check_output", return_value=response) as run:
            producer.check_tip(branch, commit)
            self.assertEqual(run.call_args.args[0][-2:], [
                "https://github.com/EliasFD3S/openslrr.git", "refs/heads/feature/topic"])
            for invalid in (response.upper(), response.replace(b"topic", b"Topic"), response * 2, b""):
                run.return_value = invalid
                with self.assertRaises(ValueError):
                    producer.check_tip(branch, commit)


class Package(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.repo = self.root / "engine"
        self.repo.mkdir()
        self.git("init", "-q")
        self.files = {"LICENSE": b"engine\n", "third_party/slrrc/LICENSE": b"compiler\n",
                      "NOTICE.txt": b"engine notice\n", "patch/README.md": b"docs only", "data/game/base.rpk": b"private"}
        self.overlays = {"patch/sl/Scripts/src/Project.java": b"overlay\n",
                         "patch/MW_Mod/a.class": b"class", "patch/particles/b.cfg": b"cfg",
                         "patch/cars/car.cfg": b"car", "patch/parts/part.cfg": b"part",
                         "patch/system/cfg/a.cfg": b"system"}
        self.files.update(self.overlays)
        for name, data in self.files.items():
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.commit = self.record()
        (self.repo / "patch/sl/local.cfg").write_bytes(b"untracked")
        self.build = self.root / "build"
        self.host = self.build / "Release/slrr_host.exe"
        self.host.parent.mkdir(parents=True)
        self.binary = b"MZ" + bytes(58) + bytes([64, 0, 0, 0]) + b"PE\0\0\x64\x86" + bytes(18) + b"\x0b\x02"
        self.host.write_bytes(self.binary)
        for notice in producer.BUILD_NOTICES:
            path = self.build / "_deps" / notice
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(notice.encode())
        self.output = self.root / "candidate"

    def git(self, *args):
        return subprocess.check_output(["git", "-C", self.repo, "-c", "core.autocrlf=false", *args])

    def record(self):
        self.git("add", "--all")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD").decode().strip()

    def package(self):
        producer.package(self.repo, self.build, self.output, "Feature/test", self.commit)

    def test_exact_candidate(self):
        self.package()
        metadata = dict(schemaVersion=1, commit=self.commit, target="windows-x86_64")
        self.assertEqual(json.loads((self.output / "release-body.json").read_bytes()),
                         dict(metadata, repository="EliasFD3S/openslrr", branch="Feature/test"))
        with ZipFile(self.output / "openslrr-windows-x86_64.zip") as archive:
            self.assertCountEqual(archive.namelist(), ["manifest.json", "slrr_host.exe", "LICENSE",
                                  "NOTICE.txt", *self.overlays])
            self.assertEqual(json.loads(archive.read("manifest.json")),
                             dict(metadata, version="0.0.0-branch." + self.commit))
            self.assertEqual(archive.read("slrr_host.exe"), self.binary)
            notice = archive.read("NOTICE.txt")
            for text in [b"compiler\n", b"engine notice\n", *(n.encode() for n in producer.BUILD_NOTICES)]:
                self.assertIn(text, notice)
            self.assertEqual(archive.read("LICENSE"), b"engine\n")
            for name, data in self.overlays.items():
                self.assertEqual(archive.read(name), data)
        with self.assertRaises(FileExistsError):
            self.package()

    def test_required_notice_attribution(self):
        self.package()
        with ZipFile(self.output / producer.ASSET) as archive:
            notice = archive.read("NOTICE.txt")
        # Expectations independent of the producer lists so omissions fail the test.
        for marker in (
            b"Font data copyright Google 2012", b"Copyright 2015 Google Inc. All Rights Reserved.",
            b"Copyright Dave Gandy 2016. All rights reserved.", b"Kenney.nl and SamBrishes",
            b"Version 2.0, January 2004", b"END OF TERMS AND CONDITIONS",
            b"SIL OPEN FONT LICENSE Version 1.1", b"TERMINATION",
            b"CC0 1.0 Universal", b"Limitations and Disclaimers",
            b"Copyright 2020-2024 Arm Limited",
        ):
            self.assertIn(marker, notice)
        for name in ("bimg-src/3rdparty/astc-encoder/LICENSE.txt", "bgfx-src/3rdparty/stb/LICENSE",
                     "bgfx-src/3rdparty/directx-headers/LICENSE", "bgfx-src/3rdparty/iconfontheaders/LICENSE",
                     "sdl3-src/src/video/yuv2rgb/LICENSE"):
            self.assertIn((name + "\n" + name + "\n\n").encode(), notice)

    def test_missing_required_notice_fails(self):
        name = "bimg-src/3rdparty/astc-encoder/LICENSE.txt"
        (self.build / "_deps" / name).unlink()
        with self.assertRaisesRegex(ValueError, "Missing or unexpected dependency notice"):
            self.package()
        self.assertFalse((self.output / producer.ASSET).exists())

    def test_patch_path_depth_limit(self):
        path = "patch/sl/" + "d/" * 29 + "a.cfg"
        self.assertEqual(len(path.split("/")), 32)
        self.assertEqual(producer.payload_name(path), path)
        with self.assertRaisesRegex(ValueError, "depth or byte limit"):
            producer.payload_name(path.replace("/a.cfg", "/d/a.cfg"))

    def test_patch_path_byte_limit(self):
        path = "patch/sl/" + ("a" * 254 + "/") * 3 + "b" * 246 + ".cfg"
        self.assertEqual(len(path.encode("utf-8")), 1024)
        self.assertEqual(producer.payload_name(path), path)
        with self.assertRaisesRegex(ValueError, "depth or byte limit"):
            producer.payload_name(path.replace(".cfg", "b.cfg"))

    def test_wrong_sha_or_dirty(self):
        with self.assertRaisesRegex(ValueError, "HEAD"):
            producer.check_source(self.repo, "0" * 40)
        (self.repo / "LICENSE").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "modified"):
            self.package()

    def test_wrong_architecture(self):
        self.host.write_bytes(self.binary.replace(b"\x64\x86", b"\x4c\x01"))
        with self.assertRaisesRegex(ValueError, "PE32"):
            self.package()

    def test_reject_unrecognized_overlay(self):
        (self.repo / "patch/sl/secret.pem").write_bytes(b"fake")
        self.commit = self.record()
        with self.assertRaisesRegex(ValueError, "file type"):
            self.package()
        self.assertFalse((self.output / producer.ASSET).exists())

    def test_reject_unknown_patch_root(self):
        with self.assertRaisesRegex(ValueError, "patch location"):
            producer.payload_name("patch/saves/config.cfg")

    def test_reject_symlink_blob_without_following_it(self):
        oid = self.git("rev-parse", "HEAD:LICENSE").decode().strip()
        self.git("update-index", "--add", "--cacheinfo", f"120000,{oid},patch/sl/link.cfg")
        tree = self.git("write-tree").decode().strip()
        with self.assertRaisesRegex(ValueError, "Not a regular"):
            list(producer.committed_files(self.repo, tree))


if __name__ == "__main__":
    unittest.main()
