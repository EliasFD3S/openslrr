# Saves, files and options: issue notes

File-system confinement, SDAT/JSON save fixes, save ordering in the
garage scripts and the options save policy. The save format is in
[json-schema.md](json-schema.md).

## Options save failure policy (#109)

`host_game_options_update` keeps failed options dirty but suspends automatic
saveConfig retries after the first failure. `host_game_options_dirty` rearms
the normal 500 ms debounce on a new setting change. Forced updates (including
shutdown) can still retry; repeated failures without a change log only once.
Success clears dirty/failure state, and options load resets it for a new
session. Runtime settings continue to apply even when persistence fails.

Release build and `options_retry` pass. An isolated compiled Config fixture
binds a counted saveConfig adapter: checks debounce, failure followed by 100
updates after another debounce interval, rearming on a real GPS option change,
forced retry, recovery and no extra writes after success. This tests the save
result policy, not filesystem permissions; the existing save implementation is
unchanged. Evidence: `assets/validation/options-retry/after.txt`.


Enhanced settings persistence follow-up: MSAA, dynamic reflections, volumetric
clouds, sunlight and local lights now round-trip through
save/game/openslrr_options beside framerate_mode. The renderer-option native
saves after explicit changes; debug-menu changes/reset use
`host_save_graphics_options`. This does not extend the stock Config binary
format. Missing keys retain defaults; invalid MSAA/bool lines are ignored.
Loading resets defaults first so settings do not leak between roots/sessions.
The parent directory is created on an explicit host save. Save failures are
reported per user change, without a periodic retry loop.

Release build, options_retry and script_safety pass. The persistence test writes
and reloads every MSAA index (0=off, 1=2x, 2=4x, 3=8x), all enhanced booleans,
framerate coexistence, the debug-save entry point, legacy and invalid files.
These are real disk round-trips in an isolated fixture; the stock Config
saveConfig remains counted/injected for retry-policy tests.

## Host options file as JSON (#405)

The host-only options (renderer option ids 0-31: video, audio, controls and
force feedback) were read and written line by line in
`host_game_options.cpp`, with a third copy of every range in the
`getRendererOption`/`setRendererOption` switches; each new option meant
three edits that could disagree.

Now one table (`kOptions` in `host_game_options.cpp`) gives each option its
id, JSON section and key, range and accessors; loading, saving and both
natives use it. The file is `save/game/openslrr_options.json`
(`src/platform/host_options_json.cpp`): `version`, then `video`, `audio` and
`input` objects, booleans as `true`/`false`, the framerate mode by name. It is
written to a `.tmp` file and renamed over the old one. On load a missing key
keeps its default, a value out of range is clamped, a wrong type or unknown
key is ignored; a file that does not parse is renamed to `.bad` and the
defaults are used. When no JSON file exists, the key=value
`save/game/openslrr_options` of older builds is read once and saved as JSON;
it is left in place for those builds. A setter saves only when the value
changed.

Validation: `options_retry` (now also: the JSON layout, missing keys,
clamps, wrong types, a damaged file, the migration, every id 0-31 through
the file) and `script_safety` pass. Not run in game.

## Garage main-menu inventory save ordering (#59)

The verified MWM Garage.class main-menu command saves before changing section.
This differs from sources/sl/Scripts/game/src/Garage.java, which already changes
section first. A save taken before Garage.exit sees player.parts empty because
the purchased parts still belong to the mechanic's VisualInventory.

The distributed patch/sl/Scripts/game/Garage.class swaps the two standalone
TREE statements. GameLogic.changeActiveSection now runs Garage.exit and
Mechanic.flushInventory before autoSaveQuiet snapshots player.parts. The VM,
natives and save format are unchanged. scripts/patch_garage_save_order.py
reproduces the binary patch from one SHA-256 verified MWM input and rejects
other mods; do not apply binary offsets to an unknown Garage container.
Since #235 the shipped class is compiled from `patch/sl/Scripts/game/src/Garage.java`
instead, which keeps this order; the binary builder is kept as the #59 reference.

Validation: inventory_transition executes the actual shipped GameLogic transition,
Mechanic.flushInventory and Garage.osdCommand/exit with isolated presentation
callbacks. The original records zero transferred items at save and three after
exit; the patched handler records all three before save, with one exit/enter.
This verifies call ordering, not a complete career file round-trip or gameplay.

## Car Lot save ordering (#315)

