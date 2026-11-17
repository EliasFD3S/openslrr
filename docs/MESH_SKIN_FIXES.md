# Mesh skinning and deformed texture handoff (#79, #99)

SCX v4 skin arrays remain empty when backfilling zero preceding vertices.
Previously their emptiness also prevented appending the first skinned vertex,
so the next vertex backfilled zeros for it. Append influences when the current
vertex is skinned OR earlier vertices established skin arrays. Unskinned-only
meshes still leave both arrays empty; mixed meshes retain parallel arrays.

The deformed-mesh cache now transfers texture handles and shared texture paths
under the same guarded condition: a previous mesh entry exists and its texture
list and SCX path match. Previously the missing braces moved shared paths
unconditionally, including dereferencing an end iterator when the previous
entry was missing. Incompatible meshes keep their own texture load path.
This is a correction to existing loader/cache logic, not a new mechanic or a
claim of parity with a separate engine.

## Verification

Release x64 build passes. Four generated SCX fixtures run through the real
loader: skinned-first, unskinned then skinned, unskinned-only, and skinned then
unskinned. Before (main 1de56f0): cases 0 and 3 fail; after: all four pass.
The fixture files are temporary and no game assets are modified.

```powershell
./scripts/run_save_io_tests.ps1 -Suite scx_skin
./scripts/run_save_io_tests.ps1 -Suite scx_skin -BaselineRef 1de56f0
python scripts/check_knowledge_refs.py
```

[Before](../assets/validation/mesh-skin/before.txt) /
[after](../assets/validation/mesh-skin/after.txt). Knowledge references: 219
checked, zero errors. The texture guard is checked by source review and full
renderer compilation, not by a GPU integration test. In-game validation and
screenshots remain pending: inspect articulated parts and replace/damage a
body panel, confirming texture stability and absence of stray vertices.
