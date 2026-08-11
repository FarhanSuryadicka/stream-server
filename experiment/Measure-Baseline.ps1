# Measures the bare network path to the phone, before any protocol runs.
#
# Why this is not optional: control RTT and transport latency are only
# interpretable against what the network was already doing on its own. On this
# rig the idle WiFi path showed p50 = 2 ms but p95 = 45 ms, so a protocol
# reporting a 45 ms p95 may be adding nothing whatsoever. Without a baseline
# that tail gets blamed on the protocol and the wrong one gets eliminated.
#
# ASCII only - Windows PowerShell 5.1 reads .ps1 as the ANSI codepage.
#
# Usage:  .\experiment\Measure-Baseline.ps1 -Phone 192.168.0.101

param(
    [Parameter(Mandatory = $true)][string]$Phone,
    [int]$Samples = 100,
    [string]$OutFile
)

function Get-Pct($sorted, $p) {
    if ($sorted.Count -eq 0) { return 0 }
    $i = [math]::Min([int][math]::Floor($p * ($sorted.Count - 1)), $sorted.Count - 1)
    return $sorted[$i]
}

Write-Host ""
Write-Host "Network baseline to $Phone ($Samples ICMP probes)"
Write-Host "================================================"

$rtt  = New-Object System.Collections.Generic.List[int]
$lost = 0

for ($i = 1; $i -le $Samples; $i++) {
    $r = Test-Connection -ComputerName $Phone -Count 1 -ErrorAction SilentlyContinue
    if ($r) { $rtt.Add([int]$r.ResponseTime) } else { $lost++ }
    if ($i % 20 -eq 0) { Write-Host "  $i/$Samples..." }
}

if ($rtt.Count -eq 0) {
    Write-Host "No replies. The phone may drop ICMP - not conclusive on its own." -ForegroundColor Yellow
    exit 1
}

$s    = $rtt | Sort-Object
$p50  = Get-Pct $s 0.50
$p95  = Get-Pct $s 0.95
$p99  = Get-Pct $s 0.99
$mean = [math]::Round(($rtt | Measure-Object -Average).Average, 2)
$mx   = ($rtt | Measure-Object -Maximum).Maximum
$mn   = ($rtt | Measure-Object -Minimum).Minimum
$lossPct = [math]::Round(100.0 * $lost / $Samples, 2)

Write-Host ""
Write-Host "  replies   : $($rtt.Count)/$Samples  (loss $lossPct %)"
Write-Host "  min/mean  : $mn / $mean ms"
Write-Host "  p50       : $p50 ms"
Write-Host "  p95       : $p95 ms"
Write-Host "  p99       : $p99 ms"
Write-Host "  max       : $mx ms"

Write-Host ""
# A tail this wide is characteristic of Android WiFi power-save: the radio
# sleeps between packets, so a sparse datagram waits for it to wake.
if ($p95 -gt ($p50 * 5) -and $p95 -gt 20) {
    Write-Host "HEAVY TAIL: p95 is $([math]::Round($p95 / [math]::Max($p50,1),1))x p50." -ForegroundColor Yellow
    Write-Host "The network path itself is bursty before any protocol is involved." -ForegroundColor Yellow
    Write-Host "Subtract this baseline before attributing a tail to a protocol," -ForegroundColor Yellow
    Write-Host "and do not rank protocols on a tail the network already produces." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Common causes, worth ruling out before measuring:" -ForegroundColor Yellow
    Write-Host "  - Android WiFi power-save (keep the screen on; disable battery" -ForegroundColor Yellow
    Write-Host "    optimisation for the app)"  -ForegroundColor Yellow
    Write-Host "  - Competing traffic on the band" -ForegroundColor Yellow
    Write-Host "  - Weak signal / distance from the router" -ForegroundColor Yellow
} else {
    Write-Host "Tail looks reasonable for WiFi." -ForegroundColor Green
}

$json = [ordered]@{
    type       = "network_baseline"
    phone      = $Phone
    samples    = $Samples
    replies    = $rtt.Count
    loss_pct   = $lossPct
    min_ms     = $mn
    mean_ms    = $mean
    p50_ms     = $p50
    p95_ms     = $p95
    p99_ms     = $p99
    max_ms     = $mx
    measured_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
} | ConvertTo-Json -Compress

if ($OutFile) {
    $json | Out-File -FilePath $OutFile -Encoding utf8
    Write-Host ""
    Write-Host "written: $OutFile"
} else {
    Write-Host ""
    Write-Host "Record this line in the run metadata:"
    Write-Host $json
}
