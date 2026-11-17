# File/save I/O fixes — issues #68, #70 and #71

Baseline: `17115433f8aaa2e6a0bcce970c25afdd7dc01ce0` (GitHub main, 16 November 2026).
The changes are engine-side native fixes. They require no Java patch or save-format migration.

## Changes and contracts

### #68 — File.delete must not recursively erase the installation

`File.delete(".")` previously resolved to the game root and passed it to
`std::filesystem::remove_all`. A nonempty career directory was recursively
removed too. The shared `under_game` helper now rejects a canonical target equal
to the root, and compares complete path components after canonicalization to
reject resolved escapes. `File.delete` uses `remove`, so it deletes a file or
an empty directory and fails on a nonempty directory. The existing successful
return for a missing path is preserved.

The stock `File.delete(path, mask)` wrapper enumerates names and invokes the
single-path native. Stock menu code deletes directory contents before deleting
the directory itself. This flow still works. No save-only allowlist was added:
other legitimate game-relative paths remain available. FindFile's separate
path handling is unchanged and remains an open risk.

### #71 — Two dots inside a career name are not a parent-directory component

The old substring check rejected `J..R-1` and `Mr...-1` along with `..`.
The validator now rejects components equal to `..`, while accepting ordinary
names containing dots. Explicit parent traversal, drive/alternate-stream
syntax, embedded NULs, the canonical root and resolved escapes are rejected.
Both forward and backslash separators are handled.

### #70 — Unrepresentable resources must not shift the rest of a save

An SDAT ResourceRef is a four-byte field. If the resource cannot be encoded,
the native now writes a four-byte null placeholder, increments the file's
`degraded_resources` count and logs the affected ID. It still returns failure
(0), so callers checking the result can detect the loss. Close logs the
number of placeholders. Later integers, strings and resources retain their
correct offsets even when stock Java ignores the failure return.

Valid and null SDAT references keep their existing representation. Raw files
keep their original IDs. This does not recover the missing resource itself.
The previous File.close durability/atomicity issue (#69) is not addressed.

## Reproducible validation

Build the Release host first, then run from the repository root:

```powershell
cmake --build build --config Release --target slrr_host --parallel 8
.\scripts\run_save_io_tests.ps1
.\scripts\run_save_io_tests.ps1 -BaselineRef 17115433f8aaa2e6a0bcce970c25afdd7dc01ce0
```

The runner discovers the installed MSVC/Windows SDK, links the existing Release
host objects except main with `tests/test_save_io.cpp`, and invokes the actual
native registry. It does not stub file operations, SDAT serialization, the
path validator, or the native return values. It opens no game window.

The baseline run substitutes the original `natives_boot.cpp` and
`natives_io.cpp` object files, extracted read-only from the specified Git
revision. It never checks out or reverts working files. Exit 1 is expected for
the baseline and exit 0 for the corrected build.

All file writes/deletes happen under process-specific directories in
`build/save-io-fixtures/`; the game root used by each fixture is that isolated
directory. Tests use no original game assets, player profiles or real saves.
The minimal VM has no classpath: `load_class: classpath not ready` messages are
expected when readString allocates the fixture's String objects. They do not
indicate a failure to load game data.

| Case | Baseline | Corrected |
| --- | --- | --- |
| Root deletion/write refusal | FAIL | PASS |
| Nonempty directory deletion refusal | FAIL | PASS |
| Stock delete-contents-then-directory and missing-path behavior | PASS | PASS |
| Dots within names versus parent traversal | FAIL | PASS |
| Dotted career name write/read/delete round trip | FAIL | PASS |
| Unknown-pack resource followed by integer/string fields | FAIL | PASS |
| Packless resource followed by integer/string fields | FAIL | PASS |
| Unsupported dynamic-blob resource followed by integer/string fields | FAIL | PASS |
| Valid and null resource round trip | PASS | PASS |
| Raw resource ID round trip | PASS | PASS |
| **Total** | **3 passed / 7 failed** | **10 passed / 0 failed** |

Recorded summary: [before](../assets/validation/save-io/before.txt) and
[after](../assets/validation/save-io/after.txt). Full local logs are written to
`build/save-io-tests/`. Screenshots are not relevant to these nonvisual fixes.

The Release x64 host builds successfully. Knowledge-reference checks are run
separately. These checks establish the named native/file regressions; they
are not an in-game validation of the complete career save/load flow.
