param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build-bgfx')
)

$ErrorActionPreference = 'Stop'
$workspace = Split-Path $PSScriptRoot -Parent
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else {
    'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$configure = @('-S', $PSScriptRoot, '-B', $BuildDirectory,
    '-DSLRR_STAGE_BUILD=ON')
if (-not (Test-Path -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt'))) {
    $configure += @('-A', 'Win32')
}
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed; release unchanged.' }
& $cmake --build $BuildDirectory --config Release --target slrr_host --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed; release unchanged.' }

$candidate = Join-Path $BuildDirectory 'candidate/slrr_host.exe'
$release = Join-Path $BuildDirectory 'Release/slrr_host.exe'
if (-not (Test-Path -LiteralPath $candidate)) { throw 'Built candidate is missing.' }
$running = Get-Process slrr_host -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $release }
if ($running) { throw 'Close the release game before promotion. The built candidate is preserved.' }

$checkpoint = Join-Path $workspace ('checkpoints/host-built-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $checkpoint -Force | Out-Null
$sourcePaths = @('src', 'include', 'shaders', 'CMakeLists.txt', 'README.md', 'REFERENCE_PORT.md', 'PHYSICS_PORT_AUDIT.md', 'VEHICLE_DYNAMICS.md', 'NATIVE_SUSPENSION_GEOMETRY.md', 'validate-and-promote.ps1') |
    ForEach-Object { Join-Path $PSScriptRoot $_ } |
    Where-Object { Test-Path -LiteralPath $_ }
Compress-Archive -LiteralPath $sourcePaths -DestinationPath (Join-Path $checkpoint 'sources.zip')
Copy-Item -LiteralPath $candidate -Destination (Join-Path $checkpoint 'slrr_host.exe')
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
Write-Output "Built release: $release"
Write-Output "Checkpoint: $checkpoint"
