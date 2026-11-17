# Save persistence fixes (#69, #85, #98)

File.close now writes to a uniquely created sibling file, checks every write,
flushes and closes it, then publishes it with MoveFileExW replacement and
write-through. Failure preserves an existing destination and retains the newest
bytes in the existing process-local fallback buffer. File.exists, copy and move
now agree with reads about these pending bytes. Successful disk transfers clear
stale destination buffers. Failed transfers retain their source buffer.

This is single-file publication, not a multi-file career transaction. Pending
buffers are lost when the process exits. Java code which deletes a save before
writing it still needs a separate transaction fix; this does not resolve #59.

File.write(String) stores null as length zero and a non-null empty string as
length one plus NUL. This agrees with the host reader and SLRR_rev's
[src/runtime/memory-file.ts](https://github.com/nedobylskiy/SLRR_rev/blob/main/src/runtime/memory-file.ts)
and [file-native.ts](https://github.com/nedobylskiy/SLRR_rev/blob/main/src/runtime/file-native.ts).
This reference is not a new verification against the original MWM executable.

Part.getSlotDamage returns null for absent damage and invalid slots without
allocating empty strings. The stock Part.java save loop uses the null sentinel
to decide whether a damage record needs serialization. Actual damage text and
set/clear behavior are preserved. No Java changes are needed.

## Validation

Run from the repository in PowerShell after building Release:

```powershell
./scripts/run_save_io_tests.ps1
./scripts/run_save_io_tests.ps1 -Suite save_persistence
./scripts/run_save_io_tests.ps1 -Suite save_persistence -BaselineRef 71aec8b
python scripts/check_knowledge_refs.py
```

The original save I/O suite passes all 10 cases. The new persistence suite
passes all 10 cases; compiling the same cases against main 71aec8b produces
6 failures and 4 passes. Tests cover locked publication, pending existence,
copy/move and failed-copy behavior, stale destination buffers, empty/null string
wire format and alignment, and absent/set/cleared slot damage. The baseline
passes some transfer cases because direct truncation bypasses the intended
replacement lock; those are forward regression checks for the new fallback.

Tests use isolated generated fixtures under build/save-io-fixtures and a minimal
RPK/CFG, never player saves. The runner links the actual Release host objects.
[Before](../assets/validation/save-persistence/before.txt) and
[after](../assets/validation/save-persistence/after.txt) record the results.
Screenshots add no evidence for these nonvisual changes.

Release x64 builds successfully. Knowledge references: 219 checked, zero errors.
In-game career save/load validation remains pending.
