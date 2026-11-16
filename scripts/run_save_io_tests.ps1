param(
  [string]$BuildDir = "build",
  [string]$BaselineRef = "",
  [ValidateSet("save_io", "save_persistence", "scx_skin", "physics_position", "garage_shell", "asset_bounds")]
  [string]$Suite = "save_io"
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path $PSScriptRoot -Parent
Push-Location $repoRoot
try {
  $buildRoot = (Resolve-Path $BuildDir).Path
  $suiteName = $Suite.Replace("_", "-")
  $outputRoot = Join-Path $buildRoot "$suiteName-tests"
  New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
  $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $installation) { throw "Visual Studio C++ x64 tools not found" }
  $toolVersion = (Get-Content (Join-Path $installation "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")).Trim()
  $toolRoot = Join-Path $installation "VC/Tools/MSVC/$toolVersion"
  $sdkRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
  $sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'um/Windows.h') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
  $env:INCLUDE = @("$toolRoot/include", "$sdkRoot/Include/$sdkVersion/ucrt", "$sdkRoot/Include/$sdkVersion/shared", "$sdkRoot/Include/$sdkVersion/um") -join ';'
  $env:LIB = @("$toolRoot/lib/x64", "$sdkRoot/Lib/$sdkVersion/ucrt/x64", "$sdkRoot/Lib/$sdkVersion/um/x64") -join ';'
  $compiler = Join-Path $toolRoot 'bin/Hostx64/x64/cl.exe'
  [xml]$project = Get-Content (Join-Path $buildRoot 'slrr_host.vcxproj')
  $dependencies = $project.SelectSingleNode("//*[local-name()='ItemDefinitionGroup' and contains(@Condition,'Release|x64')]/*[local-name()='Link']/*[local-name()='AdditionalDependencies']").InnerText
  $libraries = foreach ($entry in $dependencies.Split(';')) {
    if (-not $entry -or $entry.StartsWith('%(')) { continue }
    $entry = $entry.Replace('$(Configuration)', 'Release')
    if ([IO.Path]::IsPathRooted($entry) -or $entry -notmatch '[/\\]') { $entry }
    else { Join-Path $buildRoot $entry }
  }
  $objects = (Get-ChildItem (Join-Path $buildRoot 'slrr_host.dir/Release') -Recurse -Filter '*.obj' |
    Where-Object { $_.Name -ne 'main.obj' }).FullName
  if (-not $objects) { throw "Build slrr_host Release first" }
  $label = 'after'
  if ($BaselineRef) {
    $label = 'before'
    $baselineRoot = Join-Path $outputRoot 'baseline'
    New-Item -ItemType Directory -Path $baselineRoot -Force | Out-Null
    $baselineSources = if ($Suite -eq 'asset_bounds') { @('src/resources/scx_mesh.cpp', 'src/formats/matrixdata.cpp') } elseif ($Suite -eq 'garage_shell') { @('src/natives/natives_resref.cpp') } elseif ($Suite -eq 'physics_position') { @('src/natives/natives_osd.cpp') } elseif ($Suite -eq 'scx_skin') { @('src/resources/scx_mesh.cpp') } else { @('src/natives/natives_boot.cpp','src/natives/natives_io.cpp') }
    if ($Suite -eq 'save_persistence') { $baselineSources += 'src/resources/part_runtime.cpp' }
    foreach ($sourcePath in $baselineSources) {
      $name = [IO.Path]::GetFileNameWithoutExtension($sourcePath)
      $source = Join-Path $baselineRoot "$name.cpp"
      $bytes = & git -c "safe.directory=$($repoRoot.Replace('\','/'))" show "${BaselineRef}:$sourcePath"
      if ($LASTEXITCODE -ne 0) { throw "Could not read $BaselineRef $name" }
      [IO.File]::WriteAllText($source, ($bytes -join "`n"), [Text.UTF8Encoding]::new($false))
      $object = Join-Path $baselineRoot "$name.obj"
      & $compiler /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc/natives /c $source "/Fo:$object"
      if ($LASTEXITCODE -ne 0) { throw "Baseline compilation failed" }
      $objects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne "$name.obj" }) + @($object)
    }
  }
  $executable = Join-Path $outputRoot "$suiteName-$label.exe"
  $testObject = Join-Path $outputRoot "$suiteName-$label.obj"
  & $compiler /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc/natives "tests/test_$Suite.cpp" "/Fe:$executable" "/Fo:$testObject" @objects @libraries
  if ($LASTEXITCODE -ne 0) { throw "Save I/O regression link failed" }
  $log = Join-Path $outputRoot "$label.log"
  & $executable *> $log
  $testExit = $LASTEXITCODE
  Get-Content $log
  Write-Output "Save I/O test exit code: $testExit (log: $log)"
  exit $testExit
} finally {
  Pop-Location
}
