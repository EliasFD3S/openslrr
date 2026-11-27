# Documentation index

Start with [AGENTS.md](../AGENTS.md) for the working rules, then
[core/ENGINE_CONTRACT.md](core/ENGINE_CONTRACT.md) before changing the engine.
Building and running are in the [README](../README.md).

Each subsystem folder holds its reference documents (contracts, formats,
plans, audits) and a notes file of verified per-issue findings: what was
wrong, the fix, and how it was checked. New issue notes go in the notes file
of their subsystem, not in the knowledge base.

## Core

| File | Contents |
| --- | --- |
| [core/ENGINE_CONTRACT.md](core/ENGINE_CONTRACT.md) | Authoritative engine contract: compatibility rules, repository map, game loop, VM, threads, lifetime, resources, scene, cars, physics, rendering overview, UI, audio, saves, working method |
| [core/KNOWLEDGE_BASE.md](core/KNOWLEDGE_BASE.md) | Cross-cutting knowledge: evidence rules, subsystem map, data flows, threads and the worker pool, invariants, constants, documentation drift, risk register, open issues, self-check |

## Subsystems

| Folder | Reference | Issue notes |
| --- | --- | --- |
| [scripting/](scripting/) — VM, scripts, natives | [runtime.md](scripting/runtime.md) (fibers, GameType events, dialogs, sections), [compiler.md](scripting/compiler.md) (`System.compileAll`, shipped patch classes), [vm-gc.md](scripting/vm-gc.md) (garbage collection), [natives.md](scripting/natives.md) (native audit, no-ops, host utility natives), [mod-api.md](scripting/mod-api.md) (java.openslrr mod API: car state, camera control, camera providers), [mod-api-keys-settings.md](scripting/mod-api-keys-settings.md) (mod settings, Options → Mods rows, rebindable mod keys) | [notes.md](scripting/notes.md) |
| [rendering/](rendering/) | [contract.md](rendering/contract.md) (reflections, mesh ownership, shadows, sky and lights, light sources, debug overrides), [roadmap.md](rendering/roadmap.md) (overhaul #201) | [lighting.md](rendering/lighting.md), [shadows-ao.md](rendering/shadows-ao.md), [performance.md](rendering/performance.md), [materials.md](rendering/materials.md), [backend.md](rendering/backend.md) |
| [vehicles/](vehicles/) — cars, physics, driver | [driving-model.md](vehicles/driving-model.md) (tyre/drive model, nitrous), [physics-tick.md](vehicles/physics-tick.md) (physics clock, throttle, engine/dyno, stock calibration), [dynamics-plan.md](vehicles/dynamics-plan.md) (PR #48 migration plan) | [notes.md](vehicles/notes.md) |
| [world/](world/) — maps, pedestrians, traffic, races | [traffic-races.md](world/traffic-races.md) (traffic and race audit #197) | [notes.md](world/notes.md) |
| [ui/](ui/) — OSD, viewports, cameras, input, garage paint | [osd-v2.md](ui/osd-v2.md) (OSD v2 contract, osdx package, Options screen); see [scripting/runtime.md](scripting/runtime.md) for sections and dialogs | [notes.md](ui/notes.md) |
| [saves/](saves/) — saves, files, options | [json-schema.md](saves/json-schema.md) | [notes.md](saves/notes.md) |
| [resources/](resources/) — SCX/RPK/RPAK, audio, video | knowledge base §5.4 | [notes.md](resources/notes.md) |
| [platform/](platform/) — build system | knowledge base §3, [AGENTS.md](../AGENTS.md#build-speed-and-its-edge-cases) | [build.md](platform/build.md) |

## Research and planning

| File | Contents |
| --- | --- |
| [research/EXTERNAL_RESEARCH.md](research/EXTERNAL_RESEARCH.md) | Formats, mechanics and lessons from SLRR_rev and draft PR #3; how to migrate mechanics from SLRR_rev |
| [planning/MILESTONES.md](planning/MILESTONES.md) | Proposed roadmap |

## Elsewhere in the repository

- [patch/README.md](../patch/README.md): Java classes shipped with the host.
- `assets/validation/`: screenshots, logs and records from validation runs.

## Checking references

`python scripts/check_knowledge_refs.py` checks every backticked code
reference (`path` or `path:symbol`) in all files under `docs/`. Run it after
changing code the documentation cites.
