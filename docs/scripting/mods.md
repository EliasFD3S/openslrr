# Installing a script mod

Put the mod's folder under `mods/` in the game root (next to `system/`). Nothing needs to be copied over stock classes. For example:

~~~text
mods/
  load_order.txt                                  optional
  dash/
    mod.json
    system/Scripts/mods/dash/Mod.class            java.mods.dash.Mod
    system/Scripts/mods/dash/src/Mod.java         optional source
    sl/Scripts/game/Track.class                   optional stock override
~~~

`mod.json` is a flat UTF-8 JSON object:

~~~json
{"name":"Dash camera", "version":"1.0", "api":1, "entry":"java.mods.dash.Mod", "enabled":true}
~~~

`name` and `version` are nonempty strings. `api` is the nonnegative integer API level the mod requires; the host currently supports **1**. A mod requiring a newer host does not load. Omit `entry` for a replacement-only mod. Omit `enabled` to enable the mod; set it to `false` to disable it without uninstalling. Restart after changing or removing a mod.

## Which class wins?

List folder names (not display names) in `mods/load_order.txt`, one per line. First is highest priority. Blank lines and text after `#` are ignored. Unlisted folders follow in folder-name order. Under each VM classpath directory, the highest-priority enabled mod providing the requested file wins; the base game is last. Mirror the original path to replace an existing file. A TUFA container is replaced as a whole, not merged method by method. Keep new classes in a distinct package: `system/Scripts/mods/dash` maps to `java.mods.dash` even if the base install has no such directory.

At boot, look for `[mods]` lines. Each folder is listed or skipped with a reason (missing/invalid manifest, disabled, newer API). Conflicting files have a line naming the winner and the displaced providers:

~~~text
[mods] java.game.Track: mods/a (overrides base; also in mods/b)
~~~

Resource scripts naming a game-relative class file use the same overlay as package lookup. Disambiguate different packages with qualified names; an unqualified legacy reference is not a way to select between unrelated classes sharing a short name. Do not put a duplicate class definition in an unrelated container and rely on it loading first. RPKs, textures, general file I/O and native bindings are **not** overlaid. This is not a sandbox: install scripts only from sources you trust.

## Entry and compilation

The optional entry class declares the following method (no arguments or return value):

~~~java
package java.mods.dash;
import java.lang.*;

public class Mod {
    public static void modInit() { System.log("Dash loaded"); }
}
~~~

Game boot calls entries after stock `Init(int)` returns, before the main menu runs. Entries run **lowest priority first**, so the highest-priority mod registers last. A missing entry class or method is logged; boot continues with the other mods.

Distribute compiled TUFA classes, not javac bytecode. To build against a private game copy, install the manifest and source there and run:

~~~text
compile_game_sources.exe <game-copy> mods/dash
~~~

The compiler and `System.compileAll` select visible classes/sources in load order. Compiling a logical game directory also includes its visible mod sources; output goes beside the winning source's `src/` directory, never into a shadowed layer. Make the mod highest priority when rebuilding a class normally hidden by another mod. Use explicit package declarations for classes used by other source-only units. Restart to load newly compiled classes. See [the compiler guide](compiler.md) for TUFA details.
