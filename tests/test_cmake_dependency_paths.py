"""Configure the real project offline with isolated dependency layouts.
Uses existing pinned bgfx/bimg/bx sources read-only; fixtures/logs stay in build/.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_version_build import windows_tools

REPO = Path(__file__).resolve().parents[1]


def junction(destination, source):
    result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(destination), str(source)],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise AssertionError(result.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline-ref')
    for dep in ('bgfx', 'bimg', 'bx'):
        parser.add_argument('--' + dep + '-source', type=Path,
                            default=REPO / 'build/_deps' / (dep + '-src'))
    args = parser.parse_args()
    env = os.environ.copy()
    cmake = windows_tools(env)
    deps = {dep: getattr(args, dep + '_source').resolve() for dep in ('bgfx', 'bimg', 'bx')}
    for dep, path in deps.items():
        if not (path / 'include' / dep / (dep + '.h')).exists():
            raise RuntimeError('Supply existing pinned sources with --' + dep + '-source')
    if args.baseline_ref:
        rule = subprocess.check_output(['git', 'show', args.baseline_ref + ':CMakeLists.txt'], cwd=REPO).decode('utf-8')
    else:
        rule = (REPO / 'CMakeLists.txt').read_text(encoding='utf-8')
    root = Path(tempfile.mkdtemp(prefix='cmake114 fixtures ', dir=REPO / 'build'))
    failures = 0

    def run(command, log):
        result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        log.write_text(result.stdout, encoding='utf-8')
        return result

    # Execute the exact production guard, independently of installed compilers.
    start = rule.index('if(NOT WIN32')
    guard = rule[start:rule.index('endif()', start) + len('endif()')]
    for name, win, msvc, size, arch, accepted in (
        ('msvc-x64', True, True, 8, 'x64', True),
        ('mingw-x64', True, False, 8, '', False),
        ('msvc-arm64', True, True, 8, 'ARM64', False),
        ('msvc-x86', True, True, 4, 'X86', False),
        ('non-windows', False, False, 8, '', False),
    ):
        script = root / (name + '.cmake')
        script.write_text(f'set(WIN32 {int(win)})\nset(MSVC {int(msvc)})\n'
                          f'set(CMAKE_SIZEOF_VOID_P {size})\n'
                          f'set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "{arch}")\n' + guard, encoding='utf-8')
        result = run([cmake, '-P', str(script)], root / (name + '.log'))
        ok = (result.returncode == 0) == accepted
        failures += not ok
        print(('PASS' if ok else 'FAIL'), 'guard', name)

    for case in ('default-cache', 'custom-cache', 'source-overrides', 'in-tree', 'relative-explicit'):
        source = root / case / 'source with spaces'
        binary = root / case / 'build with spaces'
        source.mkdir(parents=True)
        (source / 'CMakeLists.txt').write_text(rule, encoding='utf-8')
        for directory in ('src', 'include', 'shaders', 'cmake'):
            junction(source / directory, REPO / directory)
        shutil.copyfile(REPO / 'slrr_host.rc', source / 'slrr_host.rc')
        (source / 'tools').mkdir()
        # Isolate the unrelated pre-existing missing optional utility in HEAD.
        (source / 'tools/compile_game_sources.cpp').write_text('int main(){return 0;}\n')
        (source / 'third_party').mkdir()
        junction(source / 'third_party/slrrc', REPO / 'third_party/slrrc')
        wrapper = source / 'third_party/bgfx.cmake'
        shutil.copytree(REPO / 'third_party/bgfx.cmake', wrapper,
                        ignore=lambda directory,names: [name for name in names if name == '.git' or
                            (Path(directory) == REPO / 'third_party/bgfx.cmake' and name in deps)])
        options = ['-DFETCHCONTENT_FULLY_DISCONNECTED=ON']
        expected = {}
        for dep, path in deps.items():
            if case in ('default-cache', 'custom-cache'):
                cache = binary / '_deps' if case == 'default-cache' else root / case / 'shared cache'
                cache.mkdir(parents=True, exist_ok=True)
                junction(cache / (dep + '-src'), path)
                expected[dep] = cache / (dep + '-src')
                if case == 'custom-cache' and dep == 'bgfx':
                    options.append('-DFETCHCONTENT_BASE_DIR=' + str(cache))
            elif case == 'source-overrides':
                options.append('-DFETCHCONTENT_SOURCE_DIR_' + dep.upper() + '=' + str(path))
                expected[dep] = path
            elif case == 'in-tree':
                junction(wrapper / dep, path)
                expected[dep] = wrapper / dep
            else:
                options.append('-D' + dep.upper() + '_DIR=' + os.path.relpath(path, wrapper))
                expected[dep] = path
        result = run([cmake, '-S', str(source), '-B', str(binary), '-G',
                      'Visual Studio 17 2022', '-A', 'x64'] + options,
                     root / (case + '.log'))
        ok = result.returncode == 0
        if ok:
            project = (binary / 'slrr_host.vcxproj').read_text(encoding='utf-8-sig').replace('\\', '/')
            bgfx = expected['bgfx'].as_posix()
            bimg = expected['bimg'].as_posix()
            ok = all(path in project for path in (
                bgfx + '/3rdparty/dear-imgui/imgui.cpp', bgfx + '/src', bimg + '/3rdparty'))
        failures += not ok
        print(('PASS' if ok else 'FAIL'), 'configure/generated paths', case)
    print(f'{10 - failures}/10 passed; fixtures: {root}')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
