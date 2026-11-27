# Mod settings and key bindings (API 1)

Import `java.openslrr.*`. Register settings and keys in the mod entry point's
`public static void modInit()`. Give each setting or action a stable ID;
changing its label is fine, but changing its ID changes its identity.

## Store a setting

`ModSettings` stores scalars in the `mods` object in
`save/game/openslrr_options.json`. A dot is part of the key, not a path.
Use the mod's name as a prefix so two mods don't write the same setting.
Reading a missing key returns the supplied default without writing a file.

~~~java
int mode = ModSettings.getInt("roadbook.mode", 0);
float scale = ModSettings.getFloat("roadbook.scale", 1.0);
String title = ModSettings.getString("roadbook.title", "Roadbook");
ModSettings.setInt("roadbook.mode", mode);
ModSettings.setFloat("roadbook.scale", scale);
ModSettings.setString("roadbook.title", title);
~~~

## Show a setting in Options → Mods

~~~java
String[] units = new String[2];
units[0] = "KM";
units[1] = "MI";
ModOptions.addChoice("roadbook.units", "DISTANCE", units, 0, "Units for the trip meter.");
ModOptions.addToggle("roadbook.show", "SHOW ROADBOOK", 1, "Display the roadbook on track.");
ModOptions.addSlider("roadbook.scale", "MAP SCALE", 0.5, 2.0, 1.0, "Scale the roadbook map.");
~~~

Choices store the zero-based index, toggles store 0 or 1, and sliders store
a float. A changed value is written to disk 500 ms after the last change
(dragging a slider does not rewrite the file every tick), at shutdown, or
before options reload. Read the same key and default with `ModSettings` when consuming the
value. Registering a row does not overwrite the player's setting.

## Register a rebindable action

~~~java
// In modInit; keep the returned axis for the lifetime of this VM.
// No default here (0): the player binds it. M would collide with VIEW MAP.
axis = ModKeys.register("roadbook.mark", "MARK WAYPOINT", 0);
// When creating the mod's OSD:
if (axis >= 0)
    osd.createHotkey(axis, Input.VIRTUAL|Osd.HK_STATIC, CMD_MARK, this);
~~~

The default is a physical DirectInput key code, or 0 for none. Nothing checks
it against the stock controls or other mods: pick a key the game does not
use, or leave it unbound. The return value is a
**virtual axis**, not a key code or ControlSet index. Handle the command on
the OSD receiver just as for a stock virtual hotkey. Players can rebind it
under **Options → Controls → Game controls**. Sixteen ControlSet slots
(118–133) are reserved for mod actions; existing stock and 118-slot control
files remain usable. A small trailer records the IDs so that load-order
changes do not swap bindings. Esc-clearing a binding persists as well.
The trailer (`MKEY`, a count, then slot, ID and flags per key) follows the
smoothing table of the controls file. An older (#405 or earlier) build that
saves a 134-slot file again drops it, and a cut trailer is read only up to
its last complete record; mod keys without a record count as new and get
their defaults back.
An empty ID or a seventeenth distinct key returns -1 without registering
a row. Registered keys become active on the next controller reset. A key
registered after Options → Controls was opened appears when it is opened
again.

The Game page lists Classic plus each registered camera provider. The
selection uses `camera.provider` (provider name; empty string for Classic);
each provider's view uses `camera.<name>.view` (zero-based). Give providers
distinct stable names.
