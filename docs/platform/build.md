# Build system: issue notes

CMake and generated-header notes, moved from the knowledge base. Build
instructions are in the [README](../../README.md); build-speed edge cases
in [AGENTS.md](../../AGENTS.md#build-speed-and-its-edge-cases).

## Branch package candidates (#480)

[Producer and publication runbook](branch-packages.md). The engine branch and its
reviewed tip identify the build; the independent producer revision on main is not
the package commit. Run `python tests/test_branch_package.py` for the offline contract tests.

Offline validation at engine `09535c9`: Visual Studio 2022 (v14.44, SDK 26100)
produced the x64 Release host. The local ZIP contained 831 overlays in the six
approved roots; every overlay and the engine license matched its Git blob.
The host digest matched the build output. The game was not run.

Notice coverage is intentionally bounded. The Release link inputs include `bimg.lib`;
its generated project compiles astcenc and `image.cpp` calls its decoder. The host
also builds bgfx’s ImGui glue (four embedded fonts and stb) and uses vendored
DirectX and icon headers. SDL3’s static project includes the bundled YUV
conversion code. The packager includes their notices, not licenses for unrelated
examples or build-only tools.

## Version metadata before compilation on every generator (#103)

CMakeLists.txt now defines slrr_version as an always-run custom target whose
byproduct is generated/slrr_version.h. slrr_host depends on that target, and
src/boot/boot.cpp explicitly lists the header as OBJECT_DEPENDS.
The old TARGET PRE_BUILD command behaved as PRE_LINK outside Visual Studio,
so Ninja/Makefiles could compile or reuse objects with the previous version.
Configure-time generation no longer hides that ordering problem on first build.

cmake/write_version.cmake still writes only when the computed content changes:
revision count and short hash follow HEAD, and a dirty checkout adds +. The
version target is checked every build, but an unchanged header does not trigger
compilation/relinking. The window-title consumer rebuilds in the same invocation after a
Git-only metadata change. Paths passed to the generator are quoted.

Validation: tests/test_version_build.py extracts the actual production version
rule into independent Git fixtures with spaces in their paths and compiles a
small main plus a header-consuming boot function at the production source paths. It checks
initial clean build, metadata-only dirty state, new commit without C++ edits,
an unchanged rebuild (header and executable timestamps preserved), and clean
rebuild after generated outputs are removed. Ninja, NMake Makefiles and Visual
Studio 17 2022 all pass. Baseline 9df8920 fails the first dirty-metadata Ninja
rebuild, reporting the previous clean version from the boot consumer. These are
build-graph tests, not a game launch or a full Ninja build of the bgfx project.
The full game's Visual Studio Release target also builds. Native bindings and
runtime gameplay are unchanged. Evidence: assets/validation/version-order.

## CMake dependency paths and toolchain guard (#114)

CMakeLists.txt reads the resolved BGFX_DIR/BIMG_DIR definitions from the
already-added bgfx.cmake directory scope. This follows its actual choice of
explicit paths, in-tree checkouts or FetchContent sources, including custom
FETCHCONTENT_BASE_DIR and FETCHCONTENT_SOURCE_DIR overrides. It does not infer
source paths from the host build directory or overwrite cache preferences.
ImGui sources, shader includes and bimg third-party headers now use the same
checkout as the corresponding libraries. A missing resolved directory fails
configuration with a direct diagnostic.

The guard now explicitly requires Windows x64 and an MSVC-compatible compiler,
matching the existing MSVC flags and documented Visual Studio workflow. MinGW
is rejected rather than allowed through to fail on /utf-8 or /fp:precise. x86,
ARM64 and non-Windows targets remain unsupported. Clang-cl is not separately
validated; the exercised compiler is Visual Studio 2022 MSVC.

Validation: tests/test_cmake_dependency_paths.py configures the real project
in isolated directories containing spaces, offline against existing dependency
sources. Five cases cover default/custom caches, source overrides, in-tree
checkouts and relative explicit directories; generated ImGui/shader/bimg paths
are checked. Five cases execute the actual guard with supported/unsupported
platform/compiler/architecture values. Current: 10/10 pass. Baseline b85dc44: 5/10 pass;
results recorded in assets/validation/cmake-dependency-paths. The guard cases
simulate variables, not installed MinGW/ARM compiler runs. Non-default cases
are configure/generate checks; the ordinary Release x64 host build succeeds.
No game runtime behavior or native registrations change.

Fixtures/logs remain under build/ for inspection. They reference the existing
sources through directory junctions without modifying dependency checkouts.
A tiny fixture-only compile_game_sources.cpp isolates the independent missing
utility file in HEAD; the pre-existing local optional-target edit stays excluded.
