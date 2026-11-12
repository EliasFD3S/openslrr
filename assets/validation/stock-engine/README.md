# Stock engine compression validation

Validated the PR fix at revision `34c8a008fcb7ea5c42ff6f44551e66ba14f1f0b4` on Windows x64 with Visual Studio 2022.

## Automated checks

The complete native CTest suite passed: **8/8 targets**, including the new `slrr_tree_eval_tests` and the existing `slrr_vm_dyno_tests`.

## Real-game checks

The corrected `slrr_host.exe` was launched against a separate test game installation with each fully stock vehicle and the Car Information screen opened (`--spawn-car ... --auto-cmd 115`). These are direct stock-spawn checks, not a dealer purchase-flow walkthrough.

| Vehicle | Stock assembly | Static compression | Result |
| --- | ---: | ---: | --- |
| Hauler's SuperDuty 500 | 84 parts | **7.9:1** | Full engine and dyno information is displayed; no compression error |
| MC GT | 87 parts | **9.0:1** | Full engine and dyno information is displayed; no compression error |

- [SuperDuty Engine Information](after-34c8a00-SuperDuty.png)
- [MC GT Engine Information](after-34c8a00-MC-GT.png)

The baseline omitted `tools/compile_game_sources.cpp`. A placeholder for that unused helper target was present only in the local validation worktree so the host and tests could be built; it is not part of the PR.
