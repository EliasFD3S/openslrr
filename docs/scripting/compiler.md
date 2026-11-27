# Script compilation and Language native contracts

Implemented 11 November 2026. See also [ENGINE_CONTRACT.md](../core/ENGINE_CONTRACT.md).

## System.compileAll(String) : int

The host embeds the C++ TUFA compiler vendored under `third_party/slrrc` from
slrr-reverse. It does not spawn javac, Node.js, Python, or the original game.
The adapter is `src/jvm/script_compiler.cpp`; the binding is in
`src/natives/natives_io.cpp`. The compiler library uses C++17 and precise
floating-point semantics. Its literal-conversion self-test must pass before
any output is published. The rest of the host remains C++20.

The path is resolved against the game root. The selected directory is scanned
recursively; paths outside the root are rejected. The compiler reads the
game's existing TUFA declarations and source declarations for dependencies,
and resolves symbolic packs through the compiler's InstallPacks provider.
This provider reproduces a Build 940 session's pack-name conventions; exact
equivalence to every mod's live resource-registration order is not guaranteed.

The source layout is `X/src/Name.java`, with case-insensitive `src`. Output
is `X/Name.class`. Java files outside that source layout are ignored by the
vendored install scanner. The return value is the number of source units
attempted, matching the reference directory-walk contract, not the number of
generated classes. Logs distinguish attempted/succeeded/failed/class counts.

### Compiling with script mods

