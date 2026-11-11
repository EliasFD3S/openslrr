# AGENTS.md — Open SLRR engine

## Purpose and source of truth

Open SLRR is a Windows x64, C++20 reimplementation of the SLRR 2.2.1 engine. It executes the original recovered scripts and loads original game data. The implementation must be judged by observed behavior and source evidence, not by native-registration counts alone.

- Editable engine source: `src/`, `include/`, `shaders/`, `cmake/`, `scripts/`, `CMakeLists.txt`.
- Reference MWM Java: `../open-SL/sources/`.
- Reference vanilla Java: `../open-SL/sources2/`.
- Reference game data: `../open-SL/game/` (MWM) and `../open-SL/game2/` (vanilla).
- Reference trees and game data are read-only during ordinary engine development.
- Current design and known behavioral gaps: `README.md`, `NATIVE_AUDIT.md`, `SCRIPT_RUNTIME.md`, `VM_GC.md`, and `HOST_NATIVE_UTILITIES.md`.
- Architecture, review findings, and execution plan: `docs/ARCHITECTURE.md`, `docs/SOURCE_REVIEW.md`, and `docs/ROADMAP.md`.

## Engineering rules

1. Preserve stock script/data compatibility. Do not patch recovered Java, compiled classes, saves, or game assets to hide a host defect.
2. Validate every size/offset calculation before pointer arithmetic or allocation, including integer overflow and truncated inputs. Resource files and script values are untrusted inputs.
3. Every native pointer retained across a cooperative yield must be protected by `Jvm::RootScope` or a registered root provider. GC runs only at host safe points; do not invoke it reentrantly.
4. VM execution and fibers remain on the host OS thread. Do not introduce concurrent Java execution. Preserve scheduler cancellation, stack unwinding, and modal-pump behavior.
5. Keep ownership explicit. Resource destruction must release associated native handles, notifications, callbacks, and GC roots.
6. Physics remains fixed-step and numerically stable across render rates. Avoid new frame-rate-dependent integration.
7. Do not silently turn an unsupported native into a no-op. Document partial behavior and update `NATIVE_AUDIT.md` and generated coverage when native registrations change.
8. Keep changes scoped. Do not reformat entire files, rename broad APIs, change dependencies, promote executables, or commit unless the task explicitly authorizes it.
9. Never run `validate-and-promote.ps1` in a shared development worktree; it archives and replaces a Release executable. Ordinary validation uses configure/build and non-mutating probes only.

## Build and validation

The pinned renderer is bgfx.cmake commit `5ea74ee66662daef4089695846d6322da0dde94e`. Setup is project-local; global package installation is not required.

```powershell
# From the repository or an agent worktree; idempotent bootstrap.
./scripts/Initialize-SlrrWorkspace.ps1 -Build

# Subsequent builds, after bootstrap.
./tools/cmake-3.31.8-windows-x86_64/bin/cmake.exe --build build-bgfx-x64 --config Release --target slrr_host --parallel

# In a worktree, use the main checkout's shared local CMake if tools/ is absent.
```

Use a separate build directory for every worktree. Do not run concurrent builds in one directory. There is currently no committed automated test target; a successful compile is only a baseline, not behavioral verification. Add focused regression tests with fixes, especially for parsers, VM semantics, scheduler/GC behavior, and deterministic physics. Record exact commands and results in the agent report.

Headless probes documented in the Markdown files are useful only when the current binary supports them. Check `slrr_host.exe --help` before using a probe. Interactive driving, rendering, audio, and physical gamepad behavior require explicit interactive validation; do not claim it from headless checks.

## Multi-agent ownership

The coordinator assigns one owner per file. Default subsystem boundaries:

| Role | Default writable scope |
| --- | --- |
| VM/runtime | `src/jvm/`, `src/boot/`, related runtime headers |
| Resources/natives | `src/resources/`, `src/natives/`, related resource headers |
| Render/UI/platform | `src/render/`, `src/ui/`, `src/platform/`, `shaders/` |
| Physics | `src/physics/`, physics headers and explicitly assigned integration call sites |
| Java contract research | `../open-SL/sources*/` read-only; report only |
| Tests/tooling | `tests/`, `scripts/`, `cmake/`; CMake/top-level headers only when explicitly assigned |
| Coordinator | Integration, shared headers, CMake, docs, final validation |

Cross-boundary edits must be listed in the task packet and assigned to only one agent. Do not let two agents edit `part_runtime.cpp`, shared headers, `CMakeLists.txt`, or documentation concurrently.

## Harness workflow

1. Start from a clean `main`, record the base commit, and write a task packet: objective, owned files, allowed dependencies, acceptance checks, and expected report.
2. Use `scripts/New-SlrrAgentWorktree.ps1 -Name <role-task>` for each concurrent coding task. Start the Harness subagent with that worktree as `cwd`.
3. Research/review agents can share the main checkout read-only. Coding agents use separate worktrees and build directories.
4. Agents must not switch branches in another agent's worktree, alter reference data, or integrate their own changes into `main`.
5. The coordinator reviews diffs, resolves cross-subsystem changes, runs the combined build/tests, and updates documentation. Integration order is normally parser/VM fixes, resources/natives, physics, then rendering/UI.
6. Stop obsolete agents and collect background jobs before reporting completion.

See `docs/MULTIAGENT.md` for task packets, report format, and integration gates.
