#Requires -Version 5.1
# Run: powershell -NoProfile -File tests/test_build_run_branch_patch.ps1
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot '../scripts/build-run-branch.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$function = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Install-BranchPatch'
}, $true)
if (-not $function) { throw 'Patch installer function missing.' }
# Load just the installer: do not fetch, build, or launch the game in this test.
. ([scriptblock]::Create($function.Extent.Text))
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('openslrr-patch-test-' + [Guid]::NewGuid().ToString('N'))
$repo = Join-Path $fixture 'repo'
$root = Join-Path $fixture 'game'
$backups = Join-Path $fixture 'backups'
foreach ($path in @('repo/patch/sl/Scripts/game', 'repo/patch/system/Scripts/render/osdx', 'repo/patch/frontend', 'game/sl/Scripts/game', 'game/save')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $fixture $path) | Out-Null
}
Set-Content (Join-Path $repo 'patch/README.md') 'not game data'
Set-Content (Join-Path $repo 'patch/sl/Scripts/game/OptionsDialog.class') 'new options'
Set-Content (Join-Path $repo 'patch/system/Scripts/render/osdx/Osd.class') 'new osdx'
Set-Content (Join-Path $repo 'patch/frontend/ui.bin') 'new asset'
Set-Content (Join-Path $root 'sl/Scripts/game/OptionsDialog.class') 'old options'
Set-Content (Join-Path $root 'save/untouched') 'my save'
Install-BranchPatch -Repo $repo -Root $root -BackupBase $backups -Commit 'fixture-commit'
foreach ($relative in @('sl/Scripts/game/OptionsDialog.class', 'system/Scripts/render/osdx/Osd.class', 'frontend/ui.bin')) {
    if ((Get-FileHash (Join-Path $repo "patch/$relative")).Hash -ne (Get-FileHash (Join-Path $root $relative)).Hash) {
        throw "Payload mismatch: $relative"
    }
}
$backup = @(Get-ChildItem -LiteralPath $backups -Directory)[0].FullName
if ((Get-Content (Join-Path $backup 'sl/Scripts/game/OptionsDialog.class')) -ne 'old options') { throw 'Original not backed up.' }
$manifest = Get-Content (Join-Path $backup 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.Commit -ne 'fixture-commit' -or $manifest.Files.Count -ne 3) { throw 'Incorrect manifest.' }
if (Test-Path (Join-Path $root 'README.md')) { throw 'Documentation copied into game.' }
if ((Get-Content (Join-Path $root 'save/untouched')) -ne 'my save') { throw 'Save changed.' }
# A later invocation must replace the installed scripts, not skip old files.
Set-Content (Join-Path $repo 'patch/sl/Scripts/game/OptionsDialog.class') 'latest options'
Install-BranchPatch -Repo $repo -Root $root -BackupBase $backups -Commit 'next-commit'
if ((Get-Content (Join-Path $root 'sl/Scripts/game/OptionsDialog.class')) -ne 'latest options') { throw 'Update was not applied.' }
$rejected = $false
try { Install-BranchPatch -Repo $repo -Root $root -BackupBase (Join-Path $root 'backups') -Commit 'bad' } catch { $rejected = $true }
if (-not $rejected) { throw 'Backup inside GameRoot accepted.' }
Write-Host "PASS payload deployment, update, backup, manifest, save preservation and backup guard. Fixture retained: $fixture"
