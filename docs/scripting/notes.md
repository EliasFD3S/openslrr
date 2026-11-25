# VM and script runtime: issue notes

Verified notes on TREE evaluation, class loading, TUFA validation, native
lookup and GameRef handles, moved from the knowledge base. Garbage
collection is in [vm-gc.md](vm-gc.md); native coverage in
[natives.md](natives.md).

## Issues #72 / #126: exact TREE integers and integer balances

`src/jvm/tree_eval.cpp:arithmetic` now handles integer operands without
converting through float. Addition, subtraction and multiplication wrap
explicitly at 32 bits; division handles INT_MIN / -1 without a host trap
and remainder handles that pair as zero. The existing divide/remainder
by zero -> zero policy is retained. Both expression dispatchers and
compound arithmetic use the same operation; unary negation and increment/
decrement also avoid signed overflow. Mixed operands still promote to float.
The deliberate MWM float-inequality behaviour remains unchanged.

`src/jvm/tree_eval.cpp:java_f2i` defines NaN -> zero, saturation at integer
limits and truncation otherwise for TREE conversions (locals, casts, fields,
array lengths and indices). Primitive casts clear the completed expression
and class carrier, avoiding the previous value-without-binop failure.
Integer field/local narrowing on mixed compound assignment is preserved.
These are general VM rules: no special case for Racer.money or the ROC.
Stock Racer.money and GameLogic.ROC_ENTRYFEE are both int; the fee is
100000. The tests confirm exact fee subtraction even for odd balances above
2^24, and integer storage after a fractional fee. They do not prove why a
particular historical ROC session went negative. A script spending more
than the balance still gets a negative integer; this change adds no clamp
or entry-eligibility rule. Keep that part of #126 subject to game validation.

Validation: Release host builds; `tree_numbers` compiles a Java fixture to
TUFA, loads it in the VM and checks exact arithmetic, compound operators,
wrapping, casts including infinities/NaN, fee storage and mixed floats.
Handcrafted TREE ordering additionally exercises the value dispatcher for
all five arithmetic tags. The INT_MIN divide/remainder cases run after
other checks pass. The same suite against pre-fix origin/main fails the
precision/fee/compound/cast cases; dangerous division cases are skipped
when earlier assertions fail. The existing save_io suite also passes. Logs
are in `assets/validation/tree-numbers/`. No production Java is modified;
the generated test Java lives only in an isolated temporary fixture.
SLRR_rev's source tree and slrr-reverse TREE evaluator sources were
consulted; no third-party numeric implementation was copied.

Previous vehicle follow-up: the user validated the #132/#116 correction and
authorized merging PR #143; it is merged and both issues are closed.

## Static increment/decrement writeback and career days (#63)

TREE previously computed ++/-- but only wrote back locals, instance fields or
array elements. Static field reads now retain their actual owner/name through
both direct/quick field opcodes and resolved name paths. Statement and
expression ++/-- use a shared field-writeback helper; postfix retains the old
expression result and prefix exposes the stepped value. Instance reads clear
the static target so an earlier static access cannot redirect a later write.
Integer stepping keeps 32-bit wrap and floats remain floats. No GameLogic/day
special case, production Java change or new native is involved.

The actual shipped GameLogic.spendTime TREE now advances day when time crosses
its existing strict `> 24*3600` condition. The exact-midnight boundary is not
changed; a positive step beyond it advances the day. Multi-day steps increment
once per wrap and still notify timeRefresher and call System.syncGameTime.

Validation: static_increment has 0/2 passing on baseline 4de3d3c and 2/2 on the
fix. A compiled generic fixture covers statement/expression pre/post ++/--,
float fields, uninitialized statics, foreign owners, both 0x1b/0x1c encodings,
multiple increments in one expression, int overflow and local/instance
regressions. A TreeHost harness executes the actual GameLogic.class method,
checking exact midnight, single/multiple day crossings and expected notification/
clock-sync calls. It isolates the VM method from the rest of the career; full
career persistence and header display require in-game validation. Evidence:
`assets/validation/static-increment/before.txt` and
`assets/validation/static-increment/after.txt`.

Release build, tree_numbers, class_lifetime and save_io pass. Native coverage
--check remains current; stock registration counts are unchanged.

