"""Build the launcher candidate around the Release host from the same CI job (Python 3.11+)."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

REPOSITORY = "EliasFD3S/openslrr"
TARGET = "windows-x86_64"
ASSET = "openslrr-windows-x86_64.zip"
PATCH_ROOTS = {"cars", "parts", "sl", "system", "MW_Mod", "particles"}
PATCH_SUFFIXES = {".java", ".class", ".cfg"}


def validate_inputs(branch, commit):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected a full lowercase commit SHA.")
    if not branch or branch.startswith(("refs/", "-")):
        raise ValueError("Expected a branch name, not a ref or option.")
    subprocess.run(["git", "check-ref-format", "refs/heads/" + branch], check=True)


def check_tip(branch, commit):
    # check-ref-format rejects globs. Match the entire response, including ref case.
    ref = "refs/heads/" + branch
    response = subprocess.check_output([
        "git", "ls-remote", "--exit-code", "--heads", f"https://github.com/{REPOSITORY}.git", ref,
    ])
    if response.splitlines() != [f"{commit}\t{ref}".encode("utf-8")]:
        raise ValueError("The official branch tip does not match the reviewed commit.")


def check_source(checkout, commit):
    def git(*args):
        return subprocess.check_output(["git", "-C", checkout, *args])

    if git("rev-parse", "HEAD").decode().strip() != commit:
        raise ValueError("Checkout HEAD is not the reviewed SHA.")
    if git("status", "--porcelain", "--untracked-files=no"):
        raise ValueError("The engine checkout contains modified tracked files.")


def release_tag(branch, commit):
    return f"branch-{hashlib.sha256(branch.encode('utf-8')).hexdigest()}-{commit}"


def payload_name(path):
    if path in {"LICENSE", "NOTICE.txt", "third_party/slrrc/LICENSE"}:
        return path
    if path == "patch/README.md":
        return None  # installation instructions, not an overlay
    parts = path.split("/")
    if len(parts) < 3 or parts[:1] != ["patch"] or parts[1] not in PATCH_ROOTS:
        raise ValueError(f"Unsupported patch location: {path!r}")
    for part in parts:
        stem = part.split(".")[0].rstrip().upper()
        if (not re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9_. -]{0,254}", part)
                or part.endswith((".", " ")) or re.fullmatch(r"CON|PRN|NUL|AUX|COM\d|LPT\d", stem)):
            raise ValueError(f"Unsupported Windows path: {path!r}")
    if Path(path).suffix not in PATCH_SUFFIXES:
        raise ValueError(f"Unsupported patch file type: {path!r}")
    return path


def committed_files(checkout, commit):
    # Use blob IDs, not a filesystem walk: no untracked game data, symlink targets,
    # checkout filters, CRLF conversion or .gitattributes export substitutions.
    git = ["git", "-C", checkout]
    tree = subprocess.check_output(git + [
        "ls-tree", "-rlz", commit, "--", "LICENSE", "NOTICE.txt", "patch", "third_party/slrrc/LICENSE",
    ])
    entries = []
    paths = {}
    for row in tree.split(b"\0")[:-1]:
        meta, raw_name = row.split(b"\t", 1)
        mode, kind, oid, size = meta.split()
        path = raw_name.decode("utf-8")
        if mode != b"100644" or kind != b"blob":
            raise ValueError(f"Not a regular non-executable file: {path!r}")
        name = payload_name(path)
        if name is None:
            continue
        parts = name.split("/")
        for i in range(1, len(parts) + 1):
            prefix = "/".join(parts[:i])
            identity = (prefix, i == len(parts))
            if paths.setdefault(prefix.lower(), identity) != identity:
                raise ValueError(f"Conflicting Windows paths: {name!r}")
        entries.append((name, oid, int(size)))
    if "license" not in paths or "patch" not in paths:
        raise ValueError("Missing committed engine license or patch payload.")
    # Keep well inside the launcher's expanded size/entry limits before reading blobs.
    if len(entries) + 3 > 4096 or sum(size for _, _, size in entries) > 128 * 1024**2:
        raise ValueError("Committed payload exceeds package limits.")
    blobs = io.BytesIO(subprocess.check_output(git + ["cat-file", "--batch"],
                                              input=b"".join(oid + b"\n" for _, oid, _ in entries)))
    for name, oid, size in entries:
        if blobs.readline().split() != [oid, b"blob", str(size).encode()]:
            raise ValueError("Git blob metadata mismatch.")
        data = blobs.read(size)
        if blobs.read(1) != b"\n":
            raise ValueError("Truncated git blob.")
        yield name, data


def release_binary(build):
    host = build / "Release" / "slrr_host.exe"
    if host.stat().st_size > 128 * 1024**2:
        raise ValueError("Host exceeds launcher size limit.")
    data = host.read_bytes()
    offset = int.from_bytes(data[60:64], "little")
    if (not data.startswith(b"MZ") or offset < 64 or data[offset:offset + 6] != b"PE\0\0\x64\x86"
            or data[offset + 24:offset + 26] != b"\x0b\x02"):
        raise ValueError("Expected an x86_64 PE32+ executable.")
    return data


# Notice paths in the clean build's pinned dependency sources (not SDK or game files).
BUILD_NOTICES = (
    "bgfx-src/LICENSE", "bgfx-src/3rdparty/dear-imgui/LICENSE.txt", "bimg-src/LICENSE",
    "bx-src/LICENSE", "bx-src/include/tinystl/LICENSE", "sdl3-src/LICENSE.txt",
    "sdl3-src/src/hidapi/LICENSE-bsd.txt", "sdl3_mixer-src/LICENSE.txt",
    "sdl3_mixer-src/src/dr_libs/LICENSE",
)


def dependency_notices(build):
    root = (build / "_deps").resolve()
    sections = []
    for name in BUILD_NOTICES:
        path = root / name
        if path.resolve() != path or not 0 < path.stat().st_size < 1024 * 1024:
            raise ValueError(f"Unexpected dependency notice: {name}")
        sections.extend([name.encode() + b"\n", path.read_bytes(), b"\n\n"])
    return b"".join(sections)


def package(checkout, build, output, branch, commit):
    check_source(checkout, commit)
    manifest = dict(schemaVersion=1, version="0.0.0-branch." + commit, commit=commit, target=TARGET)
    metadata = dict(schemaVersion=1, repository=REPOSITORY, branch=branch, commit=commit, target=TARGET)
    host = release_binary(build)
    output.mkdir(parents=True)  # never mix with or overwrite a previous candidate
    with tempfile.TemporaryDirectory(dir=output) as tmp:
        staged = Path(tmp) / ASSET
        with ZipFile(staged, "x", ZIP_DEFLATED) as archive:
            archive.writestr("manifest.json", json.dumps(manifest) + "\n")
            archive.writestr("slrr_host.exe", host)
            notices = []
            for name, data in committed_files(checkout, commit):
                if name in {"NOTICE.txt", "third_party/slrrc/LICENSE"}:
                    notices.extend([name.encode() + b"\n", data, b"\n\n"])
                else:
                    archive.writestr(name, data)
            notices.append(dependency_notices(build))
            archive.writestr("NOTICE.txt", b"".join(notices))
        with staged.open("rb") as f:
            digest = hashlib.file_digest(f, "sha256").hexdigest()
        (output / "release-body.json").write_text(json.dumps(metadata) + "\n", encoding="utf-8")
        staged.rename(output / ASSET)
    print(f"Tag: {release_tag(branch, commit)}\n{ASSET}: {(output / ASSET).stat().st_size} bytes, sha256:{digest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("check-tip")
    commands.add_parser("check-source").add_argument("--checkout", required=True, type=Path)
    pack = commands.add_parser("package")
    for arg in ("--checkout", "--build", "--output"):
        pack.add_argument(arg, required=True, type=Path)
    args = parser.parse_args()
    branch, commit = os.environ["ENGINE_BRANCH"], os.environ["ENGINE_COMMIT"]
    validate_inputs(branch, commit)
    if args.command == "check-tip":
        check_tip(branch, commit)
    elif args.command == "check-source":
        check_source(args.checkout, commit)
    else:
        package(args.checkout, args.build, args.output, branch, commit)


if __name__ == "__main__":
    main()
