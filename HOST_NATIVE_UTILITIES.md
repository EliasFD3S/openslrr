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
| `Sound.has3DHardware`, `hasMixHardware` | Zero: the XAudio2 backend does not provide the legacy DirectSound hardware-buffer capabilities. This does not mean sound is unavailable. |

Music uses Media Foundation to decode MP3/WAV/WMA and a separate XAudio2
streaming voice. MCI and DirectShow failed to open the stock menu MP3 on the
host machine; the replacement successfully decodes and starts playback.
At most 24 decoded chunks are queued, rather than loading complete songs.
The music voice is independent of FMV and effects volume, and is refilled
in modal script pumps as well as the main loop. Startup logs list scan counts
and track starts; decoding failures include their HRESULT.

## Audio options

The VM now keeps `Input.createHotkey` on Input rather than rewriting it to
`Osd.createHotkey`. The existing Java Osd overloads can therefore activate
hotkeys without the missing-method error. This does not add a general keyboard
hotkey dispatcher to the host.

Slider hit boxes (knob or track Button.parent) now route through the Slider's
range/tick-aware `setValue`. The host copies the result to `osd.sliderValue`
before dispatching its command. Async GameType handlers receive an Event with
F_GAD_CHANGE and the Slider as gadget, through handleMessage(Event); synchronous
dialog handlers retain osdCommand. This keeps tuning controls separate from
the dialog OK/Cancel buttons. Reference casts in TREE no longer turn subsequent
instance assignments into static writes. Mouse capture keeps dragging active outside
the track; release, cancellation and hidden controls are handled.

The engine-owned Config instance is loaded before Init and retained as a VM
static. Audio edits queue a save after 500 ms of inactivity and flush on normal
shutdown. Persistence uses the original Java `Config.saveConfig/loadConfig`
and `save/game/options` SDAT format, including all three channel volumes and
the other Config fields. No original Java files are patched. The engine volume
value is saved/restored; engine-loop audio itself is not yet implemented.

A temporary headless diagnostic exercised the actual OptionsDialog sliders,
verified 20/40/60% through save/reload in an isolated directory, and started all
four music sets with volume zero. Audible playback and visual mouse interaction
remain user checks; no game window was launched.

`SLRR_rev` remains a resource/script reference; no directly portable audio
device or screenshot implementation was taken from it.

## Repeated effects and UI bursts

Transient XAudio2 effects are grouped by normalized WAV path. Requests closer
than 35 ms are coalesced before reading/allocating another voice. At most two
copies of one transient cue may overlap; their gains are divided by the copy
count so correlated waveforms cannot multiply that cue's peak amplitude.
Coalesced requests return the already playing voice handle.

Menu hover/select/back share a three-voice budget and may repeat rapidly
without the transient 35 ms coalescing window. Select/back remove
lingering hover voices, and hover cannot obscure an active select/back sound.
Looping sounds are excluded from grouping. The existing overall 32-voice
budget evicts a transient first; it no longer interrupts a loop to admit a
new voice when all slots are occupied by loops.

`host_sfx_stats()` exposes the active voice and coalesced-request counts.
A temporary muted XAudio2 diagnostic confirmed 1,000 hover requests -> one
voice, 1,000 wrench requests -> one voice, two spaced wrench requests -> two
voices, click priority, and preservation of independent looping voices.

A temporary fuel-rail tuning diagnostic verified that dragging updates the
mixture ratio to 17:1 while keeping the dialog shown and its result untouched.
The audio-slider/save-reload diagnostic also passes after this correction.
