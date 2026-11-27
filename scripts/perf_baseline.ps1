<#
.SYNOPSIS
  Runs the Valo City benchmark drive over a matrix of scenes, CPU profiles and
  graphics presets, and summarises the --perf-log CSVs (#358).

.DESCRIPTION
  Each run uses a private copy of the game root (the benchmark rewrites
  save/game/options on exit). CPU profiles restrict the process affinity,
  which the job pool honours (src/platform/job_system.cpp:logical_processors),
  to approximate weaker CPUs on a hybrid Intel desktop:
    all   every logical processor
    p4    4 performance cores with Hyper-Threading (logical CPUs 0-7)
    e4    4 efficiency cores, no Hyper-Threading (logical CPUs 16-19 on an
          8P+8E part such as the i7-13700K)
  Graphics presets:
    saved the options already in the source root
    low   1920x1080, no MSAA, no AO, low shadows, no dynamic reflections or
          volumetric clouds
  Single-setting variants of saved, for the GPU pass costs (#362):
    msaa4  MSAA 4x            noao    AO off
    hard   shadow smoothing 0 norefl  dynamic reflections off
  The first and second CSV rows (warm-up) are left out of the averages.

.EXAMPLE
  powershell -File scripts\perf_baseline.ps1 -SourceRoot game -Out assets\validation\perf-baseline\run1
#>
param(
  [Parameter(Mandatory)] [string] $SourceRoot,
  [Parameter(Mandatory)] [string] $Out,
  [string] $Exe = "build\Release\slrr_host.exe",
  [string[]] $Scenes = @("day", "night"),
  [string[]] $Cpus = @("all", "p4", "e4"),
  [string[]] $Presets = @("saved", "low"),
  [int] $DriveSeconds = 40,
  # A --record-path recording to replay instead of the straight
  # full-throttle drive; the drive then lasts the recording.
  [string] $ReplayPath = "",
  [int] $Seed = 3
)
$ErrorActionPreference = "Stop"
# powershell -File passes "day,night" as one string: split it here.
$Scenes = @($Scenes | ForEach-Object { $_ -split "," })
$Cpus = @($Cpus | ForEach-Object { $_ -split "," })
$Presets = @($Presets | ForEach-Object { $_ -split "," })
$hours = @{ day = 13; night = 22 }
$masks = @{ all = 0; p4 = 0xFF; e4 = 0xF0000 }
$exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$out = (Resolve-Path $Out).Path
$work = Join-Path (Split-Path $exe) "perf-roots"

function Set-Options([string] $root, [hashtable] $set) {
  $file = Join-Path $root "save\game\openslrr_options"
  $lines = Get-Content $file | ForEach-Object {
    $k = ($_ -split "=")[0]; if ($set.ContainsKey($k)) { "$k=$($set[$k])" } else { $_ } }
  $lines | Set-Content $file -Encoding ascii
}

function Write-Preset([string] $root, [string] $preset) {
  if ($preset -eq "saved") { return }
  $variants = @{ msaa4 = @{ msaa = 2 }; noao = @{ ao_method = 0 }; hard = @{ shadow_smoothing = 0 };
                 norefl = @{ dynamic_reflections = 0 } }
  if ($variants.ContainsKey($preset)) { Set-Options $root $variants[$preset]; return }
  if ($preset -ne "low") { throw "unknown preset $preset" }
  $raw = Join-Path $root "save\game\options"
  # values[3] and values[4] are the resolution. Edit the text, not the parsed
  # JSON: in a values list 1.0 is a float and 1 an int, and a JSON round trip
  # would turn one into the other.
  $text = [IO.File]::ReadAllText($raw)
  $fixed = [regex]::Replace($text, '("values":\s*\[\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*)\d+(,\s*)\d+', '${1}1920${2}1080')
  if ($fixed -eq $text) { throw "resolution not found in $raw" }
  [IO.File]::WriteAllText($raw, $fixed)
  Set-Options $root @{ msaa = 0; ao_method = 0; shadow_quality = 0; shadow_smoothing = 0;
                       dynamic_reflections = 0; volumetric_clouds = 0 }
}

$drive = if ($ReplayPath) { @("--replay-path", (Resolve-Path $ReplayPath).Path) } else { @("--bench-drive", "$DriveSeconds") }
$rows = @()
$me = [Diagnostics.Process]::GetCurrentProcess()
$fullMask = $me.ProcessorAffinity
foreach ($preset in $Presets) {
  $root = Join-Path $work $preset
  robocopy (Resolve-Path $SourceRoot).Path $root /MIR /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
  Write-Preset $root $preset
  foreach ($scene in $Scenes) {
    foreach ($cpu in $Cpus) {
      $name = "$scene-$cpu-$preset"
      $csv = Join-Path $out "$name.csv"
      Remove-Item $csv -ErrorAction SilentlyContinue
      # Children inherit the affinity, so the pool sees it from the start.
      $me.ProcessorAffinity = if ($masks[$cpu]) { [IntPtr]$masks[$cpu] } else { $fullMask }
      try {
        $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru `
          -RedirectStandardOutput (Join-Path $out "$name.out.txt") `
          -RedirectStandardError (Join-Path $out "$name.err.txt") `
          -ArgumentList (@("--game", "--window", "--root", $root, "--no-vsync",
            "--spawn-car", "cars/racers/Baiern.rpk:0x6", "--bench-map", "ValoCity",
            "--bench-time", "$($hours[$scene])", "--seed", "$Seed",
            "--perf-log", $csv) + $drive)
      } finally { $me.ProcessorAffinity = $fullMask }
      if (-not $p.WaitForExit(600000)) { $p.Kill(); Write-Warning "$name timed out" }
      $data = @(Import-Csv $csv | Select-Object -Skip 2)
      $speed = (Select-String -Path (Join-Path $out "$name.out.txt") -Pattern "\[bench\] t=\S+ speed=(\S+)" |
        Select-Object -Last 1).Matches.Groups[1].Value
      $avg = [ordered]@{ run = $name; end_kmh = $speed }
      foreach ($c in @("fps", "frame_avg_ms", "frame_p95_ms", "frame_max_ms", "scripts_ms", "physics_ms",
                     "flush_ms", "collect_ms", "collect_batches_ms", "collect_instances_ms", "shadows_ms",
                     "refl_ms", "world_ms", "worker_submit_ms", "present_ms", "render_thread_ms",
                     "gpu_ms", "draws") + @($data[0].PSObject.Properties.Name | Where-Object { $_ -like "gpu_*_ms" })) {
        # Windows PowerShell 5.1: Measure-Object takes no script block, and
        # the CSV fields are strings.
        $sum = 0.0; foreach ($r in $data) { $sum += [double]::Parse($r.$c, [Globalization.CultureInfo]::InvariantCulture) }
        $avg[$c] = if ($data.Count) { [math]::Round($sum / $data.Count, 2) } else { $null }
      }
      $rows += [pscustomobject]$avg
      Write-Host ("{0,-18} {1,6} fps  frame {2,6} ms  flush {3,6}  gpu {4,6}" -f $name, $avg.fps, $avg.frame_avg_ms, $avg.flush_ms, $avg.gpu_ms)
    }
  }
}
$rows | Export-Csv (Join-Path $out "summary.csv") -NoTypeInformation -Encoding utf8
$rows | Format-Table -AutoSize | Out-String -Width 400 | Set-Content (Join-Path $out "summary.txt") -Encoding utf8
