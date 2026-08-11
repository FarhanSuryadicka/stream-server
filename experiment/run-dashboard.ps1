# Starts the local experiment dashboard and opens it in the default browser.
# No Python packages are required.

param(
    [int]$Port = 8088,
    [string]$Results = (Join-Path $PSScriptRoot "results")
)

$ErrorActionPreference = "Stop"
$server = Join-Path $PSScriptRoot "dashboard\server.py"

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Error "Python is not available on PATH."
}

if (-not (Test-Path (Join-Path $PSScriptRoot "receiver\build\ucv-receiver.exe"))) {
    Write-Error "Receiver is not built. Run .\experiment\build-receiver.ps1 first."
}

Write-Host "Starting UVC dashboard on http://127.0.0.1:$Port/"
Write-Host "Press Ctrl-C here to stop it."
python $server --host 127.0.0.1 --port $Port --results $Results
