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

List folder names (not display names) in UTF-8 in `mods/load_order.txt`, one per line. First is highest priority. Names are matched without regard to case; unknown names are logged and ignored. Blank lines and text after `#` are ignored. Unlisted folders follow in case-insensitive folder-name order. Folder and class filenames need not be representable in the Windows ANSI code page. Names that cannot be encoded in UTF-8 (for example, an unpaired UTF-16 surrogate on NTFS) are skipped with a diagnostic. Use the same spelling for non-ASCII letters in script paths: load-order folder names use Windows ordinal case-insensitive comparison, but logical class paths and compiler sort keys fold ASCII letters only. Class identities are case-sensitive.

The VM selects **one definition per fully qualified class name**, from the highest-priority enabled mod that declares it. For example, a mod's `system/Scripts/game/Track.class` can replace stock `sl/Scripts/game/Track.class`: both declare `java.game.Track`. The classpath root and the TUFA filename do not break ties between mods. A container can declare several classes; replacing one does not replace its siblings, and methods from competing definitions are never merged. For new classes, use a distinct package: `system/Scripts/mods/dash` maps to `java.mods.dash` even if the base install has no such directory.

A corrupt or empty mod container does not hide a valid file in a lower-priority mod or the base game. If one mod declares the same class in several containers, the VM and compiler prefer a file named exactly after the class. Otherwise the first directory, then filename, in ASCII-folded order wins.

At boot, look for `[mods]` lines. Each folder is listed or skipped with a reason (missing/invalid manifest, disabled, newer API). Each overridden or duplicate class has a line naming the winner and the displaced providers:

~~~text
[mods] java.game.Track: mods/a (overrides base; also in mods/b)
~~~

Resource scripts naming a game-relative class file use the same overlay as package lookup. An existing resource container determines the class identity, even if another package's class with that filename is already loaded. If the script file is missing from both the base and the overlay, the host reuses a loaded class only when exactly one simple name matches the filename stem. It does not cache that guess. Disambiguate different packages with qualified names; an unqualified legacy reference is not a way to select between unrelated classes sharing a short name. RPK discovery skips the entire top-level `mods/` directory, including enabled mods. Install a script mod there, not a resource pack. Textures, general file I/O and native bindings are **not** overlaid. This is not a sandbox: install scripts only from sources you trust.

With or without mods, boot indexes the declarations in base containers too. This makes a stock helper loadable by its own class name even when a mod replaces the primary class from a different file. The VM and compiler follow junctions and symlinked directories, skipping cycles and directories whose names cannot be encoded in UTF-8. The `[mods] overlay scan` line measures indexing, not the entire boot; see the [MWM probe measurements](notes.md#486-mod-overlay-folders).

## Entry and compilation

The optional entry class declares the following method (no arguments or return value):

~~~java
package java.mods.dash;
import java.lang.*;

public class Mod {
    public static void modInit() { System.log("Dash loaded"); }
}
~~~

Game boot calls entries after stock `Init(int)` completes, before the main menu runs. If stock startup aborts (for example on an unbound native), boot logs that it skipped `modInit`; no entry runs. Entries run **lowest priority first**, so the highest-priority mod registers last. An entry names an exact class; a matching filename in another package is not enough. If several mods name the same entry, it runs once, at the highest-priority one's turn; the other declarations are logged as duplicates. A missing entry class or method is logged; boot continues with the other mods.

Distribute compiled TUFA classes, not javac bytecode. To build against a private game copy, install the manifest and source there and run:

~~~text
compile_game_sources.exe <game-copy> mods/dash
~~~

The compiler and `System.compileAll` select visible classes/sources in load order. Compiling a logical game directory also includes its visible mod sources; output goes beside the winning source's `src/` directory, never into a shadowed layer. Make the mod highest priority when rebuilding a class normally hidden by another mod. Use explicit package declarations for classes used by other source-only units. Restart to load newly compiled classes. See [the compiler guide](compiler.md) for TUFA details.
