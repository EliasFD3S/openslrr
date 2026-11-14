[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9][a-z0-9-]{0,31}$')]
    [string]$Name,
    [string]$BaseRef = 'origin/main',
    [string]$WorktreeRoot = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'worktrees')
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$target = Join-Path $WorktreeRoot $Name
$branch = "agent/$Name"
if (Test-Path -LiteralPath $target) { throw "Worktree path already exists: $target" }
git -C $repo show-ref --verify --quiet "refs/heads/$branch"
if ($LASTEXITCODE -eq 0) { throw "Branch already exists: $branch" }
if ($LASTEXITCODE -ne 1) { throw "Could not inspect branch: $branch" }
git -C $repo rev-parse --verify "$BaseRef^{commit}" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Unknown base ref: $BaseRef" }
$requiredBaseFiles = @('AGENTS.md', 'CMakePresets.json', 'scripts/Initialize-SlrrWorkspace.ps1')
foreach ($relativePath in $requiredBaseFiles) {
    $baseEntry = (& git -C $repo ls-tree --name-only $BaseRef -- $relativePath)
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect $relativePath in $BaseRef" }
    if (-not $baseEntry) {
        throw "Base $BaseRef does not contain $relativePath. Integrate/commit the Harness setup before creating agent worktrees, or select a base that contains it."
    }
}
New-Item -ItemType Directory -Path $WorktreeRoot -Force | Out-Null
git -C $repo worktree add -b $branch $target $BaseRef
if ($LASTEXITCODE -ne 0) { throw "Worktree creation failed: $target" }
& (Join-Path $repo 'scripts/Initialize-SlrrWorkspace.ps1') -RepositoryDirectory $target
if ($LASTEXITCODE -ne 0) { throw "Worktree bootstrap failed; preserved for inspection: $target" }
Write-Output "Agent worktree ready: $target"
Write-Output "Branch: $branch"
Write-Output "Use this path as the Harness subagent cwd."
