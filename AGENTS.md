# Agent working guide

Before changing the engine, read [ENGINE_CONTRACT.md](docs/core/ENGINE_CONTRACT.md).
This guide describes the SLRR contract, subsystem responsibilities, and
compatibility pitfalls. Then read the specialized documentation for your task:

- VM, threads, events, and dialogs: [scripting/runtime.md](docs/scripting/runtime.md).
- Host threads, the worker pool and how to add parallel work:
  [KNOWLEDGE_BASE.md §5.10](docs/core/KNOWLEDGE_BASE.md#510-threads-and-the-worker-pool)
  and [ENGINE_CONTRACT.md §6](docs/core/ENGINE_CONTRACT.md#host-threads-and-the-worker-pool).
- Object lifetime: [scripting/vm-gc.md](docs/scripting/vm-gc.md).
- Script compilation: [scripting/compiler.md](docs/scripting/compiler.md).
- Bindings, no-ops, and limitations: [scripting/natives.md](docs/scripting/natives.md).
- Building and running: [README.md](README.md).
- Verified code map, known doc drift, open risks and issue entry points:
  [KNOWLEDGE_BASE.md](docs/core/KNOWLEDGE_BASE.md).
- Formats, mechanics and lessons from SLRR_rev and draft PR #3:
  [EXTERNAL_RESEARCH.md](docs/research/EXTERNAL_RESEARCH.md).
- Proposed roadmap: [MILESTONES.md](docs/planning/MILESTONES.md).
- Everything else (rendering, vehicles, world, UI, saves, resources, build)
  is grouped by subsystem; start from the [documentation index](docs/README.md).

SLRR_rev (https://github.com/nedobylskiy/SLRR_rev) implements most game
mechanics. Before implementing a missing native or mechanic, check whether
SLRR_rev already has it and migrate it, following
[EXTERNAL_RESEARCH.md §7](docs/research/EXTERNAL_RESEARCH.md#7-migrating-mechanics-from-slrr_rev).

After changing code that the documentation cites, run
`python scripts/check_knowledge_refs.py`.

Write an issue's verified notes (cause, fix, validation) in its subsystem's
notes file under `docs/<subsystem>/`, not in the knowledge base. The
knowledge base keeps only cross-cutting knowledge; its §14 lists where each
subsystem's notes live.

Check the active code: some audits and comments describe earlier implementations.
Preserve changes already present in the working tree. Fix general primitives
rather than adding VM exceptions for a specific car or screen. Java changes
must remain within the scope authorized by the user and must be included in
the distributed patch when needed.

Update the relevant documentation whenever a contract or responsibility changes.
Report compilation and in-game validation separately; neither result alone
establishes complete compatibility.

## Branches and pull requests

- Name branches `{type}/{issue_number}-short-desc`, where `type` is `feature`,
  `bug`, `hotfix`, or `spike`. The issue number is required. For example:
  `bug/44-remaining-limitations-fix`.
- Start the PR title with `#<issue_number>` and make its wording match the
  branch's short description, for example: `#44 Remaining limitations fix`.
- Keep the PR description concise and direct. Include a `Not fixed / limitations`
  section listing remaining work so a crawler can add it to issue #44; write
  `None` when nothing remains. Do not omit known gaps.
- For UI changes, include labelled **Before** and **After** screenshots in the
  PR description.
- Always request review from someone other than the PR author. PRs created by
  `antomys` must request `EliasFD3S`; PRs created by `EliasFD3S` must request
  `antomys`.

## Build speed and its edge cases

Compile options are described in [README.md](README.md#faster-builds). Known
limits and edge cases:

- Every target builds with `/MP`. With several builds at once the machine can
  run out of memory or cores; lower it with `-DSLRR_COMPILE_JOBS=<n>`.
- `slrr_host` uses a precompiled header of standard headers. Do not put project
  headers, macros such as `NOMINMAX`, or Windows headers in it, and a source
  file must still include what it uses.
- The shared `shaderc` (`SLRR_TOOLS_CACHE_DIR`) is keyed by the git revisions
  and uncommitted changes of bgfx.cmake, bgfx, bimg and bx, the compiler and the
  toolset. Changing the toolset's version or the sources builds a new copy;
  old `shaderc-<key>` folders are never deleted automatically, so remove them
  by hand when the disk fills.
- If those sources are not git checkouts, `shaderc` is built once in
  `<build>/shaderc-local` instead and is not shared. It is not rebuilt when the
  sources change: delete that folder after changing them.
- The first build for a new key compiles `shaderc` (about 13 minutes) while the
  other builds wait on its lock for up to 2 hours. A build killed midway leaves
  no half-written copy; the next build retries.
- `shaderc` is always built in Release, whatever configuration the game uses.
- Shader outputs are not regenerated when only `shaderc` changes; delete
  `<build>/generated/shaders` after switching to another `shaderc`.
- `tests/test_cmake_dependency_paths.py` needs Windows Python (it uses
  `winreg`); the agent environment has none, so it cannot be run there.
