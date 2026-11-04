# OpenSLRR

C++ host for **Street Legal Racing: Redline**. It loads the game's TUFA classes, runs the TREE programs, and supplies the natives, resources, rendering, and audio those scripts call.

The validated scope covers startup, menus, dialogs, and the garage, including assembling a car from its CFG files. Driving, the world, and traffic are still ahead. The game data is not in this repository.

Validation uses **SLRR 2.2.1 MWM**. Win32 is the reference configuration: the scripts and the 32-bit AVI codecs expect it. The bgfx backend is built as C++20.

## Status

| Area | In place |
| --- | --- |
| Scripts | TUFA classes, TREE programs, Java and native calls, inheritance, `switch` |
| Resources | RPK packs and cross-pack references, SCX geometry, DDS/PTX textures, CFG properties |
| Rendering | bgfx/D3D11, textured garage, orbit camera, scene instances, skydome |
| Car | Built from an RPK/CFG, stock parts, slots, and compatibility aliases. Reference: Baiern, 76 parts |
| Catalog | Categories across RPKs, classes, prices, parts, and 3D previews on the original pages |
| Materials | Paint from the CFG texture, multiple UVs, secondary layer, static envmap chosen by the scripts |
| Interface | Menus, career dialogs, Club Info, track selector, text fields, hover, DAY/TIME |
| Paint | Five 3D paint cans, colors from the scripts, scrolling, separate inventory cameras |
| Audio | UI effects, `SfxRef`, voices with volume, pitch, looping, and stop |
| FMV | AVI inside the renderer, audio track of the Activision/Invictus intros |

In the test data, `Prime.avi` has no audio track.

**Still to build:** integrated driving and physics, a real suspension pose, full transparency and lighting, dynamic reflections, the world, and traffic. Some natives are still stubs. Catalog display is tested; buying, interactive install, and saving that cycle are not. Pacejka is a probe (`--probe-pacejka`), separate from the driving loop. Multiplayer is out of scope.

## Sources

The installed TUFA scripts and assets remain the source of classes, parts, and their parameters. The host reimplements those contracts in C++. The repositories below were used as specifications:

| Reference | Role for this host |
| --- | --- |
| [SLRR-JVM-decompiler](https://github.com/nedobylskiy/SLRR-JVM-decompiler) | TUFA and `OP_FLAGS` spec, carried into `src/jvm/tufa.cpp` |
| [SLRR-JVM-compiler](https://github.com/nedobylskiy/SLRR-JVM-compiler) | TUFA writer, used when regenerating classes |
| [SLRR-JVM](https://github.com/nedobylskiy/SLRR-JVM) | Source dialect and natives, behavior reference |
| [SLRR_rev](https://github.com/nedobylskiy/SLRR_rev) | RPK/SCX formats, CFG, FindFile |
| [SLRR_rev `codex/physics-reverse-port` @ c65675f](https://github.com/nedobylskiy/SLRR_rev/tree/c65675f) | SCX layers, paint, envmap. Contracts in [REFERENCE_PORT.md](REFERENCE_PORT.md) |
| [SLRR_rev `codex/pacejka-research`](https://github.com/nedobylskiy/SLRR_rev) | SL2 Pacejka leaf, exposed by `--probe-pacejka` |
| [io_invo_scx](https://github.com/nonamesex/io_invo_scx) | SCX mesh layout |
| [bgfx](https://github.com/bkaradzic/bgfx) | D3D11 rendering |

Other SLRR_rev notes (WheelRef, wear, damage) stay as reading material. They do not drive the game yet. [QUARRY.md](QUARRY.md) describes the local workspace (`repos/`, `sources/`, `game/`) where those references were read.

## Requirements

- Windows, Visual Studio 2022, the Windows SDK, and CMake.
- Git, to fetch bgfx.
- A game install: classes, RPKs, models, textures, and sounds. It must contain `system/Scripts/lang/System.class`.

Run the commands from the root of this repository. In the development workspace, that folder is named `host/`.

## Build

```powershell
git clone https://github.com/EliasFD3S/openslrr.git
cd openslrr

git clone https://github.com/bkaradzic/bgfx.cmake.git third_party/bgfx.cmake
git -C third_party/bgfx.cmake checkout 5ea74ee66662daef4089695846d6322da0dde94e
```

The default game folder is `../game`. Pass another location with `-GameRoot` or `--root`.

```powershell
cmake -S . -B build-bgfx -G "Visual Studio 17 2022" -A Win32 `
  -DSLRR_WITH_LIBBGFX=ON -DSLRR_BUILD_TESTS=ON -DSLRR_STAGE_BUILD=ON

.\validate-and-promote.ps1

# Game installed elsewhere:
.\validate-and-promote.ps1 -GameRoot 'D:\Jeux\SLRR'
```

`validate-and-promote.ps1` builds into `build-bgfx/candidate/`, runs the tests against the original assets, archives sources, SHA-256 hashes, the test log, and the executables under `../checkpoints/`, then replaces `build-bgfx/Release/slrr_host.exe` only when the tests pass. The previous executable stays in that backup. Close the game before this step: a running executable blocks the replacement. A build with `SLRR_STAGE_BUILD=ON` updates only the candidate.

The archives contain neither the game data nor `third_party/`.

## Run

```powershell
# Intros, then menus.
.\build-bgfx\Release\slrr_host.exe --game --render bgfx --root ..\game

# Garage, skipping career creation.
.\build-bgfx\Release\slrr_host.exe --game --auto-new --render bgfx --root ..\game

# Baiern assembled from its scripts and CFG.
.\build-bgfx\Release\slrr_host.exe --game --spawn-car cars/racers/Baiern.rpk:0x6 --render bgfx --root ..\game

# Data load check, without an interactive session.
.\build-bgfx\Release\slrr_host.exe --probe --root ..\game

# Pacejka probe.
.\build-bgfx\Release\slrr_host.exe --probe-pacejka
```

`$env:SLRR_GAME_ROOT` also sets the game folder. `--root` takes priority. `slrr_host.exe --help` lists the remaining options.

In the garage: right-click and drag to orbit, arrow keys and the wheel for the camera. Escape skips the intros. Closing the window quits.

`--spawn-car PACK:LOCAL_ID` opens a development session without the career dialog. The pose is static.

## Tests

```powershell
cmake --build build-bgfx --config Release `
  --target host_runtime_test ui_state_test scene_instances_test part_config_test --parallel
ctest --test-dir build-bgfx -C Release --output-on-failure
```

With the reference data, CTest registers **16 tests**:

- simultaneous effects, `money.wav`, voice loops, and stopping voices;
- connection and playback of the intro audio track;
- Tune and script `switch` branches;
- the DAY/TIME HUD, including after it is recreated;
- career dialogs, labels, and scrolling;
- track selector descriptions, without stacking leftovers;
- hover, enabled and disabled buttons, focus;
- five gray paint cans, the next red page, and their removal on close;
- transforms, isolated inventory scenes, skydome;
- CFG, slots, aliases, and the stock assembly run by the scripts;
- catalog parts, classes, prices, and previews;
- SCX layers, UV channels, and `Part.setTexture` propagation.

`SLRR_TEST_GAME_ROOT` must point at the game for the integration tests. Without that data, only the standalone tests are built. Promotion refuses a partial suite.

These tests do not cover a full career load cycle or a driving session. The audio tests check that voices are created and report playback state. Positional effects go through the 2D mixer.

## Layout

```text
include/slrr/     Interfaces
src/jvm/          TUFA parser, TREE interpreter, call resolution
src/natives/      Natives exposed to scripts
src/resources/    RPK, SCX, textures, CFG, instances, assembly
src/render/       Backends and fonts
src/ui/           Widgets, focus, hover, clock
src/platform/     Input, audio, camera, FMV
src/boot/         Startup and main loop
src/physics/      Pacejka probe
shaders/          bgfx shaders
tests/            Standalone and integration tests
third_party/      Local dependencies, ignored by Git
assets/           Cursors and icon extracted for the host
```

`game/`, `game2/`, `sources/`, `sources2/`, `repos/`, and `docs/` live beside this repository. They are not versioned here.