## Immutable loaded classes and reference overloads (#73, #66)

`Jvm::load_class_file` caches successfully loaded absolute, lexically normalized
paths (case-folded on Windows). Failed/empty loads are not cached. A new file
may introduce new classes, including siblings concatenated with an existing
class; exact loaded class names are never overwritten or reinitialized. Class
storage is published before its literal/static enrichment, so recursive class
queries see the existing definition. Live methods, TREE vectors, statics and
RID initializer metadata remain valid for the VM lifetime. Changed files,
including new blobs appended to a previously loaded path, require restart.
A fresh VM sees the new disk contents. No live hot-reload API is introduced.

`find_method_by_name` scores null as a reference instead of tying primitive
arguments. Primitive arrays are reference parameters as well. This fixes the
ordinary this(null)/this(parent) overload chain without recognizing Dummy or
any particular mod class. Numeric conversion scoring is retained. Selection
among multiple equally scored reference overloads remains declaration-order;
this does not implement full source-language overload inference.

Validation: baseline dede4b5 fails all three class_lifetime scenarios; current
code passes them. Coverage includes repeated/normalized-path load with mutable
statics, reload inside an executing native callback/TREE frame, new concatenated
containers with an existing class and a new sibling, failed-load retry and an
already-loaded file later corrupted on disk. Constructor fixtures cover four
Dummy-shaped overloads, null/int selection and primitive-array parameters.
The actual game Dummy.class is also executed with production GameType natives;
all four constructors produce native roots and preserve parent/flags. This is
not a full career, catalog or suspended-fiber gameplay run. The minimal test
pack setup logs unrelated frontend RID misses. Evidence:
`assets/validation/class-lifetime/before.txt` and
`assets/validation/class-lifetime/after.txt`.

Release build, vm_lookup, tree_numbers, save_io and TUFA corpus checks pass.
The corpus is unchanged: 1,501 files, 1,546 accepted classes and the existing
Dialog.class CONS-kind-2 rejection. vm_lookup now checks immutable definitions
in the running VM and refreshed metadata in a fresh VM. No native signatures
or production Java changed. Static ++ behavior is covered separately by #63 below; the constructor
counter fixture uses explicit addition to isolate constructor dispatch.

## Issues #88 / #89 / #74: reject malformed TUFA before execution

`src/jvm/tufa.cpp` bounds collection counts by remaining bytes before
reserving storage: constants >=8 bytes, field/method records 16/20, trees
4, nodes 3, interfaces 4. File sizing, allocation and reads are inside the
load exception boundary; failed tellg is rejected. Parser failure clears
all previously parsed blobs, so a malformed tail cannot publish a valid
prefix from a concatenated class container. Null nonempty input is rejected.

`parse_one_blob` validates pool links and the constant-bearing TREE opcodes
before the VM seeds static fields. UTF8, Class, Resource and reference
carriers must match the operation, while reference() still accepts the
existing bare UTF8/NameAndType compatibility forms. This prevents malformed
string/RID initializers and lazy RID fixups from encountering bad constants
after class publication. Static enrichment also catches TREE exceptions.
No unsupported constant kinds or new opcode semantics are introduced.

`kTufaMaxLocals` is 65536: method local counts and local load/declaration
slots are checked by the parser. TREE evaluation additionally bounds method
locals/arguments and declarations before allocating or indexing; size_t
arithmetic avoids imm+1 wrapping. These runtime checks also protect
in-memory TREE callers that bypass file parsing.

Validation: Release host compiles. The tufa_validation suite compiles a
valid two-class TUFA container, mutates collection counts, method-local
counts, three extreme declaration slots, static string/RID constants and
the concatenated tail, and checks clean parser/VM rejection. It also checks
the runtime declaration/local-count guards directly and executes the valid
fixture after a rejected file. No production Java is modified.

`-Suite tufa_validation -CorpusOnly` scans actual game classes; with
`-BaselineRef origin/main` it substitutes only the baseline parser. Both
parsers accept 1546 classes from the same 1501 files, rejecting only the
pre-existing `game/system/Scripts/render/osd/Dialog.class` (CONS kind 2).
This wave adds no new corpus rejection. Corpus comparison deliberately
does not run huge-count malformed fixtures against the unsafe parser.
Logs: `assets/validation/tufa-validation/`. Existing tree_numbers and
save_io suites pass. SLRR_rev `SLRR_rev:docs/formats/tufa.md` and
`SLRR_rev:src/formats/tufa.ts` were consulted for container/record contracts;
validation is implemented locally, without copied code.

