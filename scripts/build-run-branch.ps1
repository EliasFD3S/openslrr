#Requires -Version 5.1
<#
.SYNOPSIS
  Fetch, build, and run an OpenSLRR branch on Windows x64.
.EXAMPLE
  .\scripts\build-run-branch.ps1 -Branch main -GameRoot 'D:\Games\SLRR'
.EXAMPLE
  .\scripts\build-run-branch.ps1 -Branch feature/my-branch -GameRoot 'D:\Games\SLRR' `
    -Workspace 'D:\OpenSLRR-builds' -FmodSdkDir 'D:\FMOD Studio API Windows'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Branch,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$GameRoot,

    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'OpenSLRR-branch-builds'),
    [string]$FmodSdkDir = $env:SLRR_FMOD_SDK_DIR,
    [string]$ToolsCacheDir = $env:SLRR_TOOLS_CACHE_DIR,
    [string[]]$GameArguments = @()
)

$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed (exit $LASTEXITCODE): $Program $($Arguments -join ' ')"
    }
}

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitOperatingSystem) {
    throw 'OpenSLRR requires Windows x64 and Visual Studio 2022 with the C++ workload.'
}
foreach ($command in @('git', 'cmake')) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "Required command '$command' was not found in PATH."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $GameRoot 'system/Scripts/lang/System.class') -PathType Leaf)) {
    throw "GameRoot must contain system/Scripts/lang/System.class: $GameRoot"
}
$GameRoot = (Resolve-Path -LiteralPath $GameRoot).Path
foreach ($argument in $GameArguments) {
    if ($argument -eq '--root' -or $argument -like '--root=*') {
        throw 'Specify the game root with -GameRoot, not -GameArguments.'
    }
}
Invoke-Checked 'git' @('check-ref-format', "refs/heads/$Branch")

# Hash the entire branch name: slash-containing names get separate checkouts.
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $branchId = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Branch)))).Replace('-', '').ToLowerInvariant()
} finally { $sha.Dispose() }
New-Item -ItemType Directory -Force -Path $Workspace | Out-Null
$Workspace = (Resolve-Path -LiteralPath $Workspace).Path
$repo = Join-Path $Workspace "branch-$branchId"
$repoUrl = 'https://github.com/EliasFD3S/openslrr.git'
$bgfxUrl = 'https://github.com/bkaradzic/bgfx.cmake.git'
# This is the pin documented by the version of OpenSLRR containing this script.
$bgfxRevision = '5ea74ee66662daef4089695846d6322da0dde94e'

# Lock until the game exits so a second invocation cannot replace its build.
try {
    $lock = [System.IO.File]::Open((Join-Path $Workspace "branch-$branchId.lock"),
        [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::None)
} catch {
    throw "Cannot acquire the branch lock. Another invocation may be running: $Branch"
}
try {
    if (-not (Test-Path -LiteralPath $repo)) {
        # A normal clone has a populated worktree, so the clean check works on first use.
        Invoke-Checked 'git' @('clone', '--', $repoUrl, $repo)
    }
    if (-not (Test-Path -LiteralPath (Join-Path $repo '.git'))) {
        throw "Existing checkout path is not a Git repository: $repo"
    }
    $origin = & git -C $repo remote get-url origin
    if ($LASTEXITCODE -ne 0 -or $origin -ne $repoUrl) {
        throw "Unexpected origin for checkout: $repo"
    }
    # Do not overwrite local edits, including ignored files that would be tracked
    # by the requested branch. Build outputs remain available for reuse.
    $status = & git -C $repo status --porcelain --untracked-files=no
    if ($LASTEXITCODE -ne 0) { throw 'Failed to inspect checkout status.' }
    if ($status) { throw "Checkout has tracked changes; preserve or clean them first: $repo" }
    Invoke-Checked 'git' @('-C', $repo, 'fetch', '--no-tags', 'origin', "+refs/heads/${Branch}:refs/remotes/origin/${Branch}")
    Invoke-Checked 'git' @('-C', $repo, 'checkout', '--no-overwrite-ignore', '--detach', "refs/remotes/origin/$Branch")

    $bgfx = Join-Path $repo 'third_party/bgfx.cmake'
    if (-not (Test-Path -LiteralPath $bgfx)) {
        Invoke-Checked 'git' @('clone', '--', $bgfxUrl, $bgfx)
    }
    if (-not (Test-Path -LiteralPath (Join-Path $bgfx '.git'))) {
        throw "Existing bgfx.cmake path is not a Git repository: $bgfx"
    }
    $bgfxOrigin = & git -C $bgfx remote get-url origin
    if ($LASTEXITCODE -ne 0 -or $bgfxOrigin -ne $bgfxUrl) {
        throw "Unexpected origin for bgfx.cmake checkout: $bgfx"
    }
    $bgfxStatus = & git -C $bgfx status --porcelain --untracked-files=no
    if ($LASTEXITCODE -ne 0) { throw 'Failed to inspect bgfx.cmake checkout status.' }
    if ($bgfxStatus) { throw "bgfx.cmake has tracked changes; preserve or clean them first: $bgfx" }
    Invoke-Checked 'git' @('-C', $bgfx, 'fetch', 'origin', $bgfxRevision)
    Invoke-Checked 'git' @('-C', $bgfx, 'checkout', '--no-overwrite-ignore', '--detach', $bgfxRevision)

    $build = Join-Path $repo 'build'
    $configureArgs = @('-S', $repo, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
    if ($FmodSdkDir) { $configureArgs += "-DSLRR_FMOD_SDK_DIR=$FmodSdkDir" }
    if ($ToolsCacheDir) { $configureArgs += "-DSLRR_TOOLS_CACHE_DIR=$ToolsCacheDir" }
    Invoke-Checked 'cmake' $configureArgs
    Invoke-Checked 'cmake' @('--build', $build, '--config', 'Release', '--target', 'slrr_host', '--parallel')

    $exe = Join-Path $build 'Release/slrr_host.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Build did not produce $exe" }
    Write-Host "Launching branch '$Branch' from $exe"
    Push-Location $repo
    try {
        & $exe --game --root $GameRoot --window @GameArguments
        if ($LASTEXITCODE -ne 0) { throw "Game exited with code $LASTEXITCODE." }
    } finally { Pop-Location }
} finally { $lock.Dispose() }
