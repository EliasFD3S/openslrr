<p align="center">
  <img src="assets/branding/open-slrr.png" alt="Open SLRR" width="600">
</p>

<p align="center">
  <strong>Rebuild the engine. Return to the garage.</strong><br>
  A C++ reimplementation of the Street Legal Racing: Redline engine.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/status-in_development-8b5cf6" alt="In development">
  <img src="https://img.shields.io/badge/platform-Windows_x86-0078d4" alt="Windows x86">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599c" alt="C++20">
  <img src="https://img.shields.io/badge/reference-SLRR_2.2.1_MWM-c0c0c0" alt="SLRR 2.2.1 MWM">
</p>

<p align="center">
  <a href="#progress">Progress</a> · <a href="#build">Build</a> · <a href="#run">Run</a> · <a href="#sources-and-credits">Sources and credits</a>
</p>

---

Open SLRR brings the game's scripts, cars, and resources into a new engine. The project reimplements resource loading, script execution, rendering, the interface, and the native functions those scripts use.

**The garage is coming to life, but the game is not fully playable yet.** Menus, the car showroom, parts previews, and car assembly are in place. In the garage you can name the part under the cursor, hide a group of parts, open panels, and remove or fit a part together with what is attached to it. Driving and a complete career remain to be built.

Original game data is required and is not included. The reference version is **SLRR 2.2.1 MWM**; compatibility with other versions and mods is not guaranteed. This is an independent project, unaffiliated with the original game's developers or publishers.


## Progress

These milestones describe existing functionality, rather than a percentage of the game completed.

| Area | Current state |
| --- | --- |
| Scripts | ✅ TUFA classes and TREE programs, inheritance, Java and native calls |
| Resources | ✅ RPK packs, SCX models, DDS/PTX textures, and CFG configurations |
| Garage and rendering | ✅ bgfx/D3D11, textures, script camera, instances, and sky |
| Interface | ✅ Menus, career dialogs, catalog, car showroom, track selection, and paint inventory |
| Cars | ✅ Assembly from scripts and CFG files; reference Baiern with 76 parts. The showroom car turns over the film. Car information shows the powertrain layout, gear ratios, and dyno scales |
| Workshop | ✅ Point at a part to read its name, hide the body or the engine, open doors and adjustment dialogs, and remove or fit a part together with the parts attached to it. An inventory part turns while you point at it |
| Physics | ✅ Chassis, springs, dampers, and wheel contacts, verified with Baiern and Duhen. A released car settles on the garage floor. A parked car holds its standing height |
| Audio and video | ✅ Effects, voices, loops, and intros with their audio tracks where available |
| Driving | ⏳ Tyre slip, engine, clutch, transmission, and controls still need to be connected |
| World and career | ⏳ City, traffic, saves, and the complete purchase and save cycle |

**Next major milestones**

- [ ] Complete the simulation and controls needed to drive.
- [ ] Extend wall collisions, suspension constraints, and deformation.
- [ ] Finish buying a part and saving the car.
- [ ] Complete transparency rendering.
- [ ] Bring the city and traffic to life.

Some native functions are still placeholders. You can browse the catalog and work on the car in the garage. Buying a part and saving the car have not been validated yet. Multiplayer is outside the current scope.

## Build

### Requirements

- Windows and **Visual Studio 2022**, with the **Desktop development with C++** workload and the Windows SDK.
- **CMake 3.21 or later** and **Git**, available in your terminal.
- An installation of **SLRR 2.2.1 MWM**, containing `system/Scripts/lang/System.class`.
- An Internet connection while preparing and initially configuring dependencies.

The reference configuration is **Win32**, including on 64-bit Windows: the original scripts and AVI codecs expect a 32-bit environment.

### Get the project

In PowerShell:

```powershell
git clone https://github.com/EliasFD3S/openslrr.git
cd openslrr

git clone https://github.com/bkaradzic/bgfx.cmake.git third_party/bgfx.cmake
git -C third_party/bgfx.cmake checkout 5ea74ee66662daef4089695846d6322da0dde94e
```

Run the following commands from this folder, which contains `CMakeLists.txt`. In the local development workspace, it is named `host/`.

### Configure and build

Replace the game path with your installation location:

```powershell
cmake -S . -B build-bgfx -G "Visual Studio 17 2022" -A Win32 `
  -DSLRR_WITH_LIBBGFX=ON `
  -DSLRR_BUILD_TESTS=ON `
  -DSLRR_STAGE_BUILD=OFF `
  -DSLRR_TEST_GAME_ROOT="D:/Games/SLRR"

cmake --build build-bgfx --config Release --parallel
```

The executable is generated at `build-bgfx/Release/slrr_host.exe`. The first build includes bgfx and its shader tools, so it may take a while.

## Run

```powershell
# Intros, then menus.
.\build-bgfx\Release\slrr_host.exe --game --render bgfx --root "D:\Games\SLRR"

# Open the garage, skipping career creation.
.\build-bgfx\Release\slrr_host.exe --game --auto-new --render bgfx --root "D:\Games\SLRR"

# Baiern assembled from its scripts and configurations.
.\build-bgfx\Release\slrr_host.exe --game --spawn-car cars/racers/Baiern.rpk:0x6 --render bgfx --root "D:\Games\SLRR"
```

In the garage, **right-click and drag**, the **arrow keys**, and the **scroll wheel** control the camera. **Escape** skips the intros. Close the window to quit. The car rests on its wheels, but driving controls are not connected yet. In the showroom the view turns around the car on its own.

You can also set the game path with `$env:SLRR_GAME_ROOT`. The `--root` option takes priority; without either setting, the default folder is `../game`. Run `slrr_host.exe --help` for all options.

## Verify the build

After building:

```powershell
ctest --test-dir build-bgfx -C Release --output-on-failure
```

With the reference game data configured, the suite registers **18 tests**, covering scripts, the interface, audio, scenes, parts, and suspension contacts. Without game data, only standalone tests are registered. These checks do not yet cover a driving session or a complete career.

## Sources and credits

The project draws on community research to understand SLRR's formats and behavior. Your installation's scripts and resources supply the classes, parts, and their parameters; the C++ engine reimplements the functions they expect.

| Source | Contribution |
| --- | --- |
| [SLRR-JVM-decompiler](https://github.com/nedobylskiy/SLRR-JVM-decompiler) | TUFA class format and script instructions |
| [SLRR-JVM-compiler](https://github.com/nedobylskiy/SLRR-JVM-compiler) | Writing and regenerating TUFA classes |
| [SLRR-JVM](https://github.com/nedobylskiy/SLRR-JVM) | Reference for the scripting language and native functions |
| [SLRR_rev](https://github.com/nedobylskiy/SLRR_rev) | RPK/SCX formats, configurations, materials, and physics research |
| [io_invo_scx](https://github.com/nonamesex/io_invo_scx) | SCX mesh structure |
| [bgfx](https://github.com/bkaradzic/bgfx) and [bgfx.cmake](https://github.com/bkaradzic/bgfx.cmake) | Graphics rendering and CMake build integration |

Physics research is being integrated gradually: listing a reference does not mean the entire original simulation has already been reproduced. Thanks to the authors of these tools and research, and to the original game's creators.

## Contributing

When reporting a problem, include your game version, launch command, expected behavior, and actual result. Attach console output and, if useful, a screenshot. If you use a mod, name it and check whether the issue also occurs with the reference game data.

To explore the code: `src/jvm/` runs scripts, `src/resources/` loads data, `src/natives/` connects scripts to the engine, and `src/render/`, `src/ui/`, `src/platform/`, and `src/physics/` contain the main systems. Tests live in `tests/`.
