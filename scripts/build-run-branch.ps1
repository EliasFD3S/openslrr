#Requires -Version 5.1
<#
.SYNOPSIS
  Fetch, build, and run an OpenSLRR branch on Windows x64.
.EXAMPLE
  .\scripts\build-run-branch.ps1 -Branch main -GameRoot 'D:\Games\SLRR'
.EXAMPLE
  .\scripts\build-run-branch.ps1 -Branch feature/my-branch -GameRoot 'D:\Games\SLRR' `
    -Workspace 'D:\OpenSLRR-builds'
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

function Install-BranchPatch {
    param([string]$Repo, [string]$Root, [string]$BackupBase, [string]$Commit)

    $patch = Join-Path $Repo 'patch'
    if (-not (Test-Path -LiteralPath $patch -PathType Container)) {
        throw "Branch has no patch directory: $patch"
    }
    # Top-level files (README, etc.) are documentation. Every payload folder
    # is deployed, including system, sl, and any future asset folders.
    $files = @(Get-ChildItem -LiteralPath $patch -Directory -Force | ForEach-Object {
        Get-ChildItem -LiteralPath $_.FullName -File -Recurse -Force
    })
    if ($files.Count -eq 0) { throw "Branch patch contains no game files: $patch" }
    $patchPrefix = $patch.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    # Script mods shipped with the branch (#470: mods/nfsu2-camera) go to
    # <GameRoot>/mods/<name>; their tests folders are not installed.
    $mods = Join-Path $Repo 'mods'
    $modsPrefix = $mods.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $modFiles = @()
    if (Test-Path -LiteralPath $mods -PathType Container) {
        $modFiles = @(Get-ChildItem -LiteralPath $mods -File -Recurse -Force | Where-Object {
            $_.FullName.Substring($modsPrefix.Length) -notmatch '(^|[\\/])tests([\\/]|$)'
        })
    }
    $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $backup = Join-Path $BackupBase ([Guid]::NewGuid().ToString('N'))
    if ([IO.Path]::GetFullPath($backup).StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Patch backups must be outside GameRoot (old classes there can shadow the new UI). Choose a workspace outside GameRoot.'
    }
    $sources = @($files | ForEach-Object { [PSCustomObject]@{ File = $_; Relative = $_.FullName.Substring($patchPrefix.Length) } }) +
        @($modFiles | ForEach-Object { [PSCustomObject]@{ File = $_; Relative = Join-Path 'mods' $_.FullName.Substring($modsPrefix.Length) } })
    $entries = @($sources | ForEach-Object {
        $relative = $_.Relative
        $destination = Join-Path $Root $relative
        [PSCustomObject]@{
            Relative = $relative
            Source = $_.File.FullName
            Destination = $destination
            Existed = (Test-Path -LiteralPath $destination -PathType Leaf)
            SHA256 = (Get-FileHash -LiteralPath $_.File.FullName -Algorithm SHA256).Hash
        }
    })
    # Do not follow junctions/symlinks out of either tree when copying.
    foreach ($entry in $entries) {
        foreach ($path in @($entry.Source, $entry.Destination)) {
            $current = $path
            while ($current) {
                if (Test-Path -LiteralPath $current) {
                    if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                        throw "Patch path contains a symlink or junction: $current"
                    }
                }
                $current = Split-Path -Parent $current
            }
        }
    }
    # Back up every existing destination before modifying any game file.
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    foreach ($entry in $entries) {
        if ($entry.Existed) {
            $saved = Join-Path $backup $entry.Relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saved) | Out-Null
            Copy-Item -LiteralPath $entry.Destination -Destination $saved -Force
        }
    }
    [PSCustomObject]@{ Commit = $Commit; GameRoot = $Root; Files = $entries } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'manifest.json') -Encoding UTF8
    Write-Host "Applying $($entries.Count) branch patch files; backup: $backup"
    foreach ($entry in $entries) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $entry.Destination) | Out-Null
        Copy-Item -LiteralPath $entry.Source -Destination $entry.Destination -Force
        if ((Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256).Hash -ne $entry.SHA256) {
            throw "Patch verification failed: $($entry.Destination). Backup: $backup"
        }
    }
    Write-Host "Verified all branch patch files for commit $Commit in $Root"
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
    # Fetch the requested branch, including any commits that were added since
    # the previous run. The dedicated checkout uses detached HEAD, so updating
    # its remote-tracking ref and checking that ref out is its equivalent of
    # pulling the branch's latest changes.
    Invoke-Checked 'git' @('-C', $repo, 'fetch', '--no-tags', 'origin', "+refs/heads/${Branch}:refs/remotes/origin/${Branch}")
    $remoteRef = "refs/remotes/origin/$Branch"
    Invoke-Checked 'git' @('-C', $repo, 'checkout', '--no-overwrite-ignore', '--detach', $remoteRef)
    $expectedCommit = (& git -C $repo rev-parse --verify "$remoteRef^{commit}").Trim()
    if ($LASTEXITCODE -ne 0) { throw "Failed to resolve fetched branch: $remoteRef" }
    $checkedOutCommit = (& git -C $repo rev-parse --verify 'HEAD^{commit}').Trim()
    if ($LASTEXITCODE -ne 0 -or $checkedOutCommit -ne $expectedCommit) {
        throw "Checkout does not match fetched branch '$Branch' (expected $expectedCommit, got $checkedOutCommit)."
    }
    Write-Host "Building branch '$Branch' at commit $checkedOutCommit"

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

    # This checkout is reused across invocations. Reject a cache tied to a
    # different source tree or an incomplete cache instead of building stale
    # output from somewhere other than this checkout.
    $build = Join-Path $repo 'build'
    if (Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt')) {
        # CMake's -L output can omit INTERNAL entries on some versions. Read
        # the cache file directly for the authoritative source directory.
        $cachedSource = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') |
            Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } |
            Select-Object -First 1
        if (-not $cachedSource) {
            throw "Existing CMake cache has no source-directory entry: $build. Remove or choose another workspace: $Workspace"
        }
        $cachedSourcePath = $cachedSource.Substring('CMAKE_HOME_DIRECTORY:INTERNAL='.Length)
        if ([IO.Path]::GetFullPath($cachedSourcePath) -ne [IO.Path]::GetFullPath($repo)) {
            throw "Existing CMake cache points to a different source tree: $cachedSourcePath. Remove or choose another workspace: $Workspace"
        }
    }
    $configureArgs = @('-S', $repo, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64')
    if ($ToolsCacheDir) { $configureArgs += "-DSLRR_TOOLS_CACHE_DIR=$ToolsCacheDir" }
    Invoke-Checked 'cmake' $configureArgs
    Invoke-Checked 'cmake' @('--build', $build, '--config', 'Release', '--target', 'slrr_host', '--parallel')

    $exe = Join-Path $build 'Release/slrr_host.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Build did not produce $exe" }
    $exe = (Resolve-Path -LiteralPath $exe).Path
    $exeCommit = (& git -C $repo rev-parse --verify 'HEAD^{commit}').Trim()
    if ($LASTEXITCODE -ne 0 -or $exeCommit -ne $checkedOutCommit) {
        throw "Refusing to launch: checkout changed during build (built $checkedOutCommit, current $exeCommit)."
    }
    Install-BranchPatch -Repo $repo -Root $GameRoot `
        -BackupBase (Join-Path $Workspace 'patch-backups') -Commit $exeCommit
    Write-Host "Launching branch '$Branch' at commit $exeCommit from $exe"
    Push-Location $repo
    try {
        & $exe --game --root $GameRoot --window @GameArguments
        if ($LASTEXITCODE -ne 0) { throw "Game exited with code $LASTEXITCODE." }
    } finally { Pop-Location }
} finally { $lock.Dispose() }
