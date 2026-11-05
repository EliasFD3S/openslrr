param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build-bgfx'),
    [string]$GameRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) 'game')
)

$ErrorActionPreference = 'Stop'
$workspace = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path -LiteralPath (Join-Path $GameRoot 'system/Scripts/lang/System.class'))) {
    throw 'Original game assets are required: integration tests must not be skipped.'
}
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else {
    'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$ctest = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
$configure = @('-S', $PSScriptRoot, '-B', $BuildDirectory,
    '-DSLRR_STAGE_BUILD=ON', '-DSLRR_BUILD_TESTS=ON', "-DSLRR_TEST_GAME_ROOT=$GameRoot")
if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt'))) {
    $configure += @('-A', 'Win32')
}
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed; release unchanged.' }
& $cmake --build $BuildDirectory --config Release --target host_runtime_test ui_state_test scene_instances_test part_config_test vehicle_dynamics_test --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed; release unchanged.' }
$testLog = Join-Path $BuildDirectory 'validated-ctest.log'
& $ctest --test-dir $BuildDirectory -C Release --output-on-failure --output-log $testLog
if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed; release unchanged.' }

$candidate = Join-Path $BuildDirectory 'candidate/slrr_host.exe'
$release = Join-Path $BuildDirectory 'Release/slrr_host.exe'
if (-not (Test-Path -LiteralPath $candidate)) { throw 'Validated candidate is missing.' }
$running = Get-Process slrr_host -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $release }
if ($running) { throw 'Close the release game before promotion. The validated candidate is preserved.' }

$checkpoint = Join-Path $workspace ('checkpoints/host-validated-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $checkpoint -Force | Out-Null
$sourcePaths = @('src', 'include', 'shaders', 'tests', 'CMakeLists.txt', 'README.md', 'REFERENCE_PORT.md', 'PHYSICS_PORT_AUDIT.md', 'VEHICLE_DYNAMICS.md', 'NATIVE_SUSPENSION_GEOMETRY.md', 'validate-and-promote.ps1') |
    ForEach-Object { Join-Path $PSScriptRoot $_ }
Compress-Archive -LiteralPath $sourcePaths -DestinationPath (Join-Path $checkpoint 'sources.zip')
Copy-Item -LiteralPath $candidate -Destination (Join-Path $checkpoint 'slrr_host.exe')
Copy-Item -LiteralPath $testLog -Destination (Join-Path $checkpoint 'ctest.log')
if (Test-Path -LiteralPath $release) {
    Copy-Item -LiteralPath $release -Destination (Join-Path $checkpoint 'previous-slrr_host.exe')
}
$files = foreach ($sourcePath in $sourcePaths) {
    if (Test-Path -LiteralPath $sourcePath -PathType Container) {
        Get-ChildItem -LiteralPath $sourcePath -Recurse -File
    } else { Get-Item -LiteralPath $sourcePath }
}
$files | ForEach-Object {
    [pscustomobject]@{
        Path = $_.FullName.Substring($PSScriptRoot.Length + 1)
        SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
} | Sort-Object Path | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $checkpoint 'source-hashes.json')
Copy-Item -LiteralPath $candidate -Destination $release -Force
if ((Get-FileHash -LiteralPath $release).Hash -ne (Get-FileHash -LiteralPath $candidate).Hash) {
    throw 'Release copy failed verification; use the checkpoint executable.'
}
Get-FileHash -LiteralPath $release | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $checkpoint 'executable-hash.json')
Write-Output "Validated release: $release"
Write-Output "Checkpoint: $checkpoint"
