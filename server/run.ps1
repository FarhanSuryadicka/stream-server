# Launches the desktop monitor.
#
#   .\server\run.ps1            normal
#   .\server\run.ps1 -Console   show Qt's log output in this terminal
#
# The build copies the Qt runtime beside the executable (windeployqt), so no
# PATH setup is needed and double-clicking the .exe works too. -Console is for
# when something goes wrong: the GUI build has no console, so QML errors are
# invisible without it.

[CmdletBinding()]
param([switch]$Console)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $repo "server/build/ui/ucv-monitor.exe"

if (-not (Test-Path $exe)) {
    Write-Host ""
    Write-Host "  ucv-monitor.exe belum ada." -ForegroundColor Red
    Write-Host "  Build dulu:  .\server\build.ps1"
    exit 1
}

$receiver = Join-Path $repo "experiment/receiver/build/ucv-receiver.exe"
if (-not (Test-Path $receiver)) {
    Write-Host "  Peringatan: ucv-receiver.exe belum ada." -ForegroundColor Yellow
    Write-Host "  Aplikasi tetap terbuka dan bisa membaca run lama, tetapi tombol"
    Write-Host "  Start measurement akan menolak sampai receiver dibangun:"
    Write-Host "      .\experiment\build-receiver.ps1"
    Write-Host ""
}

if ($Console) {
    # Qt suppresses its own logging when there is no console attached; this
    # makes QML errors and warnings visible.
    $env:QT_ASSUME_STDERR_HAS_CONSOLE = "1"
    & $exe
} else {
    Start-Process $exe
    Write-Host "  UCV Monitor dijalankan." -ForegroundColor Green
}
