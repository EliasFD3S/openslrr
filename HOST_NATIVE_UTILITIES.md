# Rendering, sound and cursor native utilities

Implemented against `docs/stubs.txt`, the original script callers, and the
contracts documented in `repos/slrr-reverse/engine/Runtime/Render/RenderNatives.cpp`,
`Audio/Sound.cpp` and `Cars/Cars.cpp`.

| Native | Host behavior |
| --- | --- |
| `GfxEngine.forceRendering`, `flush` | Seven presentations, guarded against recursion. The loader is synchronous; bgfx frames retire deferred GPU resource releases. No Java callbacks or VM collection are entered by this presentation hook. |
| `GfxEngine.printScreen(String)` | Queues a framebuffer readback through bgfx's built-in TGA writer. Relative paths use the game root. Parent directories are created. The script's `printScreenIndexed` uses `.tga`, which is preserved without doubling the suffix. Requires the bgfx renderer. |
| `Text.setDefaultColor`, `getDefaultColor` | Complete 32-bit ARGB, initially `0xffffffff`. Applied on text creation; existing texts retain their colors. |
| `MouseCursor.getPos` | New Vector3 containing normalized screen x/y and zero z. A missing inner cursor returns null. |
| `MouseCursor.getPickedPos` | New Vector3 containing the world-space scene hit, using the same camera/ray convention as mechanic picking. Keeps the last hit on a miss, initially zero. |
| `Sound.changeMusicSet` | Selects Garage_Shop, Roam_ride, Race_chase or Main_menu under the game's music directory. Invalid set/NONE stops music. Re-selecting the current set does not restart it. |
| `Sound.nextTrack`, `prevTrack` | Wrap around the active playlist and actually switch the streamed track. |
| `Sound.getMode`, `setMode` | Per-playlist mode state (initially 3). Accept 0..3, preserve the old mode for invalid values. No active set: get returns -1, set returns its input, matching the reference native. Specific shuffle/repeat policies for these mode values have not been established; the host currently advances sequentially at EOF. |
| `Sound.has3DHardware`, `hasMixHardware` | Zero: the XAudio2/MCI backend does not provide the legacy DirectSound hardware-buffer capabilities. This does not mean sound is unavailable. |

Music streams through a separate Windows MCI alias. It does not use the FMV
alias or replace the XAudio2 effects engine. Music volume is independent of
effects volume. Polling runs during modal script pumps too. Playback/capture
need a manual check on the target machine; compilation and the headless VM
bootstrap do not establish their visual or audible output.

`SLRR_rev` remains a resource/script reference; no directly portable audio
device or screenshot implementation was taken from it.