Both `System.compileAll` and `compile_game_sources.exe` use the enabled layers from [the mod load order](mods.md#which-class-wins). A logical directory such as `system/Scripts/mods/dash` includes visible sources at that path in mods and the base game. To compile just one mod, pass its physical directory (for example, `mods/dash`). This does **not** force a hidden source to win. Dependency declarations are selected by fully qualified class name and mod priority, including secondary classes in a TUFA. A fully shadowed unit is skipped. If a source also declares a visible sibling, the compiler rebuilds its entire TUFA beside its own `src/` directory; it never publishes into another layer. Give source-only dependencies explicit package declarations. Restart to use the compiled classes in the game.

### Float literal conversion (#420)

`System.compileAll` converts float literals as the Build 940 exe does
(setting B of `third_party/slrrc/floatlit.hpp`). The stock MWM classes were
compiled by x87 builds of the same routine (D for data classes, A for core
scripts), so `compile_game_sources` defaults to `--float-mode auto`: each
unit takes the setting whose literal bits occur in the class it replaces
(its `.openslrr.bak` first), B when nothing tells them apart. Pass
`--float-mode B` to reproduce classes built before #420; the shipped patch
classes affected are listed in [notes.md](notes.md#float-literals-of-rebuilt-stock-classes-420).

### Multiple classes in a single container

One Java-like source file can declare multiple classes. Its output is one
`.class` containing concatenated TUFA blobs, in declaration order. It is not
a Java SE class file. Independent `.java` files are not arbitrarily merged.
The host's TUFA reader validates all blobs before the file is published.

System sources may omit `package`. The adapter recovers their namespace from
the existing compiled container when possible, or from known engine script
directories. This avoids emitting an unqualified `System` instead of
`java.lang.System`, for example. Sources with an explicit package retain it.
Source-only dependency resolution in unusual mod layouts still needs separate
validation; successful compilation does not establish runtime equivalence.

### Publication and runtime lifetime

- Compilation happens in memory before replacing an output file.
- A syntax/code-generation/validation error preserves the previous output.
- The first replaced original is retained as `Name.class.openslrr.bak`.
- A temporary sibling file is published using a Windows atomic replacement.
- Script-thread checkpoints run between source units. A single compilation
  and initial declaration indexing remain synchronous C++ work.
- Existing VM classes are not replaced or reinitialized while executing.
  Restart the game to use regenerated code already loaded by the current VM.
  This also applies to direct `.class` paths: successful file loads are cached,
  and another container cannot overwrite a loaded class with the same exact
  name. New classes in a newly loaded concatenated container are still accepted.
  Failed loads can be retried after fixing the file.
- This implements explicit compileAll. Automatic recompilation on every
  missing/stale class lookup is not implemented by this change.

Compilation does not pretend to enqueue resource loading work in isLoading.

## System.isLoadingReset() : void

The original seeds a 32-sample resource loading history; it does not cancel
ongoing loading or clear the active-load flag. The host now keeps an analogous
history of measured pack-loading milliseconds. `openLib` and `rpkScan` record
their work, and host frames age the samples. Reset seeds the history from the
latest/pending work; a scan already running remains active.

`isLoading` reports active scans or history above a 4 ms average-work threshold.
Reading the flag never advances the ring. Idle frames drain it, and resetting
an idle loader creates no synthetic wait. This is a host approximation using
measured pack work, not a port of the original resource streaming queue.
Mesh/texture streaming queues remain outside its scope.

## System.analyze overloads

`analyze(Object)`, `analyze(float)` and `analyze(int)` deliberately retain
their no-op behavior. The reference PE implementations discard their arguments.
Adding logging, GC, serialization or script inspection would invent behavior.
These signatures remain in the no-op count rather than being relabeled as
newly implemented features.

## Validation

There is no compiler test target. A local probe compiled the installed patched `OptionsDialog.java`
against the game declarations without writing into the game. It generated
three classes in one 83,244-byte TUFA: `java.game.OptionsDialog`,
`java.game.ControlsFileReqDialog`, and `java.game.UR_Dialog`. Both the host
reader and SLRR-JVM-decompiler's strict parser accepted it. This is structural
validation, not an in-game test of every generated method.

### Shipped patch classes (#235)

The light API and the patched Garage were compiled with the CLI on a copy of
the game root, never on the installed game. From the repository root, in Git
Bash (`R` is gitignored scratch space):

```bash
R=build/issue235-compile-root
mkdir -p $R && for e in game/*; do case "${e##*/}" in save|screenshots|music) ;; *) cp -r "$e" $R/;; esac; done
mv $R/sl/Scripts/game/src/OptionsDialog.java $R/OptionsDialog.java.aside
mkdir -p $R/system/Scripts/render/src
cp patch/system/Scripts/render/src/*.java $R/system/Scripts/render/src/
./build/Release/compile_game_sources.exe "$(cygpath -w $R)" system/Scripts/render
cp patch/sl/Scripts/game/src/Garage.java $R/sl/Scripts/game/src/
./build/Release/compile_game_sources.exe "$(cygpath -w $R)" sl/Scripts/game
cp $R/system/Scripts/render/{LightSource,PointLight,SpotLight,RectLight}.class patch/system/Scripts/render/
cp $R/sl/Scripts/game/Garage.class patch/sl/Scripts/game/
```

The render package compiles first so Garage resolves `java.render.SpotLight`.
Moving `OptionsDialog.java` aside keeps the second run to Garage alone; the
directory argument compiles every `.java` file in the `src` folders below it. Results: 4/4 render
units, and 1/1 Garage unit producing three classes (94,397 bytes with the three garage tables; 94,435 since the #315 Car Lot save order, 94,595 since the #330 ROC sunlight), with two
`W_UNVERIFIED` warnings for constructs already in the MWM source (a final
static method, an empty `else ;`).

### OSD v2 classes (osdx and the Options screen)

`patch/system/Scripts/render/osdx` (the CRC 2005 OSD, see
[ui/osd-v2.md](../ui/osd-v2.md)) compiles before the game classes that use
it: copy the folder into the compile root, run
`compile_game_sources.exe <root> system/Scripts/render/osdx`, then
`compile_game_sources.exe <root> sl/Scripts/game` with `OptionsDialog.java`
and `OsdxOptions.java` in `sl/Scripts/game/src`. Results: 49 osdx classes
(37 + 12 dialogs) and the game classes, with `W_UNVERIFIED` warnings only.

The class index takes the first class file of a name in directory order, so
remove backup folders (for example `_openslrr_backup_235/`) from the compile
root: an old `OptionsDialog.class` there hides the fields of the current one.
A run that changes `OptionsDialog` fields used by `OsdxOptions` needs a second
run ("restart for loaded classes"): the first one reports `E_MEMBER` for the
new fields. For #390 a compile root copied from the patched game without
`save`, `music`, `screenshots` and `_openslrr_backup_235`, with only the two
sources in `sl/Scripts/game/src`, rebuilt the HEAD sources byte for byte
before the change was compiled.
For the same reason copy only the `src/` folders of osdx into the compile
root, not its `.class` files: an existing class file wins over its source, so
the other classes would compile against the old declarations.

A single-class import now shadows the classes of a star import, as in Java
(`third_party/slrrc/classes.cpp:lookup`): `import java.render.osdx.Osd;`
wins over `import java.render.*;`. No shipped source uses a single-class
import, so their output is unchanged.

### Patched car classes: stock anti-roll bars

The installed MWM 2.2.1 car classes differ from the `sources/` tree (MWM added
stock neons), and MWM ships no car source. The 24 patched racer sources under
`patch/cars/racers/*_data/scripts/src/` were first rebuilt to the installed
bytecode, then edited. A copied game root has no `src/` folder for cars, so
only the files placed there compile:

```bash
R=build/swaybar-compile-root   # copy of game/ as above
for f in patch/cars/racers/*/scripts/src/*.java; do
  mkdir -p "$(dirname "$R/${f#patch/}")"; cp "$f" "$R/${f#patch/}"; done
for d in Baiern_data Duhen_data Einvagen_data Enula_data Focer_data MC_data Nonus_data Prime_data SuperDuty_data; do
  ./build/Release/compile_game_sources.exe "$(cygpath -w $R)" cars/racers/$d/scripts; done
```

Check: compiling the sources with the bar lines still commented must give the
installed `game/` classes byte for byte, apart from header bytes 9-11
(`cmp -l a b | awk '$1>11'` prints nothing). Source line numbers are part of the
bytecode, so blank lines matter, and a resource literal must use the form the
file already uses (the SuperDuty rollbar matches its `stg_` list entry).
Results: 24/24 units identical before the edit (the first commit of #311 holds
that state); afterwards each class grows only by its bar lists. To locate MWM
additions, list the source line of every `addPart` in both class trees (the
TufaDecompiler `tufa.py` parser reads them) and diff the two lists.

Reference projects consulted: SLRR-JVM, SLRR-JVM-compiler,
SLRR-JVM-decompiler and slrr-reverse. The bundled compiler's exact provenance
and license are recorded in `third_party/slrrc/README.openslrr.md` and `LICENSE`.
