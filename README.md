Validation evidence for the fix of issue #37 (agent branch agent/fix-garage-wheel-collision-f4fd29d6686a). Not product code.

- before-rim-removed.png: baseline 60b143e, garage, stock Baiern (cars/racers/Baiern.rpk:0x6), front-left rim removed.
- after-rim-removed.png: fix branch, same scenario, camera and timing.
- validation-only.patch: the temporary instrumentation used (part_runtime.cpp, env SLRR_VALIDATE_WHEEL*, SLRR_VALIDATE_CAM).
