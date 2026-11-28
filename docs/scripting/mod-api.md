# Mod API (java.openslrr)

OpenSLRR ships a small, versioned script API that mods use instead of
replacing stock classes such as `java.game.Track`. Its classes live in
`patch/system/Scripts/openslrr` (package `java.openslrr`, sources in
`src/`); the natives are in `src/natives/natives_openslrr.cpp`. Mod folders,
load order and the mod entry point are described separately (#486); mod keys
and settings are #488.

The API exists only in OpenSLRR: a script that calls it does not run in the
original game.

## Version

`OpenSLRR.apiVersion()` returns `kModApiVersion` (`include/slrr/mod_api.hpp`),
currently **1**. A mod declares the version it needs in its `mod.json`
(`"api"`); the engine skips a mod that needs a newer one. The version grows
whenever a class or method of this package changes incompatibly or a mod could
depend on an addition.

## CarState

Static natives that take the car's GameRef (`Track.cameraTarget`, `player.car`).
A GameRef that is not a car (the chassis or its Vehicle) gives `null` for
vectors and `0` / `0.0` otherwise; an installed part such as a wheel is not a
car. The same applies to the car of `setPoseRelative` / `setPoseLocal`.
World axes, y up; metres, m/s, rad/s.

| Method | Returns |
|---|---|
| `Vector3 drawnPos(GameRef car)` | The chassis origin as drawn: the car between its last two physics steps (the frame the chase camera follows). |
| `Ypr drawnOri(GameRef car)` | The drawn orientation, extracted like `GameRef.getOri`. |
| `Vector3 vel(GameRef car)` | World linear velocity of the chassis body (last physics step). |
| `Vector3 angVel(GameRef car)` | World angular velocity of the chassis body, rad/s. |
| `float speed(GameRef car)` | Length of `vel`, m/s. |
| `int nitro(GameRef car)` | 1 while the nitrous burns. |
| `int wheelsOnGround(GameRef car)` | Wheels in ground contact, 0..4 (0 while physics is inactive). |
| `Vector3 boxMin(GameRef car)`, `boxMax` | Extent of the car's meshes in its chassis frame (x right, y up, z back), relative to the chassis origin. Measured from the drawn meshes on each call, in the chassis node's own drawn frame, so an offset or rotated body mesh or a large attachment does not move the frame. This is the frame of `setPoseLocal`. |

`drawnPos` is the chassis origin, not the reference point `GameRef.getPos`
returns for a car (near the wheel contacts, about 0.7 m lower on the Baiern).

**Timing.** Scripts (`animate()` callbacks) run before the frame's car poses
are final. With the frame-locked physics of the benchmark, `drawnPos` and
`drawnOri` are exactly the pose drawn in this frame. With the free-running
physics of the game, the poses are published after the scripts and just before
the frame is drawn, so the values read in `animate()` are those of the previous
frame. A camera placed from them with `setPose` trails the car by one frame;
use `setPoseRelative` or `setPoseLocal` to stay on the drawn car.

## CameraControl

Static natives on a script camera (a GameRef of type `GameRef.RID_CAMERA`):
the world camera or a preview camera drawn into an inset viewport (a car or
part preview). `setPoseRelative` / `setPoseLocal` follow a car in the active
world; in a preview world, whose car has no drawn frame there, use `setPose`.
While a pose is set it is the camera's whole view: `move`, `look`, `dist`,
`angle` and `roll` do not apply until `clearPose`.

| Method | Effect |
|---|---|
| `setPose(cam, eye, at, up, fovDeg)` | Exact world eye, focus and up, no smoothing. |
| `setPoseRelative(cam, car, eyeOffset, atOffset, up, fovDeg)` | Eye and focus are world-space offsets from the car's drawn origin. The engine adds the origin when it draws the frame, so the camera stays on the drawn car at any frame rate. The offsets and up are not turned with the car: a chase camera that turns them itself (with `drawnOri`) uses the previous frame's orientation in free-running play, which is fine for a camera that swings behind the car but not for one fixed to the body. |
| `setPoseLocal(cam, car, eyeLocal, atLocal, upLocal, fovDeg)` | Eye, focus and up in the car's chassis frame (metres; x right, y up, z back, the frame of `boxMin` / `boxMax`). When it draws the frame, the engine turns them with the car's drawn orientation and adds its drawn origin, so a bumper or hood camera stays on the body while the car turns, pitches and banks. A null or zero `upLocal` is the car's up. |
| `clearPose(cam)` | Back to the stock camera behaviour, from the last pose. |
| `float sweep(from, to, radius)` | Fraction 0..1 of the segment that a sphere of this radius (at most 2 m) travels before it touches the scenery; 1 is clear. The engine's chase camera uses the same sweep. A missing or non-finite endpoint, or a negative, NaN or infinite radius, returns 1 without sweeping. |

`fovDeg` is the vertical view angle in degrees, clamped to 1..170; 0 or less
keeps the camera's own (the `zoom` command, 60 by default). A null or zero
`up` is world up (the car's up for `setPoseLocal`); an up along the view
direction falls back to that, then to world -z. A pose with a NaN or infinite
value anywhere (eye, focus, up or fov), or an eye equal to its focus, is
ignored and the previous pose stays. A posed camera uses a 0.10 m near plane;
the far plane is the view distance outdoors. `getPos` / `getOri` on the
camera follow the pose resolved for the last drawn frame (in free-running
play that is the draw-time resolution, the frame `drawnPos` reports in the
next `animate()`), roll included (`getOri().r` is the bank of the
up axis, in the convention of the `roll` command). The roll is also the
camera's `roll` (except on the follow camera bound to the player's car), so
`clearPose` continues with the same horizon. Within 1e-4 (horizontal over length)
of straight up or down, the yaw is taken from the up axis and the roll is 0,
so the written yaw, pitch and roll always rebuild the drawn up. The stock
smoothing (the eye and focus of `move` / `look`, orbit and chase) also starts
from the last drawn pose, so `clearPose` does not snap back to an older view;
on the follow camera bound to the player's car its focus is the car's, as
the follow rule expects. An aimed preview camera's cached eye and focus
follow the pose too.

`GameRef.setMatrix` on a script camera now applies the Ypr roll (as the `roll`
command), except on the follow camera bound to the player's car, whose roll
stays the script's `roll`. The roll applies to a free script camera, to one
attached to a car with a short `move` (for example Track's chase view, whose
stock random roll is back) and to aimed preview cameras. A `setMatrix` with a
NaN or infinite value is rejected whole, roll included.

## CameraProvider and Cameras

A camera mod subclasses `CameraProvider` (an abstract class without natives;
every method has a harmless default) and registers an instance:

```java
public static void modInit()
{
	Cameras.register( new MyCamera() );
}
```

| `CameraProvider` | |
|---|---|
| `String name()`, `int views()`, `String viewName(int v)` | Shown in the options and in the "Camera:" message. |
| `GameRef world` | The world (map) the car drives in, set by Track before `activate`: the parent for the provider's camera. |
| `GameRef camera` | The provider sets it to the camera it renders in `activate`, and back to null in `deactivate`. While the navigator follows the camera (GPS modes 2 and 3), Track follows this camera; when it is null, the car. |
| `void activate(GameRef car, Viewport vp, int view)` | Track has hidden its own camera. Create a `RID_CAMERA` in `world`, render it to `vp`, drive it every frame from an `animate()` callback, starting in `view`. |
| `void deactivate()` | Another camera takes over: destroy that camera. Look back counts as released. |
| `void setView(int view)` | The change-view key chose another view. |
| `void lookBack(int pressed)` | LOOK BACK pressed (1) or released (0). |

| `Cameras` (static) | |
|---|---|
| `register(p)`, `count()`, `get(i)` | The registry; registering the same instance twice does nothing. |
| `selected()`, `select(i)` | The selected provider's index, -1 for the classic cameras. An index out of range selects the classic cameras. |
| `view(p)`, `setView(p, v)` | Each provider's current view. |

The selection and the views last for the session; the mod settings (#488)
persist them by name (`camera.provider`, `camera.<name>.view`).

### How the patched Track uses a provider

- When a provider is selected, it takes the follow view: when the player
  enters a track, and when the selection changes while the follow view or a
  provider is showing (`Track.syncCameraProvider`, from `animate`).
- While LOOK BACK is held, V is ignored, for a provider too.
- When the camera target goes away (null or despawned), the provider is
  deactivated.
- The change-view key (V) in the follow view activates the provider in its
  current view; in the provider's view it moves to the next view (wrapping)
  and calls `setView`. The cockpit and chase views are not in that cycle while
  a provider is selected; with none selected the classic cycle is unchanged.
- LOOK BACK goes to `lookBack` while the provider is active.
- Every other camera change calls `deactivate` first: `changeCamInternal`,
  `changeCamTV`, `changeCamFollow` / `Chase` / `InvChase` / `Point` / `Free`
  (through `makeCamExternal`), `changeCamNone` (also on leaving the track),
  day-race staging in `City` and `ROCTrack.moveCamera`. The provider comes back
  with V. A change of camera target re-activates it on the new target.
- On activation Track hides its follow camera but keeps it alive and bound to
  the car (`cameraMode` is `CAMMODE_PROVIDER`, 8): stock and MWM tracks use the
  `cam` field directly.
- The repair (F6 with cheats), mechanic and paint views drive that `cam` for
  their close-up. Opening one while a provider shows switches back to the
  stock follow camera first (`deactivate`). While any of them is open, V and
  LOOK BACK are ignored and selection changes wait; closing the last one hands
  the follow view back to the selected provider (`activate`), or applies a
  selection made meanwhile.
- Watching a night race in `City` and stopping returns to the provider's view
  when it was showing before.
- The navigator in GPS modes 2 and 3 (follow the camera) follows the
  provider's `camera` field, or the car when it is null.
- Track sets `world` to its map and passes the OSD viewport.

## Units and frames in short

- Chassis frame: x right, y up, z back (the car drives toward -z).
- Yaw 0 faces -z; `Vector3.rotate(new Ypr(yaw, 0, 0))` turns an offset with the car.
- `setPoseLocal` offsets are in the chassis frame; `setPoseRelative` offsets
  are world axes.
- Positions are script metres, the space of `GameRef.getPos` and `setMatrix`.
