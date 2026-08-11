# Builds the Android app and installs it on the connected phone.
#
# Usage:  .\experiment\install-phone.ps1
#         .\experiment\install-phone.ps1 -Clean    # after editing native code
#
# Install while the phone is on USB, THEN unplug and attach the camera — the
# phone has one USB port and the camera will take it.

param([switch]$Clean)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
if (-not (Test-Path $adb)) { Write-Error "adb not found at $adb" }

Write-Host "checking for a connected device..."
$devices = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\S' }
$online  = $devices | Where-Object { $_ -match '\sdevice$' }

if (-not $online) {
    $unauthorized = $devices | Where-Object { $_ -match 'unauthorized' }
    if ($unauthorized) {
        Write-Error @"
Phone is connected but NOT authorised.

Look at the phone screen and accept the "Allow USB debugging?" prompt,
then run this again.
"@
    }
    Write-Error @"
No device found.

  1. Enable Developer options: Settings > About phone > tap Build number 7x
  2. Enable USB debugging: Settings > Developer options > USB debugging
  3. Plug the phone into this PC and accept the authorisation prompt

Note: an emulator will not work. This app is arm64-only and needs USB host
support for the camera.
"@
}

Write-Host "device: $($online -join ', ')`n"

# Gradle's CMake integration has been observed serving a stale .so after
# native edits even when the task reports as executed (see CLAUDE.md), which
# shows up as UnsatisfiedLinkError for a symbol that is definitely there.
# -Clean sidesteps that.
if ($Clean) {
    Write-Host "clean build (native sources changed)..."
    & .\gradlew.bat :app:clean
    if ($LASTEXITCODE -ne 0) { Write-Error "clean failed" }
    Remove-Item -Recurse -Force "app\.cxx" -ErrorAction SilentlyContinue
}

Write-Host "building..."
& .\gradlew.bat :app:assembleDebug
if ($LASTEXITCODE -ne 0) { Write-Error "build failed" }

$apk = "app\build\outputs\apk\debug\app-debug.apk"
if (-not (Test-Path $apk)) { Write-Error "APK not found at $apk" }

# Install with adb rather than :app:installDebug.
#
# Xiaomi/HyperOS rejects a plain streamed install with
# INSTALL_FAILED_USER_RESTRICTED even with "Install via USB" enabled, but
# accepts the same APK with -t (allow test packages). Gradle's installDebug
# does not pass -t, so it fails on this device where adb succeeds.
Write-Host "installing..."
& $adb install -r -t $apk
if ($LASTEXITCODE -ne 0) {
    Write-Error @"
Install failed.

On Xiaomi/HyperOS, enable both of these under
Settings > Additional settings > Developer options:

  - Install via USB
  - USB debugging (Security settings)     <- often needs a Mi account + SIM

Then disable "Turn on MIUI optimization" if it is still refused.
"@
}

# Confirm the JNI surface actually made it into the .so. A missing symbol here
# means an UnsatisfiedLinkError at runtime on the phone, which is much harder
# to diagnose from the on-screen log than from here.
$nm = Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk\27.1.12297006\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-nm.exe" -ErrorAction SilentlyContinue
$so = Get-ChildItem "app\build" -Recurse -Filter "libuvcserver.so" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($nm -and $so) {
    $syms = & $nm.FullName -D $so.FullName 2>$null | Select-String "Java_com_anjas_uvcserver_UvcNative_"
    $expected = @("openDevice","closeDevice","startServer","stopServer","getLogText",
                  "startControl","stopControl","getControlState","setRunId",
                  "startExperiment","stopExperiment","getExperimentState")
    $missing = $expected | Where-Object { $syms -notmatch "UvcNative_$_`$" -and $syms -notmatch "UvcNative_$_\s" }
    Write-Host "`nJNI symbols in libuvcserver.so: $($syms.Count)/$($expected.Count)"
    if ($syms.Count -lt $expected.Count) {
        Write-Warning "Fewer symbols than expected - if the app throws UnsatisfiedLinkError, re-run with -Clean"
    }
}

Write-Host "`nInstalled."

# Prefers the wired interface and warns on multi-subnet — the PC is expected
# on Ethernet so only the phone's WiFi link is under test.
. (Join-Path $PSScriptRoot "Get-RigInfo.ps1")
Show-UcvRigInfo

Write-Host @"
Next:
  1. Unplug the phone from this PC
  2. Attach the USB camera via OTG
  3. Join the phone to the WiFi of the router this PC is cabled to
  4. Enter the wired IP shown above on the phone

Then follow experiment\docs\04-run-procedure.md
"@
