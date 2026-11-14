[CmdletBinding()]
param(
    [string]$RepositoryDirectory = (Split-Path $PSScriptRoot -Parent),
    [switch]$Build
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath $RepositoryDirectory).Path
$commonGit = (& git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
if ($LASTEXITCODE -ne 0 -or -not $commonGit) { throw "Not a Git worktree: $repo" }
$mainRepo = Split-Path $commonGit -Parent
$cmakeVersion = '3.31.8'
$cmakeName = "cmake-$cmakeVersion-windows-x86_64"
$tools = Join-Path $mainRepo 'tools'
$downloads = Join-Path $tools '_downloads'
$cmakeHome = Join-Path $tools $cmakeName
$cmake = Join-Path $cmakeHome 'bin/cmake.exe'
$cmakeZip = Join-Path $downloads "$cmakeName.zip"
$cmakeUrl = "https://cmake.org/files/v3.31/$cmakeName.zip"
$cmakeSha256 = '81aa9964dbabd71fe02e7ec50472fd3ad56138c49944515ece9001efbff8d719'
$bgfxCommit = '5ea74ee66662daef4089695846d6322da0dde94e'

New-Item -ItemType Directory -Path $downloads -Force | Out-Null
if (-not (Test-Path -LiteralPath $cmake)) {
    if (-not (Test-Path -LiteralPath $cmakeZip)) {
        curl.exe -fL $cmakeUrl -o $cmakeZip
        if ($LASTEXITCODE -ne 0) { throw "CMake download failed: $cmakeUrl" }
    }
    $actualHash = (Get-FileHash -LiteralPath $cmakeZip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $cmakeSha256) { throw "CMake checksum mismatch: $actualHash" }
    if (Test-Path -LiteralPath $cmakeHome) { throw "Incomplete local CMake directory: $cmakeHome" }
    Expand-Archive -LiteralPath $cmakeZip -DestinationPath $tools
}
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake executable missing: $cmake" }

$mainBgfx = Join-Path $mainRepo 'third_party/bgfx.cmake'
if (-not (Test-Path -LiteralPath (Join-Path $mainBgfx 'CMakeLists.txt'))) {
    if (Test-Path -LiteralPath $mainBgfx) { throw "Incomplete bgfx checkout: $mainBgfx" }
    New-Item -ItemType Directory -Path (Split-Path $mainBgfx -Parent) -Force | Out-Null
    git -C $mainRepo clone https://github.com/bkaradzic/bgfx.cmake.git $mainBgfx
    if ($LASTEXITCODE -ne 0) { throw 'bgfx clone failed' }
    git -C $mainBgfx checkout $bgfxCommit
    if ($LASTEXITCODE -ne 0) { throw 'bgfx pinned checkout failed' }
}
$actualBgfx = (& git -C $mainBgfx rev-parse HEAD).Trim()
if ($actualBgfx -ne $bgfxCommit) { throw "bgfx must remain at $bgfxCommit; observed $actualBgfx in $mainBgfx" }

$repoBgfx = Join-Path $repo 'third_party/bgfx.cmake'
if ($repo -ne $mainRepo -and -not (Test-Path -LiteralPath (Join-Path $repoBgfx 'CMakeLists.txt'))) {
    if (Test-Path -LiteralPath $repoBgfx) { throw "Incomplete worktree bgfx path: $repoBgfx" }
    New-Item -ItemType Directory -Path (Split-Path $repoBgfx -Parent) -Force | Out-Null
    New-Item -ItemType Junction -Path $repoBgfx -Target $mainBgfx | Out-Null
}
$actualRepoBgfx = (& git -C $repoBgfx rev-parse HEAD).Trim()
if ($actualRepoBgfx -ne $bgfxCommit) { throw "Repository bgfx must remain at $bgfxCommit; observed $actualRepoBgfx in $repoBgfx" }
$repoBgfxDirty = (& git -C $repoBgfx status --porcelain)
if ($repoBgfxDirty) { throw "bgfx checkout has local changes; review them before building: $repoBgfx" }

Push-Location $repo
try {
    & $cmake --preset windows-x64-release
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    if ($Build) {
        & $cmake --build --preset windows-x64-release --parallel
        if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
    }
}
finally {
    Pop-Location
}
Write-Output "Workspace ready: $repo"
Write-Output "CMake: $cmake"
Write-Output "bgfx: $actualRepoBgfx"