MWM saves before entering the Car Lot: in the installed `Garage.class`,
`GameLogic.autoSaveQuiet()` is at source line 1520 and
`changeActiveSection(player.carlot)` at line 1521. The decompiled `sources/` Garage
has them the other way round, and since #235 the shipped class is compiled from
that order. The save then ran after `CarLot.enter` had parked the current car.
`GameLogic.save` brackets its car save with `carlot.lockPlayerCar()` and
`releasePlayerCar()`. The lock found no `player.car` and did nothing, and the
release handed the parked car back to the player (`setParent(player)`, outside
the lot's world) and cleared `carSlots[curcar]`. The lot drew every other car,
and the header still named the current one. Choosing another car then ran
`saveCar(old_curcar)` on a slot marked empty, which deletes
`save/temp/PlayerCar<n>` without writing it.

The fix restores the MWM order for this command only, and first flushes the
mechanic strip (`mechanic.flushInventory()`), so the save still holds what the
#59/#235 order protects. `Garage.exit` flushes again and finds nothing. This is
a script fix. The host's `setParent` keeps its meaning: a car parented to a
`Player` belongs to no world and is not drawn.

Lessons:

- A decompiled `sources/` class is not proof of stock call order. Before
  reordering or relying on statement order in a patched class, read the
  installed class's TREE. The source line of each node gives the order
  (`scripts/patch_garage_save_order.py` shows the TREE layout). MWM saves
  before the transition for the main menu, Choose Track, both dealers and Car Lot.
- A blank Car Lot slot whose header still names a car means the car left the
  lot's world after `CarLot.enter`. Look for a `releasePlayerCar` between enter
  and the first frame, not for a load failure.
- It was not caused by the JSON saves (#291): it reproduces at `5b002e4` with an
  SDAT career, and the stock MWM `Garage.class` shows the car on the current host.

Validation: `shipped_carlot_save_order` in `tests/test_inventory_transition.cpp`
runs the shipped `osdCommand(CMD_CARLOT)` with the real `GameLogic` transition
and `Mechanic.flushInventory`: three items are moved before the save, with one
exit and one enter. The recording probe expects `flush,save,change`. In game
(OpenSLRR host, scripted `--bench-career ... --auto-cmd 111`, not interactive):
a one-car JSON career and a four-car SDAT career both show the current car in
its slot. Before/after captures are in `assets/validation/carlot-current-car/`.
Picking another car in the lot afterwards was not played through.
The suite also checks generic interface dispatch; no class-specific VM workaround
is needed. In-game validation must buy parts, leave via Main Menu, reload and
check the inventory, then repeat with normal Exit Game saving.

## File/save I/O fixes — issues #68, #70 and #71

Baseline: `17115433f8aaa2e6a0bcce970c25afdd7dc01ce0` (GitHub main, 16 November 2026).
The changes are engine-side native fixes. They require no Java patch or save-format migration.

### Changes and contracts

#### #68 — File.delete must not recursively erase the installation

`File.delete(".")` previously resolved to the game root and passed it to
`std::filesystem::remove_all`. A nonempty career directory was recursively
removed too. The shared `under_game` helper now rejects a canonical target equal
to the root, and compares complete path components after canonicalization to
reject resolved escapes. `File.delete` uses `remove`, so it deletes a file or
an empty directory and fails on a nonempty directory. The existing successful
return for a missing path is preserved.

The stock `File.delete(path, mask)` wrapper enumerates names and invokes the
single-path native. Stock menu code deletes directory contents before deleting
the directory itself. This flow still works. No save-only allowlist was added:
other legitimate game-relative paths remain available. FindFile now shares
the same physical confinement (#84, below).

#### #71 — Two dots inside a career name are not a parent-directory component

The old substring check rejected `J..R-1` and `Mr...-1` along with `..`.
The validator now rejects components equal to `..`, while accepting ordinary
names containing dots. Explicit parent traversal, drive/alternate-stream
syntax, embedded NULs, the canonical root and resolved escapes are rejected.
Both forward and backslash separators are handled.

#### #70 — Unrepresentable resources must not shift the rest of a save

An SDAT ResourceRef is a four-byte field. If the resource cannot be encoded,
the native now writes a four-byte null placeholder, increments the file's
`degraded_resources` count and logs the affected ID. It still returns failure
(0), so callers checking the result can detect the loss. Close logs the
number of placeholders. Later integers, strings and resources retain their
correct offsets even when stock Java ignores the failure return.

Valid and null SDAT references keep their existing representation. Raw files
keep their original IDs. This does not recover the missing resource itself.
The previous File.close durability/atomicity issue (#69) is not addressed.

### Reproducible validation

Build the Release host first, then run from the repository root:

```powershell
cmake --build build --config Release --target slrr_host --parallel 8
.\scripts\run_save_io_tests.ps1
.\scripts\run_save_io_tests.ps1 -BaselineRef 17115433f8aaa2e6a0bcce970c25afdd7dc01ce0
```

The runner discovers the installed MSVC/Windows SDK, links the existing Release
host objects except main with `tests/test_save_io.cpp`, and invokes the actual
native registry. It does not stub file operations, SDAT serialization, the
path validator, or the native return values. It opens no game window.

The baseline run substitutes the original `natives_boot.cpp` and
`natives_io.cpp` object files, extracted read-only from the specified Git
revision. It never checks out or reverts working files. Exit 1 is expected for
the baseline and exit 0 for the corrected build.

All file writes/deletes happen under process-specific directories in
`build/save-io-fixtures/`; the game root used by each fixture is that isolated
directory. Tests use no original game assets, player profiles or real saves.
The minimal VM has no classpath: `load_class: classpath not ready` messages are
expected when readString allocates the fixture's String objects. They do not
indicate a failure to load game data.

| Case | Baseline | Corrected |
| --- | --- | --- |
| Root deletion/write refusal | FAIL | PASS |
| Nonempty directory deletion refusal | FAIL | PASS |
| Stock delete-contents-then-directory and missing-path behavior | PASS | PASS |
| Dots within names versus parent traversal | FAIL | PASS |
| Dotted career name write/read/delete round trip | FAIL | PASS |
| Unknown-pack resource followed by integer/string fields | FAIL | PASS |
| Packless resource followed by integer/string fields | FAIL | PASS |
| Unsupported dynamic-blob resource followed by integer/string fields | FAIL | PASS |
| Valid and null resource round trip | PASS | PASS |
| Raw resource ID round trip | PASS | PASS |
| **Total** | **3 passed / 7 failed** | **10 passed / 0 failed** |

Recorded summary: [before](../../assets/validation/save-io/before.txt) and
[after](../../assets/validation/save-io/after.txt). Full local logs are written to
`build/save-io-tests/`. Screenshots are not relevant to these nonvisual fixes.

The Release x64 host builds successfully. Knowledge-reference checks are run
separately. These checks establish the named native/file regressions; they
are not an in-game validation of the complete career save/load flow.

### #84 — FindFile path confinement

FindFile directories and children now use the same canonical component checks
as File. Enumeration alone may address the root. Leading separators remain
game-relative, so delete-by-mask lists and deletes from the same directory.
Parent traversal, drive/alternate-stream syntax, embedded NULs and resolved
escapes yield no entries and clear earlier state. Dotted names, masks, sorting
and file/directory flags are preserved.

Physical confinement is intentional: internal NTFS junctions work, external
junctions remain rejected, including same-prefix sibling targets. This is path
validation, not protection against concurrent reparse-point replacement.

Run `scripts/run_save_io_tests.ps1 -Suite findfile_paths`, optionally with
`-BaselineRef db8f069`. Baseline: 1 pass / 4 failures; corrected: 5 passes.
The real native registry and isolated NTFS junctions are exercised. Existing
`save_io` and `save_persistence` suites each pass 10 tests. No Java patch or
save-format change is required. See [evidence](../../assets/validation/findfile-paths/results.txt).

## Save persistence fixes (#69, #85, #98)

File.close now writes to a uniquely created sibling file, checks every write,
flushes and closes it, then publishes it with MoveFileExW replacement and
write-through. Failure preserves an existing destination and retains the newest
bytes in the existing process-local fallback buffer. File.exists, copy and move
now agree with reads about these pending bytes. Successful disk transfers clear
stale destination buffers. Failed transfers retain their source buffer.

This is single-file publication, not a multi-file career transaction. Pending
buffers are lost when the process exits. Java code which deletes a save before
writing it still needs a separate transaction fix; this does not resolve #59.

File.write(String) stores null as length zero and a non-null empty string as
length one plus NUL. This agrees with the host reader and SLRR_rev's
[src/runtime/memory-file.ts](https://github.com/nedobylskiy/SLRR_rev/blob/main/src/runtime/memory-file.ts)
and [file-native.ts](https://github.com/nedobylskiy/SLRR_rev/blob/main/src/runtime/file-native.ts).
This reference is not a new verification against the original MWM executable.

Part.getSlotDamage returns null for absent damage and invalid slots without
allocating empty strings. The stock Part.java save loop uses the null sentinel
to decide whether a damage record needs serialization. Actual damage text and
set/clear behavior are preserved. No Java changes are needed.

### Validation

Run from the repository in PowerShell after building Release:

```powershell
./scripts/run_save_io_tests.ps1
./scripts/run_save_io_tests.ps1 -Suite save_persistence
./scripts/run_save_io_tests.ps1 -Suite save_persistence -BaselineRef 71aec8b
python scripts/check_knowledge_refs.py
```

The original save I/O suite passes all 10 cases. The new persistence suite
passes all 10 cases; compiling the same cases against main 71aec8b produces
6 failures and 4 passes. Tests cover locked publication, pending existence,
copy/move and failed-copy behavior, stale destination buffers, empty/null string
wire format and alignment, and absent/set/cleared slot damage. The baseline
passes some transfer cases because direct truncation bypasses the intended
replacement lock; those are forward regression checks for the new fallback.

Tests use isolated generated fixtures under build/save-io-fixtures and a minimal
RPK/CFG, never player saves. The runner links the actual Release host objects.
[Before](../../assets/validation/save-persistence/before.txt) and
[after](../../assets/validation/save-persistence/after.txt) record the results.
Screenshots add no evidence for these nonvisual changes.

Release x64 builds successfully. Knowledge references: 219 checked, zero errors.
In-game career save/load validation remains pending.
