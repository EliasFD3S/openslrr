param(
  [string]$BuildDir = "build",
  [string]$BaselineRef = "",
  [switch]$CorpusOnly,
  [ValidateSet("save_io", "save_persistence", "scx_skin", "physics_position", "garage_shell", "map_props", "asset_bounds", "ped_crossing", "time_format", "night_props", "sun_cycle", "tree_numbers", "ui_lifetime", "resource_cache", "tufa_validation", "rpk_validation", "audio_recovery", "vm_lookup", "script_safety", "video_safety", "options_retry", "live_lookup", "driver_mask", "driver_selector", "pad_fallback", "class_lifetime", "static_increment", "inventory_transition", "map_views", "map_alpha", "physics_recovery", "findfile_paths", "splash_boot", "menu_debounce", "tooltip_render", "car_select_view", "trigger_markers", "trigger_volume", "gameref_alias", "osd_text_batch", "osd_text_gpu")]
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
  $suiteIncludes = @()
  if ($Suite -eq 'osd_text_gpu') {
    $includePaths = $project.SelectSingleNode("//*[local-name()='ItemDefinitionGroup' and contains(@Condition,'Release|x64')]/*[local-name()='ClCompile']/*[local-name()='AdditionalIncludeDirectories']").InnerText
    $suiteIncludes = @('/Isrc/render', '/Zc:__cplusplus', '/Zc:preprocessor', '/DBX_CONFIG_DEBUG=0', '/DBGFX_CONFIG_DEBUG_ANNOTATION=0', '/DBGFX_CONFIG_MULTITHREADED=1', '/DBGFX_CONFIG_VIDEO=1', '/DNDEBUG') + @($includePaths.Split(';') | Where-Object { $_ -and -not $_.StartsWith('%(') } | ForEach-Object { "/I$_" })
  }
  $label = 'after'
  if ($BaselineRef) {
    $label = 'before'
    $baselineRoot = Join-Path $outputRoot 'baseline'
    New-Item -ItemType Directory -Path $baselineRoot -Force | Out-Null
    $baselineSources = if ($Suite -eq 'osd_text_gpu') { @('src/render/bgfx_osd.cpp','src/render/bgfx_lib_backend.cpp') } elseif ($Suite -eq 'gameref_alias') { @('src/resources/gameref.cpp') } elseif ($Suite -eq 'trigger_markers') { @('src/resources/scene_instances.cpp') } elseif ($Suite -eq 'car_select_view') { @('src/boot/mainloop.cpp','src/resources/gameref.cpp') } elseif ($Suite -eq 'tooltip_render') { @('src/resources/scene_instances.cpp') } elseif ($Suite -eq 'menu_debounce') { @('src/platform/host_input.cpp') } elseif ($Suite -eq 'splash_boot') { @('src/boot/boot.cpp') } elseif ($Suite -eq 'physics_recovery') { @('src/resources/part_runtime.cpp','src/resources/gameref.cpp') } elseif ($Suite -eq 'map_views') { @('src/resources/gameref.cpp') } elseif ($Suite -eq 'static_increment') { @('src/jvm/tree_eval.cpp') } elseif ($Suite -eq 'class_lifetime') { @('src/jvm/jvm.cpp') } elseif ($Suite -eq 'live_lookup') { @('src/resources/gameref.cpp') } elseif ($Suite -eq 'vm_lookup') { @('src/natives/native_registry.cpp', 'src/jvm/jvm.cpp') } elseif ($Suite -eq 'tufa_validation') { @('src/jvm/tufa.cpp') } elseif ($Suite -eq 'tree_numbers') { @('src/jvm/tree_eval.cpp') } elseif ($Suite -eq 'sun_cycle') { @('src/resources/scene_sun.cpp') } elseif ($Suite -eq 'night_props') { @('src/resources/scene_instances.cpp') } elseif ($Suite -eq 'time_format') { @('src/ui/ui_time.cpp') } elseif ($Suite -eq 'asset_bounds') { @('src/resources/scx_mesh.cpp', 'src/formats/matrixdata.cpp') } elseif ($Suite -eq 'garage_shell' -or $Suite -eq 'map_props') { @('src/natives/natives_resref.cpp') } elseif ($Suite -eq 'physics_position') { @('src/natives/natives_osd.cpp') } elseif ($Suite -eq 'scx_skin') { @('src/resources/scx_mesh.cpp') } else { @('src/natives/natives_boot.cpp','src/natives/natives_io.cpp') }
    if ($Suite -eq 'save_persistence') { $baselineSources += 'src/resources/part_runtime.cpp' }
    foreach ($sourcePath in $baselineSources) {
      $name = [IO.Path]::GetFileNameWithoutExtension($sourcePath)
      $source = Join-Path $baselineRoot "$name.cpp"
      $bytes = & git -c "safe.directory=$($repoRoot.Replace('\','/'))" show "${BaselineRef}:$sourcePath"
      if ($LASTEXITCODE -ne 0) { throw "Could not read $BaselineRef $name" }
      [IO.File]::WriteAllText($source, ($bytes -join "`n"), [Text.UTF8Encoding]::new($false))
      $object = Join-Path $baselineRoot "$name.obj"
      & $compiler /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc/natives /Isrc/platform "/I$buildRoot/generated" @suiteIncludes /c $source "/Fo:$object"
      if ($LASTEXITCODE -ne 0) { throw "Baseline compilation failed" }
      $objects = @($objects | Where-Object { [IO.Path]::GetFileName($_) -ne "$name.obj" }) + @($object)
    }
  }
  $executable = Join-Path $outputRoot "$suiteName-$label.exe"
  $testObject = Join-Path $outputRoot "$suiteName-$label.obj"
  & $compiler /nologo /std:c++20 /EHsc /MD /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc/natives @suiteIncludes "tests/test_$Suite.cpp" "/Fe:$executable" "/Fo:$testObject" @objects @libraries
  if ($LASTEXITCODE -ne 0) { throw "Save I/O regression link failed" }
  $log = Join-Path $outputRoot "$label.log"
  if ($CorpusOnly) {
    if ($Suite -notin @('tufa_validation','rpk_validation')) { throw "CorpusOnly requires a validation suite" }
    & $executable --corpus *> $log
  } else {
    & $executable *> $log
  }
  $testExit = $LASTEXITCODE
  Get-Content $log
  Write-Output "Save I/O test exit code: $testExit (log: $log)"
  exit $testExit
} finally {
  Pop-Location
}
