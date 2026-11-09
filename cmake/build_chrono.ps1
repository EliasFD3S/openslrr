# Builds and installs the Project Chrono core module (and Eigen) into
# third_party/chrono-install so that CMakeLists.txt can find it with
# find_package(Chrono CONFIG). Run once from the repository root:
#
#   powershell -ExecutionPolicy Bypass -File cmake\build_chrono.ps1
#
# Requirements: Git, CMake 3.21+, Visual Studio 2022 (x64 C++ workload).
param(
  [string]$ChronoTag = "10.0.0",
  [string]$EigenTag = "3.4.0",
  [string]$Config = "Release",
  [string]$CMake = "cmake"
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root "third_party"
New-Item -ItemType Directory -Force $tp | Out-Null

function Invoke-Checked([string]$exe, [string[]]$arguments) {
  & $exe @arguments
  if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

# Eigen (header-only): install it so it provides Eigen3Config.cmake.
$eigenSrc = Join-Path $tp "eigen-$EigenTag"
$eigenInstall = Join-Path $tp "eigen-install"
if (-not (Test-Path (Join-Path $eigenSrc "CMakeLists.txt"))) {
  $zip = Join-Path $tp "eigen.zip"
  Invoke-WebRequest "https://gitlab.com/libeigen/eigen/-/archive/$EigenTag/eigen-$EigenTag.zip" -OutFile $zip
  Expand-Archive $zip $tp -Force
  Remove-Item $zip
}
if (-not (Test-Path (Join-Path $eigenInstall "share\eigen3\cmake\Eigen3Config.cmake"))) {
  Invoke-Checked $CMake @("-S", $eigenSrc, "-B", (Join-Path $tp "eigen-build"),
    "-DCMAKE_INSTALL_PREFIX=$eigenInstall", "-DBUILD_TESTING=OFF",
    "-DEIGEN_BUILD_DOC=OFF", "-DEIGEN_BUILD_PKGCONFIG=OFF")
  Invoke-Checked $CMake @("--build", (Join-Path $tp "eigen-build"), "--target", "install", "--config", $Config)
}

# Chrono core only: no demos, tests, or optional modules.
$chronoSrc = Join-Path $tp "chrono"
if (-not (Test-Path (Join-Path $chronoSrc "CMakeLists.txt"))) {
  Invoke-Checked git @("clone", "--depth", "1", "--branch", $ChronoTag,
    "https://github.com/projectchrono/chrono.git", $chronoSrc)
}
$chronoBuild = Join-Path $tp "chrono-build"
$chronoInstall = Join-Path $tp "chrono-install"
Invoke-Checked $CMake @("-S", $chronoSrc, "-B", $chronoBuild,
  "-G", "Visual Studio 17 2022", "-A", "x64",
  "-DCMAKE_INSTALL_PREFIX=$chronoInstall",
  "-DEigen3_DIR=$eigenInstall\share\eigen3\cmake",
  "-DBUILD_DEMOS=OFF", "-DBUILD_TESTING=OFF", "-DBUILD_BENCHMARKING=OFF",
  "-DCH_ENABLE_OPENMP=OFF")
Invoke-Checked $CMake @("--build", $chronoBuild, "--config", $Config, "--parallel", "--target", "install")
Write-Host "Chrono installed to $chronoInstall"
