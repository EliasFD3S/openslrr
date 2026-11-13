#!/usr/bin/env python3
"""Check that code references in the knowledge documents still resolve.

The knowledge documents cite the code with backticked anchors:

    `src/natives/native_registry.cpp`                  file must exist
    `src/natives/native_registry.cpp:descriptor_match` symbol must appear in it
    `SLRR_rev:docs/formats/tufa.md`                    external file (optional)
    `GAME:sl/Scripts/game/src/Mechanic.java:repair`    stock script source (optional)
    `PR3:docs/ROADMAP.md`                              file on the PR #3 branch (optional)

Symbol anchors are used instead of line numbers because line numbers rot with
every edit. A numeric anchor such as `src/main.cpp:42` is reported as an
error. External anchors are checked only when their checkout is supplied with
--external NAME=DIR; otherwise they are counted and skipped.

Standard library only. Exit status 0 when every checked reference resolves.

Usage:
    python scripts/check_knowledge_refs.py
    python scripts/check_knowledge_refs.py --external SLRR_rev=../SLRR_rev \
        --external GAME=data/sources --external PR3=../openslrr-pr3

GAME is a directory of stock script sources (the folder that contains
sl/Scripts, system/Scripts and parts/scripts). PR3 is a checkout of the
PR #3 branch (antomys/main).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_DOCS = ["docs/KNOWLEDGE_BASE.md", "docs/EXTERNAL_RESEARCH.md", "docs/MILESTONES.md"]
LOCAL_ROOTS = (
    "src/", "include/", "shaders/", "docs/", "patch/", "third_party/",
    "cmake/", "assets/", "scripts/",
)
LOCAL_FILES = ("CMakeLists.txt", "README.md", "AGENTS.md", ".gitignore", "LICENSE")
EXTERNAL_NAMES = ("SLRR_rev", "PR3", "GAME")

# `prefix:path` or `path` or `path:anchor`, inside single backticks.
TICK = re.compile(r"`([^`\s]+)`")
SYMBOL = re.compile(r"^[A-Za-z_~][A-Za-z0-9_:~.<>]*$")


def classify(token: str):
    """Return (kind, name, path, anchor) or None when the token is not a reference."""
    name = None
    body = token
    for ext in EXTERNAL_NAMES:
        if body.startswith(ext + ":"):
            name, body = ext, body[len(ext) + 1:]
            break
    path, _, anchor = body.partition(":")
    if "<" in path:
        return None  # A placeholder such as docs/<file>, not a reference.
    if name is None:
        if not (path.startswith(LOCAL_ROOTS) or path in LOCAL_FILES):
            return None
        # Directory references and globs are only checked for existence of the prefix.
        return ("local", None, path, anchor or None)
    return ("external", name, path, anchor or None)


def check_target(base: Path, path: str, anchor: str | None) -> str | None:
    if any(ch in path for ch in "*?"):
        matches = list(base.glob(path))
        return None if matches else f"no file matches {path}"
    target = base / path
    if not target.exists():
        return f"missing {path}"
    if anchor is None:
        return None
    if anchor.isdigit():
        return f"numeric anchor {path}:{anchor}; use a symbol anchor"
    if not SYMBOL.match(anchor):
        return f"malformed anchor {path}:{anchor}"
    if target.is_dir():
        return f"anchor on a directory {path}:{anchor}"
    text = target.read_text(encoding="utf-8", errors="replace")
    # The last component of a qualified name must appear as a whole word.
    leaf = re.split(r"::|\.", anchor)[-1]
    if not re.search(r"(?<![A-Za-z0-9_])" + re.escape(leaf) + r"(?![A-Za-z0-9_])", text):
        return f"symbol {anchor} not found in {path}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("docs", nargs="*", default=DEFAULT_DOCS,
                        help="documents to check (default: the knowledge documents)")
    parser.add_argument("--external", action="append", default=[], metavar="NAME=DIR",
                        help="checkout used for NAME: references, e.g. SLRR_rev=../SLRR_rev")
    args = parser.parse_args()

    externals: dict[str, Path] = {}
    for item in args.external:
        name, sep, directory = item.partition("=")
        if not sep or name not in EXTERNAL_NAMES:
            parser.error(f"--external expects one of {EXTERNAL_NAMES} as NAME=DIR")
        externals[name] = Path(directory).resolve()

    errors: list[str] = []
    checked = skipped = 0
    for doc in args.docs:
        doc_path = (REPO / doc) if not Path(doc).is_absolute() else Path(doc)
        if not doc_path.exists():
            errors.append(f"{doc}: document not found")
            continue
        in_fence = False
        for lineno, line in enumerate(doc_path.read_text(encoding="utf-8").splitlines(), 1):
            if line.lstrip().startswith("```"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for match in TICK.finditer(line):
                ref = classify(match.group(1))
                if ref is None:
                    continue
                kind, name, path, anchor = ref
                if kind == "external" and name not in externals:
                    skipped += 1
                    continue
                base = REPO if kind == "local" else externals[name]
                checked += 1
                problem = check_target(base, path, anchor)
                if problem:
                    errors.append(f"{doc}:{lineno}: {problem}")

    for error in errors:
        print(error)
    print(f"checked {checked} references, skipped {skipped} external, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
