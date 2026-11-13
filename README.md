Validation evidence for the fix of issue #37 (agent branch agent/fix-garage-wheel-collision-f4fd29d6686a). Not product code.

Garage, front-left rim removed, same camera, timing and window size per car:
- before-rim-removed.png / after-rim-removed.png: stock Baiern (cars/racers/Baiern.rpk:0x6). Baseline 60b143e vs fix.
- superduty-before-rim-removed.png / superduty-after-rim-removed.png: Hauler's SuperDuty (cars/racers/SuperDuty.rpk:0x6).
- superduty-after-rear-rim-removed.png: SuperDuty with a rear inner rim removed; the outer dually wheel carries the corner.
- validation-only.patch: the temporary instrumentation (part_runtime.cpp, env SLRR_VALIDATE_WHEEL*, SLRR_VALIDATE_CAM).
