# Pre-flight check before a measurement run.
#
# Catches the failures that otherwise look like "the protocol does not work":
# wrong subnet, firewall blocking the receiver ports, phone unreachable.
# Two minutes of a run producing nothing is a bad way to discover a typo.
#
# ASCII only, deliberately: Windows PowerShell 5.1 reads .ps1 as the system
# ANSI codepage, so a UTF-8 dash or quote here becomes mojibake that breaks
# string parsing.
#
# Usage:  .\experiment\Test-Connectivity.ps1 -Phone 192.168.0.51

param(
    [Parameter(Mandatory = $true)][string]$Phone,
    [int]$ControlPort = 8200,
    [int]$VideoPort   = 8201
)

. (Join-Path $PSScriptRoot "Get-RigInfo.ps1")

$ok = $true

Write-Host ""
Write-Host "Pre-flight check"
Write-Host "================"

# 1. Same subnet? This is the silent killer: the receiver simply waits and no
#    frames ever arrive, with no error raised anywhere.
$pcIp     = Get-UcvReceiverIp
$pcNet    = ($pcIp  -replace '\.\d+$', '')
$phoneNet = ($Phone -replace '\.\d+$', '')

Write-Host ""
Write-Host "[1] Subnet"
Write-Host "    PC    : $pcIp"
Write-Host "    Phone : $Phone"
if ($pcNet -eq $phoneNet) {
    Write-Host "    OK - same /24" -ForegroundColor Green
} else {
    Write-Host "    MISMATCH - $pcNet.x vs $phoneNet.x" -ForegroundColor Red
    Write-Host "    Frames will never arrive and nothing will report an error." -ForegroundColor Red
    $alt = Get-UcvNetCandidates | Where-Object { $_.IPAddress -match "^$([regex]::Escape($phoneNet))\." }
    if ($alt) {
        Write-Host "    This PC also has $($alt[0].IPAddress) on that subnet - use it instead." -ForegroundColor Yellow
    }
    $ok = $false
}

# 2. Phone reachable? Not conclusive on its own - plenty of Android builds
#    drop ICMP while UDP still flows.
Write-Host ""
Write-Host "[2] Reachability"
if (Test-Connection -ComputerName $Phone -Count 2 -Quiet -ErrorAction SilentlyContinue) {
    Write-Host "    OK - phone answers ping" -ForegroundColor Green
} else {
    Write-Host "    No ping reply." -ForegroundColor Yellow
    Write-Host "    Some Android builds drop ICMP but still pass UDP, so this" -ForegroundColor Yellow
    Write-Host "    alone is not conclusive. If step 4 also fails, the phone is" -ForegroundColor Yellow
    Write-Host "    genuinely unreachable." -ForegroundColor Yellow
}

# 3. Firewall. The receiver binds the video port, so inbound must be allowed.
Write-Host ""
Write-Host "[3] Windows Firewall (inbound UDP $VideoPort)"
$rules = Get-NetFirewallRule -Enabled True -Direction Inbound -Action Allow -ErrorAction SilentlyContinue |
         Where-Object {
             $pf = $_ | Get-NetFirewallPortFilter -ErrorAction SilentlyContinue
             $pf -and $pf.Protocol -eq 'UDP' -and
             ($pf.LocalPort -contains "$VideoPort" -or $pf.LocalPort -eq 'Any')
         }
if ($rules) {
    Write-Host "    A matching allow rule exists." -ForegroundColor Green
} else {
    Write-Host "    No explicit allow rule found for UDP $VideoPort." -ForegroundColor Yellow
    Write-Host "    Windows usually prompts on first bind - accept it for PRIVATE" -ForegroundColor Yellow
    Write-Host "    networks. If no prompt appears and no frames arrive, add one" -ForegroundColor Yellow
    Write-Host "    from an admin shell:" -ForegroundColor Yellow
    Write-Host "      New-NetFirewallRule -DisplayName ucv-receiver -Direction Inbound -Protocol UDP -LocalPort $VideoPort -Action Allow" -ForegroundColor Yellow
}

# 4. Is there a route to the phone control port at all?
Write-Host ""
Write-Host "[4] Control channel on the phone (UDP $ControlPort)"
Write-Host "    Probing..."
try {
    $udp = New-Object System.Net.Sockets.UdpClient
    $udp.Client.ReceiveTimeout = 1500
    $udp.Connect($Phone, $ControlPort)

    # A deliberately malformed 32-byte datagram. The phone handler validates
    # magic and CRC and drops it silently, so this tests only that the socket
    # is bound and routable. The real check is the receiver clock-sync step,
    # which aborts the run if the phone does not answer.
    $probe = New-Object byte[] 32
    [void]$udp.Send($probe, $probe.Length)
    Write-Host "    Datagram sent without error - route to the phone is open." -ForegroundColor Green
    Write-Host "    This does not prove the app is listening; the clock-sync" -ForegroundColor Gray
    Write-Host "    step in the receiver is the authoritative check." -ForegroundColor Gray
    $udp.Close()
} catch {
    Write-Host "    Send failed: $($_.Exception.Message)" -ForegroundColor Red
    $ok = $false
}

Write-Host ""
Write-Host "================"
if ($ok) {
    Write-Host "Pre-flight OK. Start the receiver:" -ForegroundColor Green
    Write-Host ""
    Write-Host "  .\experiment\receiver\build\ucv-receiver.exe --phone $Phone --run-id <id-from-phone> --duration 120 --out results"
} else {
    Write-Host "Pre-flight FAILED - fix the above before running." -ForegroundColor Red
    exit 1
}