PR #146 was validated and merged at the user's request; #107/#94 are closed.
For this wave, confirm menu/options, career loading, garage/city and catalog
behaviour in game before merge; parser corpus compatibility is separate
from gameplay validation. RPK TOC count validation remains a separate risk.

## VM lookup costs (#81, #111)

`NativeRegistry::has` and `NativeRegistry::invoke` perform an exact hash lookup,
then at most one alternate lookup for the omitted void return (`(...)` /
`(...)V`). They no longer scan all registered natives on a miss. Exact
registrations retain priority when both spellings are registered; duplicate
exact signatures still throw. Return types and class aliases are not widened.
Late registrations work without negative-cache invalidation.

`Jvm::seed_static_fields` records eligible short-tree RID initializer metadata
once per class load in `static_rids_`. `Jvm::get_static` consults this index
instead of rescanning fields and trees for every small integer. Values remain
mutable; an unresolved local RID is retried against the active resource index,
and loaded definitions and metadata stay immutable until restart (see #73/#66 below).

Validation: Release build; `vm_lookup` before/after compatibility tests cover
void aliases, exact priority, missing/duplicate natives, late registration,
mutable statics, unresolved-to-resolved RIDs and recompile/restart separation. The isolated
100,000-miss / 4,000-registration benchmark measured about 1.43 s before and
7 ms after on this machine; this is not an in-game FPS claim. `tree_numbers`
passes. TUFA corpus remains 1,501 files / 1,546 accepted classes, with the same
pre-existing Dialog.class kind-2 rejection. Evidence:
`assets/validation/vm-lookup/before.txt` and
`assets/validation/vm-lookup/after.txt`. No production Java or native API added.

## Script input safety (#87, #90)

Native adapters now use checked `native_arg_number`, `native_arg_integer` and
`native_arg_object` helpers (`src/natives/natives_internal.hpp`). Missing or
mistagged values use explicit fallbacks; numeric arguments accept Int/Float,
non-finite floats use the fallback, and float-to-int conversions saturate.
Options getter/setter, display-mode lookup/change, camera priority, time strings
and System.exit(String) no longer read unchecked argument slots/union members.
Incomplete or object-valued renderer-option setters leave settings unchanged.
Text.setPos and RenderRef.setMatrix accept integer coordinates as well as floats.
No native signatures or production Java changed.

TREE direct and compound array writes share `store_array_slot`
(`src/jvm/tree_eval.cpp:store_array_slot`). Existing auto-growth is preserved
below 1,048,576 slots, matching the new-array cap. Negative indices and indices
at or above that limit are ignored, leaving slots/length unchanged; the first
rejection is logged once per process. Compound growth now synchronizes length
as direct growth already did. This is a compatibility limit, not Java bounds
exception semantics. System.arraycopy keeps its separately documented policy;
this limit does not establish a total VM memory budget.

Validation: Release build and `script_safety`, `tree_numbers`, `save_io` suites
pass. The new suite compiles a Java fixture and exercises direct/compound
stores, the last allowed slot, negative/cap/INT32_MAX indices, mutable options,
short/mistagged native calls, Float camera priorities and time flags, Int text
and matrix coordinates, and System.exit with an Int argument. Unsafe old array
behavior was not executed with huge indices. Evidence:
`assets/validation/script-safety/after.txt`. In-game validation remains separate.

## GameRef aliases: events and timers (#93)

`src/resources/gameref.cpp:gameref_set_event_mask` and
`src/resources/gameref.cpp:gameref_add_timer` resolve the canonical node before
creating a new GameType state. Alias writes now reach the same event mask and
timer queue used by reads, clear/remove operations and timer dispatch. Existing
canonical kinds are preserved; unregistered objects still create GameType state.
No Java changes or scene-specific exceptions. This fixes state routing, not the
scheduler itself. Stock paths mostly use direct objects, so the main impact is
aliased native instances and mod scripts.

Release x64 compilation succeeds. Three deterministic GameRef tests cover mask
set/clear and kind preservation, timer canonical destination/one-shot/removal,
and unregistered-object creation. Main 784a760: 1 pass / 2 fail; corrected:
3 pass / 0 fail. Run `./scripts/run_save_io_tests.ps1 -Suite gameref_alias`,
or add `-BaselineRef 784a760` to reproduce. In-game validation remains pending;
these tests exercise the actual state primitives but not Java event dispatch.

PR #121 integration update (2026-11-17): conflicts were limited to this
knowledge base and the test runner; current main's suites and fixes are kept.
Release x64 rebuild succeeds. Against main 4e4b782, gameref_alias still reports
1 pass / 2 fail; the integrated branch reports 3 pass / 0 fail. live_lookup and
trigger_markers pass too. The trigger animation regression now samples two
bob heights and both yaw basis components, avoiding equal-value samples at
animation extrema, and reports caught exceptions instead of aborting silently.
These remain automated host checks; no additional in-game validation is claimed.
Evidence: assets/validation/gameref-alias/integration.txt.

OSD text lines share one bounded glyph batcher and an unlit shader, avoiding per-glyph draws/world-light bindings: `src/render/bgfx_osd.cpp:draw_osd_line`. Measured draw counts and pixel comparison are in [scripting/runtime.md](runtime.md#batched-osd-text-82).

## Live GameRef lookup (#106)

`gameref_find_live` now uses a live-ID hash index rather than scanning g_refs.
The index points at stable unordered_map elements and is erased before their
states are removed. ID allocation is monotonic until clear, without redundant
collision scans or signed overflow; exhaustion returns 0. IDs of entries that
become aliases remain dormant and are filtered at lookup, preserving wrapper
unbind semantics. Alias deletion, canonical deletion and clear remove indexed
entries. This index does not add Java GC roots or change ID ownership.

`prepare_batch` resolves the controlled chassis and validates its script
frame once before the batch. Loose panels can have different owner IDs;
their per-panel lookups remain, now indexed. Seated-driver world traversal
(#112) is unchanged and stays open; caching its results requires reliable
invalidation of all relevant scene changes.

Release build, live_lookup, physics_position and ped_crossing pass. The same
lifetime/alias fixture passes with baseline a15ab7b and current code, covering
stable addresses after rehash, dormant aliases, alias/canonical removal, reused
keys and clear/reset. Isolated 100,000 lookups / 8,000 nodes measured about
2.71 s before and 0.81 ms after; not an in-game FPS measurement. Evidence:
`assets/validation/live-lookup/before.txt` and
`assets/validation/live-lookup/after.txt`. Gameplay validation is separate.

## Chassis.getMass is the assembled car (#413)

`Chassis.getMass` was bound to the same lambda as `Part.getMass`, so it
returned the `_main_*.cfg` body alone: 838.5 kg for a Baiern CoupeSport and
757 kg for every Focer, instead of the assembled car. The physics already
used the whole installed tree. `CarInfo` showed the short mass and a kg/HP
near half the real one (Focer WRC 2.58), and the catalog delivery price
(`chassis.getMass()*5.8`) was low.

The executable binds `Chassis.getMass` at 0x0043CAA0: from the chassis it
reaches the car's vehicle object and returns `1 / inverse mass` of the
vehicle body (`[vehicle+0x13BC]`). `Part.getMass` (0x00469290) returns the
part's own body mass. The host now returns `assembly_mass_center` for the
chassis, the sum the physics body is built from, and keeps `Part.getMass`
as the part's CFG mass. A chassis outside any car reports the sum of its own
installed subtree; the executable may report 0 there (not checked).

Validation: `swaybars` test `chassis_mass_is_the_assembled_car` on the stock
MWM game: Baiern CoupeSport body 838.5 kg, `Chassis.getMass` 1523.6 kg,
equal to the `Part.getMass` sum and to the released physics body. Before
the fix the same test reads 838.5 kg and fails. A VM probe reads Focer
RC 200 / RC 300 / WRC at 1475.3 / 1492.9 / 1395.4 kg (757 kg before), so the
WRC's kg/HP goes from 2.58 to about 4.76. Not checked in game.
