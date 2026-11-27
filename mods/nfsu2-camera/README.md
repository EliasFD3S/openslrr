# NFSU2 camera (mod)

Need for Speed: Underground 2 style driving camera for OpenSLRR (issue #470),
built on the mod API (`java.openslrr`, [docs/scripting/mod-api.md](../../docs/scripting/mod-api.md)).
It adds the camera **ARCADE** to **Options → Game → CAMERA**; **CLASSIC**
keeps the stock cameras.

## Install

Copy this folder to `<game>/mods/nfsu2-camera` (next to `system/`) and
restart. It needs OpenSLRR with mod API 1 and the patched `Track.class`
and `system/Scripts/openslrr` classes from `patch/`. `scripts/build-run-branch.ps1`
installs it with the patch. Remove the folder, or set `"enabled": false`
in `mod.json`, to uninstall. The `tests` folder is not part of the mod.

## Use

Select **ARCADE** under **Options → Game → CAMERA**. While driving:

| Key | Action |
| --- | --- |
| V (CHANGE VIEW) | Cycle the views: BUMPER → HOOD → NEAR → FAR. |
| B (LOOK BACK), held | Rear view: the chase views look at the car from in front of it; BUMPER and HOOD look back from the rear bumper. |
| NEXT ARCADE VIEW | Same as V. Unbound by default; bind it under **Options → Controls → Game controls**. |

The view is remembered between sessions (`camera.ARCADE.view`). Race
staging and other scripted cameras take over as before; V brings the ARCADE
camera back.

**Options → Mods**:

- **ARCADE VIEW** (BUMPER, HOOD, NEAR, FAR; default FAR): picks the view.
  A new profile starts in it.
- **ARCADE SHAKE (%)** (0–200, default 100): strength of the road
  vibration, nitrous and crash shakes. 0 turns them off. The tilt in bends
  is not a shake and stays.

## What it does

A port of the engine arcade camera that #476 removed (commit 46f90d7,
`src/resources/arcade_camera.cpp`), with the same values:

- **BUMPER**: low, just ahead of the front bumper. **HOOD**: just below the
  roof at the base of the windscreen, looking over the bonnet. Both are fixed
  to the body (placed from the car's mesh extent, `CarState.boxMin/boxMax`)
  and follow it as it turns, pitches and rolls.
- **NEAR** (4.4 m) and **FAR** (6.3 m) chase the car low behind it. In a
  slide the eye swings toward the direction of travel (60 % of up to 0.6 rad of
  slip, from about 40 km/h) so the car's angle shows; in bends it lags the car
  by at most 1.2 rad and catches up faster at speed.
- Speed: the view angle widens by 20–22° by 250 km/h and the eye drops back
  and lower; it also drops back while the car pulls and closes in under braking.
- Nitrous: +10° of view angle, 0.8 m further back, a light shake.
- Road vibration from 150 km/h (strongest on the bumper), a crash shake for
  a velocity change above 4 g, a tilt of up to 0.06 rad in bends, and a view
  that floats behind jumps (the slide is not followed in the air).
- The chase eye is kept out of walls with `CameraControl.sweep` (0.15 m
  sphere), as the engine chase camera does.

The values approximate NFSU2's views by eye: the game's own camera table was
not available.

V and NEXT ARCADE VIEW do nothing while LOOK BACK is held.

For tuning, `"nfsu2camera.debug": 1` in the `mods` section of
`save/game/openslrr_options.json` logs the camera state once a second
(`[nfsu2camera]` lines).

## Limitations

- No right-drag look and no mouse-wheel zoom on the ARCADE views: the mod API
  has no mouse input for a provider camera.
- NEXT ARCADE VIEW shows no "Camera:" message (only Track's V key does).
- The NEXT ARCADE VIEW key works while the ARCADE camera is showing, also
  behind the in-game menu (it is a global hotkey: providers have no OSD).

## Files

| File | Role |
| --- | --- |
| `mod.json` | Name, version, API 1, entry `java.mods.nfsu2camera.Mod`. |
| `system/Scripts/mods/nfsu2camera/Mod` | `modInit`: settings, key, provider registration. |
| `.../ArcadeCamera` | The `CameraProvider`: state, poses, placement. |
| `.../ArcadeMath` | Pure math and tuning (no natives besides `Math`). |
| `.../ArcadeDriver` | `animate()` callback and the key's `osdCommand`. |
| `tests/nfsu2-camera-test` | VM test (43 checks), a separate mod; see below. |

Sources are in `src/`. To rebuild against a private game copy that has this
folder under `mods/`: `compile_game_sources.exe <game> mods/nfsu2-camera`.

To run the VM test, copy `tests/nfsu2-camera-test` to
`<game>/mods/nfsu2-camera-test` as well, boot the game
(`slrr_host --game --root <game> --no-wait --auto-cmd mainmenu`) and read
the `[arcadetest]` lines; the game quits after the run. Remove the test mod
afterwards.
