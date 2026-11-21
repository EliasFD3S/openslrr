# Save JSON schema (version 2)

Implemented 21 November 2026 (#291). Contract: [ENGINE_CONTRACT.md §15](ENGINE_CONTRACT.md#15-saves-and-transitions).
Code: `src/natives/save_json.cpp` (schema, encoder, decoder) and
`src/natives/natives_io.cpp` (files, packs, blobs, conversion).

Every file a script writes through `java.io.File` is saved as JSON. Career,
car and skin files get named, nested objects that follow the stock scripts;
any other file (options, controls, mod data) is a typed `values` list. The
names are meant to map directly to database columns and tables.

## Envelope

```json
{
  "format": "OpenSLRR-save",
  "version": 2,
  "kind": "career",
  "career": { ... },
  "blobs": [{"id": 1, "kind": 7, "base64": "iVBORw0K..."}]
}
```

`kind` is `career`, `vehicle`, `skin` or `raw`; the object under that key (or
`values` for `raw`) holds the data. `blobs` holds embedded dynamic resources
(Painter textures as PNG); values refer to them as `{"blob": id}`.

## Values

| Script value | JSON |
| --- | --- |
| int | integer: `21` |
| float | number with a decimal point or exponent: `515.0`, `0.28709567`; a non-finite float is `{"floatBits": n}` |
| string | string, one character per byte (U+0001..U+00FF); `null` is a null string |
| resource | `{"rpk": "cars/racers/MC.rpk", "id": 6}` (pack path relative to the game, pack-local id), `{"blob": 1}`, or `null` |

In `values` lists a null resource is `{"rpk": null}` to tell it from a null
string. In named fields the type comes from the schema, so an integer written
for a float field (for example by a database export) is accepted.

## Career (`save/carrer/<profile>/main`, `GameLogic.save`)

```text
career
  fileId, fileVersion
  player: character, money, prestige, globalRank, flags, name,
          racesWon, racesLost, hints, winPinkSlips (fileVersion > 20)
  inventory: [ {"part": PART} | {"folder": {name, description, items: [...]}} ]
  decals: [ {texture, stickies} ]
  racers: [ {fileId, seed, prestige, dayCar: VD, nightCar: VD,
             racerSaveVersion, botSaveVersion, nightWins, nightLosses, bestNightQM} ]
  carMarket: {new: [VD | null], used: [VD | null]}
  currentCar, time, day, reserved: [7 ints]

VD (VehicleDescriptor): saveVersion, vehicle, colorIndex, power, optical,
  tear, wear, stockPrestige, fullPrestige, name (v2+), stockQM, fullQM (v3+)
```

## Car (`PlayerCarN`, `Vehicle.save`)

```text
vehicle
  fileId, fileVersion (5)
  chassis: PART
  maxTestTrackSpeedSq, bestTestTrackAcc, bestTestTrackAcc120,
  bestTestTrackTime0, bestTestTrackTime0SpeedSq, bestTestTrackTime1,
  bestTestTrackTime2, bestTestTrackTime2SpeedSq, racesWon, racesLost, bestNightQM

PART (Part.save)
  class        runtime script class (information only, not read back)
  type         part resource; null ends the part
  partVersion, texture, mesh, wear, tear
  slots: [ {slot, damage, attachedSlot, part: PART (when attachedSlot != 0)} ]
  data:  [ {script: "<class>", ...fields} | {script: "<class>", values: [...]} ]
```

`data` holds what part subclasses write after `super.save(File)`, one block
per script class, in order. Stock classes have named fields:

| Script | Fields |
| --- | --- |
| Chassis | saveVersion, wheels, wheelDamage[], brakeBalance, mileage |
| Block | saveVersion, idleRpm, rpmLimit, sparkTimeMin, sparkTimeIncrease, sparkTimeRpm0, sparkTimeRpm1 |
| Camshaft | saveVersion, advance |
| Canister | saveVersion, amount |
| Transmission | saveVersion, gearRatios[8], finalDrive, diffLock, frontDrive |
| FuelInjectorSystem | saveVersion, mixtureRatio, fuelType |
| NOSInjectorSystem | saveVersion, nitroConsumption |
| TurboCharger | saveVersion, wastegatePressure |
| Tyre | saveVersion, inflation |
| ShockAbsorber | saveVersion, damping, reboundFactor |
| Pneumatic_ShockAbsorber | damping, corner, upLength, downLength |

Fields appear only for the `saveVersion` that writes them. A mod class, or a
stock class whose values do not match the table, keeps its values in order
under `values`.

The block boundaries come from script frames: `Jvm::invoke` reports each
script method that receives the open `File` (`set_script_frame_hooks`), so a
value is attributed to the part object and class that wrote or read it.

## Skin (`Vehicle.saveSkin`)

```text
skin: fileId, fileVersion, parts: [ {type, texture} ]
```

## Guarantees and fallbacks

- The named form is written only if decoding it gives back exactly the same
  stream (words, string bytes, resources by pack path and id, blobs).
  Otherwise the whole file is written as `values`.
- Reading accepts keys in any order and ignores unknown keys in named objects;
  missing fields, wrong types, unknown `kind`, a missing blob or a slot number
  of -1 make `File.open` fail instead of loading a shifted stream.
- Version 1 JSON and SDAT type 1/2 are still read. When a script reads such a
  file under `save/` to its last byte (for example `GameLogic.load` or
  `Vehicle.load`), `File.close` rewrites it as version 2. A partial read such
  as `fileCheck`, a read past the end, or a string with inner NUL bytes leaves
  it untouched. Raw host streams without a container are not converted.
- A car in `save/carrer/<profile>/` is converted through its working copy in
  `save/temp/`: it is converted when the car lot loads it and copied back on
  the next career save.
