# Reports the PC's network identity for a measurement run, and warns about
# the conditions that silently ruin results.
#
# Dot-source it to reuse Get-UcvReceiverIp:
#     . .\experiment\Get-RigInfo.ps1
#     $ip = Get-UcvReceiverIp

function Get-UcvNetCandidates {
    Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object {
            $_.IPAddress -notmatch '^127\.' -and       # loopback
            $_.IPAddress -notmatch '^169\.254\.' -and  # APIPA: no DHCP answered
            $_.InterfaceAlias -notmatch 'Loopback|Bluetooth'
        } |
        ForEach-Object {
            $ad = Get-NetAdapter -InterfaceIndex $_.InterfaceIndex -ErrorAction SilentlyContinue
            [pscustomobject]@{
                IPAddress = $_.IPAddress
                Interface = $_.InterfaceAlias
                Subnet    = ($_.IPAddress -replace '\.\d+$', '.0/24')
                LinkSpeed = if ($ad) { $ad.LinkSpeed } else { 'unknown' }
                IsWired   = ($ad -and $ad.InterfaceDescription -notmatch 'Wi-?Fi|Wireless|802\.11')
            }
        }
}

# The receiver should sit on the WIRED interface: the harness measures the
# phone's WiFi link, and running the PC on WiFi too would fold the PC's own
# radio jitter into every number (harness spec section 6.2).
function Get-UcvReceiverIp {
    $c = Get-UcvNetCandidates
    if (-not $c) { return $null }
    $wired = $c | Where-Object { $_.IsWired } | Select-Object -First 1
    if ($wired) { return $wired.IPAddress }
    return ($c | Select-Object -First 1).IPAddress
}

function Show-UcvRigInfo {
    $c = @(Get-UcvNetCandidates)

    Write-Host "`nPC network interfaces"
    Write-Host "---------------------"
    foreach ($n in $c) {
        $kind = if ($n.IsWired) { 'wired' } else { 'wireless' }
        Write-Host ("  {0,-15} {1,-12} {2,-10} {3}" -f $n.IPAddress, $n.Interface, $kind, $n.LinkSpeed)
    }

    if (-not $c) {
        Write-Warning "No usable IPv4 address. Check the cable / DHCP."
        return
    }

    $ip = Get-UcvReceiverIp
    Write-Host "`n  ==> Enter this on the phone: $ip`n"

    # A PC on two subnets is the classic silent failure: the phone joins the
    # router's subnet, you type the other address, and nothing arrives with
    # no error anywhere.
    $subnets = $c | Select-Object -ExpandProperty Subnet -Unique
    if ($subnets.Count -gt 1) {
        Write-Warning @"
This PC is on more than one subnet:
$($c | ForEach-Object { "    $($_.IPAddress)  ($($_.Interface))" } | Out-String)
The phone must be on the SAME subnet as the address you enter. If the phone
joins the router that this PC's LAN cable is plugged into, use the wired
address above - not the wireless one. Getting this wrong looks exactly like
a protocol failure: the receiver waits and no frames ever arrive.
"@
    }

    $wired = $c | Where-Object { $_.IsWired } | Select-Object -First 1
    if (-not $wired) {
        Write-Warning @"
No wired interface found. The harness expects the PC on Ethernet so that only
the phone's WiFi link is under test; on WiFi the PC's own radio jitter is
measured too. Record this against the run if you proceed.
"@
    } elseif ($wired.LinkSpeed -match '^(\d+)\s*Mbps$' -and [int]$Matches[1] -lt 1000) {
        Write-Host "note: Ethernet negotiated at $($wired.LinkSpeed)." -ForegroundColor Yellow
        Write-Host "      Fine for the 4 Mbps test stream, but record it in the run metadata." -ForegroundColor Yellow
    }
}

# Only run the display when invoked directly, not when dot-sourced.
if ($MyInvocation.InvocationName -ne '.') { Show-UcvRigInfo }
