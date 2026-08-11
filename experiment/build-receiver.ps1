# Builds the PC-side receiver and its test binaries into experiment/receiver/build.
#
# CMake needs an explicit compiler here: this machine has no MSVC installed,
# only the MinGW-w64 toolchain pulled in via winget, which CMake does not find
# on its own.
#
# Usage:  .\experiment\build-receiver.ps1
#         .\experiment\build-receiver.ps1 -Clean

param([switch]$Clean)

$ErrorActionPreference = "Stop"

$root      = Split-Path -Parent $PSScriptRoot
$srcDir    = Join-Path $root "experiment\receiver"
$buildDir  = Join-Path $srcDir "build"

# Locate MinGW. The winget install does not put it on PATH for existing
# shells, so search its package directory before giving up.
$mingwBin = $null
$candidates = @(
    (Get-Command g++ -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source),
    "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
)
foreach ($c in $candidates) {
    if ($c -and (Test-Path $c)) { $mingwBin = Split-Path -Parent $c; break }
}
if (-not $mingwBin) {
    Write-Error @"
No C++ compiler found.

Install one with:
    winget install --id BrechtSanders.WinLibs.POSIX.UCRT

(Visual Studio with the C++ workload works too, in which case CMake will find
it without this script's help.)
"@
}

Write-Host "compiler : $mingwBin\g++.exe"
Write-Host "source   : $srcDir"
Write-Host "build    : $buildDir`n"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "cleaning $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

& cmake -S $srcDir -B $buildDir -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_CXX_COMPILER="$mingwBin\g++.exe" `
    -DCMAKE_MAKE_PROGRAM="$mingwBin\mingw32-make.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed" }

& cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { Write-Error "build failed" }

# The primitives are what every measurement rests on, and a bug in them does
# not crash — it produces plausible wrong numbers. Run the tests as part of
# the build rather than leaving it optional.
Write-Host "`nrunning tests..."
Push-Location $buildDir
try {
    & ctest --output-on-failure
    if ($LASTEXITCODE -ne 0) { Write-Error "TESTS FAILED - do not trust measurements from this build" }
} finally {
    Pop-Location
}

Write-Host "`nBuilt:"
Get-ChildItem "$buildDir\*.exe" | ForEach-Object {
    Write-Host ("  {0,-22} {1,8:N0} bytes" -f $_.Name, $_.Length)
}

# Shared with install-phone.ps1 so both report the same address. It prefers
# the wired interface and warns when the PC sits on more than one subnet —
# the failure that looks exactly like a broken protocol.
. (Join-Path $PSScriptRoot "Get-RigInfo.ps1")
Show-UcvRigInfo

Write-Host "Next: see experiment\docs\04-run-procedure.md"
