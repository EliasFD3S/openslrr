"""Exercise the production version CMake rule with real incremental builds.

Runs isolated Git fixtures with spaces in their paths. On Windows, discovers
VS CMake/Ninja, MSVC and the Windows SDK without changing the caller environment.
Use --baseline-ref REV to reproduce the old rule. No game assets are required.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]


def windows_tools(env):
    vswhere = Path(os.environ["ProgramFiles(x86)"]) / "Microsoft Visual Studio/Installer/vswhere.exe"
    vs = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip())
    tool_version = (vs / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt").read_text().strip()
    tools = vs / "VC/Tools/MSVC" / tool_version
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows Kits\Installed Roots") as key:
        sdk = Path(winreg.QueryValueEx(key, "KitsRoot10")[0])
    version = max((p.name for p in (sdk / "Include").iterdir() if (p / "um/Windows.h").exists()),
                  key=lambda name: tuple(map(int, name.split("."))))
    env["INCLUDE"] = os.pathsep.join(map(str, [tools / "include", sdk / "Include" / version / "ucrt",
        sdk / "Include" / version / "shared", sdk / "Include" / version / "um"]))
    env["LIB"] = os.pathsep.join(map(str, [tools / "lib/x64", sdk / "Lib" / version / "ucrt/x64",
        sdk / "Lib" / version / "um/x64"]))
    cmake_root = vs / "Common7/IDE/CommonExtensions/Microsoft/CMake"
    env["PATH"] = os.pathsep.join(map(str, [tools / "bin/Hostx64/x64", sdk / "bin" / version / "x64",
        cmake_root / "Ninja", Path(env.get("SystemRoot", r"C:\Windows")) / "System32"])) + os.pathsep + env["PATH"]
    return str(cmake_root / "CMake/bin/cmake.exe")


def run_case(generator, cmake, env, rule_source):
    build_root = REPO / "build"
    build_root.mkdir(exist_ok=True)
    # Leave this isolated fixture in build/ for inspecting failures; never modify
    # the real checkout's commits, working tree, generated header or game data.
    fixture = Path(tempfile.mkdtemp(prefix="version fixture ", dir=build_root))
    source = fixture / "source with spaces"
    source.mkdir()
    binary = fixture / "build with spaces"
    git = [shutil.which("git"), "-c", "safe.directory=" + str(source)]

    def command(args):
        result = subprocess.run(args, cwd=source, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise AssertionError(f"{args}:\n{result.stdout}")
        return result.stdout.strip()

    start = rule_source.index("set(SLRR_VERSION_H")
    end = rule_source.index("target_include_directories(slrr_host", start)
    # Use the actual production rule, including configure-time execution on the
    # baseline. This is not a second implementation of the dependency graph.
    rule = rule_source[start:end]
    (source / "cmake").mkdir()
    shutil.copyfile(REPO / "cmake/write_version.cmake", source / "cmake/write_version.cmake")
    (source / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.21)\nproject(version_probe LANGUAGES CXX)\n'
        'add_executable(slrr_host src/main.cpp src/boot/boot.cpp)\n' + rule +
        '\ntarget_include_directories(slrr_host PRIVATE "${CMAKE_BINARY_DIR}/generated")\n', encoding="utf-8")
    (source / "src/boot").mkdir(parents=True)
    (source / "src/main.cpp").write_text('#include <cstdio>\n'
        'const char* boot_version();\n'
        'int main() { std::puts(boot_version()); }\n', encoding="utf-8")
    (source / "src/boot/boot.cpp").write_text('#include "slrr_version.h"\n'
        'const char* boot_version() { return SLRR_VERSION; }\n', encoding="utf-8")
    note = source / "metadata.txt"
    note.write_text("initial\n", encoding="utf-8")
    command(git + ["init"])
    command(git + ["config", "user.name", "Version regression test"])
    command(git + ["config", "user.email", "version-test@example.invalid"])
    command(git + ["config", "commit.gpgsign", "false"])
    command(git + ["config", "core.hooksPath", str(source / "no-hooks")])
    command(git + ["add", "."])
    command(git + ["commit", "-m", "initial fixture"])
    configure = [cmake, "-S", str(source), "-B", str(binary), "-G", generator]
    if generator.startswith("Visual Studio"):
        configure += ["-A", "x64"]
    command(configure)
    exe_name = "slrr_host.exe" if os.name == "nt" else "slrr_host"
    exe = binary / ("Release/" + exe_name if generator.startswith("Visual Studio") else exe_name)
    header = binary / "generated/slrr_version.h"

    def build_check(label, dirty=False):
        command([cmake, "--build", str(binary), "--config", "Release", "--target", "slrr_host"])
        expected = "r" + command(git + ["rev-list", "--count", "HEAD"]) + "-" + command(git + ["rev-parse", "--short=7", "HEAD"]) + ("+" if dirty else "")
        actual = command([str(exe)])
        assert actual == expected, f"{generator} {label}: binary reports {actual}, expected {expected}"
        print(f"PASS {generator}: {label} -> boot {expected}", flush=True)

    build_check("clean initial build")
    note.write_text("dirty metadata only\n", encoding="utf-8")
    build_check("dirty metadata first rebuild", True)
    command(git + ["add", "metadata.txt"])
    command(git + ["commit", "-m", "change metadata without touching C++"])
    build_check("new commit first rebuild")
    timestamps = (header.stat().st_mtime_ns, exe.stat().st_mtime_ns)
    build_check("unchanged rebuild")
    assert timestamps == (header.stat().st_mtime_ns, exe.stat().st_mtime_ns), "unchanged build rewrote/relinked output"
    print(f"PASS {generator}: unchanged header and executable timestamps", flush=True)
    command([cmake, "--build", str(binary), "--config", "Release", "--target", "clean"])
    build_check("clean rebuild restores generated header")
    return fixture


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref")
    parser.add_argument("--generator", action="append")
    args = parser.parse_args()
    env = dict(os.environ)
    cmake = windows_tools(env) if os.name == "nt" else shutil.which("cmake")
    if args.baseline_ref:
        rule_source = subprocess.check_output(["git", "-c", "safe.directory=" + str(REPO),
            "show", args.baseline_ref + ":CMakeLists.txt"], cwd=REPO).decode("utf-8")
    else:
        rule_source = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
    generators = args.generator or (["Ninja", "NMake Makefiles", "Visual Studio 17 2022"] if os.name == "nt" else ["Ninja", "Unix Makefiles"])
    for generator in generators:
        fixture = run_case(generator, cmake, env, rule_source)
        print(f"Evidence fixture: {fixture}", flush=True)
