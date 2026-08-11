# Builds the desktop monitor. Native Windows only - no WSL, no MSYS.
#
#   .\server\build.ps1              full app (needs Qt)
#   .\server\build.ps1 -CoreOnly    measurement logic + tests, no Qt needed
#   .\server\build.ps1 -Clean       delete the build dir first
#   .\server\build.ps1 -Run         launch the app when the build succeeds
#
# Why -Clean matters: CMake caches the compiler in CMakeCache.txt. After
# changing toolchain or Qt version, reconfiguring in place keeps using the OLD
# compiler and produces a binary that fails only at startup, with nothing in the
# build log to explain it. Deleting the directory is the reliable fix.

[CmdletBinding()]
param(
    [switch]$CoreOnly,
    [switch]$Clean,
    [switch]$Run,
    [string]$QtDir      = "C:/Qt/6.8.1/mingw_64",
    [string]$ToolchainDir = "C:/Qt/Tools/mingw1310_64/bin"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $repo "server"
$build = if ($CoreOnly) { Join-Path $src "build-core" } else { Join-Path $src "build" }

function Fail($message) {
    Write-Host ""
    Write-Host "  $message" -ForegroundColor Red
    exit 1
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail "cmake tidak ada di PATH. Install CMake 3.21+ atau tambahkan ke PATH."
}

if ($Clean -and (Test-Path $build)) {
    Write-Host "Menghapus build lama: $build" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $build
}

$args = @("-S", $src, "-B", $build, "-G", "MinGW Makefiles")

if ($CoreOnly) {
    $args += "-DUCV_BUILD_UI=OFF"
    # Any MinGW will do for the core: it links no Qt, so there is no prebuilt
    # runtime to match.
    $gpp = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $gpp) {
        if (-not (Test-Path "$ToolchainDir/g++.exe")) {
            Fail "Tidak menemukan g++. Install MinGW atau jalankan tanpa -CoreOnly."
        }
        $args += "-DCMAKE_C_COMPILER=$ToolchainDir/gcc.exe"
        $args += "-DCMAKE_CXX_COMPILER=$ToolchainDir/g++.exe"
        $args += "-DCMAKE_MAKE_PROGRAM=$ToolchainDir/mingw32-make.exe"
    }
} else {
    if (-not (Test-Path $QtDir)) {
        Fail @"
Qt tidak ditemukan di $QtDir

Install sekali saja:
    pip install aqtinstall
    python -m aqt install-qt   windows desktop 6.8.1 win64_mingw -O C:/Qt
    python -m aqt install-tool windows desktop tools_mingw1310    -O C:/Qt

Atau build tanpa UI:  .\server\build.ps1 -CoreOnly
"@
    }
    if (-not (Test-Path "$ToolchainDir/g++.exe")) {
        Fail @"
MinGW yang cocok dengan Qt tidak ditemukan di $ToolchainDir

Qt 6.8.1 dibangun dengan GCC 13.1.0. Memakai GCC lain akan link sukses tetapi
aplikasi GAGAL SAAT START dengan 'procedure entry point ... could not be
located', tanpa petunjuk apa pun di log build.

    python -m aqt install-tool windows desktop tools_mingw1310 -O C:/Qt
"@
    }
    $args += "-DCMAKE_PREFIX_PATH=$QtDir"
    $args += "-DCMAKE_C_COMPILER=$ToolchainDir/gcc.exe"
    $args += "-DCMAKE_CXX_COMPILER=$ToolchainDir/g++.exe"
    $args += "-DCMAKE_MAKE_PROGRAM=$ToolchainDir/mingw32-make.exe"
}

Write-Host "Configure..." -ForegroundColor Cyan
& cmake @args
if ($LASTEXITCODE -ne 0) { Fail "cmake configure gagal" }

Write-Host "Build..." -ForegroundColor Cyan
& cmake --build $build -j 8
if ($LASTEXITCODE -ne 0) { Fail "build gagal" }

Write-Host "Test..." -ForegroundColor Cyan
Push-Location $build
try { & ctest --output-on-failure } finally { Pop-Location }
if ($LASTEXITCODE -ne 0) { Fail "ada test yang gagal" }

$exe = Join-Path $build "ui/ucv-monitor.exe"
Write-Host ""
Write-Host "  BERHASIL" -ForegroundColor Green
if ($CoreOnly) {
    Write-Host "  Core + test saja (UI tidak dibangun)."
} else {
    Write-Host "  Aplikasi: $exe"
    if (-not (Test-Path (Join-Path $repo "experiment/receiver/build/ucv-receiver.exe"))) {
        Write-Host "  Catatan: ucv-receiver.exe belum ada - jalankan experiment\build-receiver.ps1" -ForegroundColor Yellow
    }
    if ($Run) {
        Write-Host "  Menjalankan..." -ForegroundColor Cyan
        Start-Process $exe
    } else {
        Write-Host '  Jalankan dengan:  .\server\run.ps1'
    }
}
