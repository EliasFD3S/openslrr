# Garage paint validation (#64)

## Revisions and captures

- **Before:** `d1c719d` (main); Windows x64 Release executable SHA-256 `6850405d9dc14b38fda521dd03f924f04d1b43635dc7e9795fcbb555e5eb1c0a`.
- **After:** engine source `dbe4ff15d66a77191d3c96aa0620a9f7eeb6d8c2`; Windows x64 Release executable SHA-256 `a44a9cb1112a16addb22b1e35ebb02b9c53ebe1a71599afbef6d0e2ce9a15a7a`.
- Actual Direct3D 11 application captures, 1280×720 client, same parked stock Baiern, garage, camera, daytime and paint-tool state. The game clock advances normally. No mockups or Java changes.
- Validation used an isolated copy of the installed MWM game data and saves. Original reference assets/saves were not edited.

### Before — d1c719d

Select whole-part paint, the first (white) can, click the driver's door, then move the pointer off the car. The door stays red. The cans bob out of their cells.

![Before: paint click leaves the door red](<before.png>)

### After — dbe4ff1

Repeat the same actions. Only the clicked door becomes white; the remaining panels retain their red paint. The cans stay visible at their script poses.

![After: white clicked door and stationary paint cans](<after.png>)

## Compilation and automated checks

Windows MSVC 2022 x64 Release host build passed. The final executable was used for the final comparison and reload captures.

All seven Windows regression suites passed; complete output is in [windows-regressions.txt](<windows-regressions.txt>):

| Suite | Result |
| --- | --- |
| `paint_texture` | 8 passed |
| `save_io` | 13 passed, including runtime PNG blobs, stock side-image loading, file-backed decal aliases and resource ownership |
| `save_persistence` | 10 passed |
| `car_select_view` | 3 passed |
| `driver_selector` | Passed, including independent file-texture handles and replacement isolation |
| `gameref_alias` | 3 passed |
| `trigger_markers` | Passed; world marker movement/sensors retained, local inventory paint cans stationary and opaque |

Reproduction from a built checkout with local game assets available as `game`:

```powershell
foreach ($suite in 'paint_texture','save_io','save_persistence','car_select_view','driver_selector','gameref_alias','trigger_markers') {
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_save_io_tests.ps1 -BuildDir build -Suite $suite
  if ($LASTEXITCODE) { throw "Failed: $suite" }
}
```

Additional checks passed: Linux GCC ASan+UBSan paint core (8 tests), native-coverage Python tests (9), generated coverage consistency, knowledge-reference validation (327 references) and `git diff --check`.

## Actual game validation

Comparison scenario command (use a disposable game copy):

```text
slrr_host.exe --game --root <isolated-game-copy> --window --spawn-car cars/racers/Baiern.rpk:0x6 PaintQA --auto-cmd 118
```

- **Whole-part paint:** first white can changes the clicked door only.
- **Spray:** hold and sweep the last dark can's spray across the front fender; a localized dark stroke appears, leaving the other panels alone.
- **Skin persistence:** save through SAVE PAINT JOB, exit the process, spawn a fresh red car, then LOAD PAINT JOB. The white door and dark spray return.
- **Decals:** buy the outlined Redline set through Catalog → Decals → Logos, return to Body paint → Apply decals. Hovering previews the decal; moving off the car clears it without painting. Clicking commits it, changes the count from five to four, and the mark remains after moving away. Save the paint job and load it in a fresh process: the decal returns.
- **Career persistence:** load the copied legacy Duhen career (including its stock JPEG side images), paint its front door white, return to the main menu to save, exit, and reload the career in a fresh process. The white door returns. Save logs report successful publication with zero degraded resources.

Final-renderer reload captures:

![Reloaded saved whole-part paint and spray](<skin-reload.png>)

![Reloaded saved decal](<decal-reload.png>)

![Reloaded career with its painted door](<career-reload.png>)

## Limitations

Projection, size/orientation and preview expiry are host reconstructions, not proven bit-exact original-engine behavior. Thin decal artwork can alias at the existing paint texture's resolution. The garage decal-picker thumbnail meshes remain blank; catalog images, selection, hover preview and application work. This change does not reconstruct the separate button-render mesh path.

Runtime paint images are limited to 4096×4096. PNG-backed SDAT textures are an OpenSLRR save extension; those newly written image blobs are not intended to load in the original executable. Automated fixture passes and the gameplay scenarios above do not establish complete compatibility with every mod or car.
