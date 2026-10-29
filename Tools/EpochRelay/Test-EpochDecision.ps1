$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'EpochDecision.ps1')
$relay = "[relay] held epoch=3 tick=1243 buttons=16514 weapon_select=2 forward=1 right=0`n[relay] released held_epoch=3 server_epoch=6`n"
$server = @'
Player 2 killed by player 1 sequence=1
Player 2 respawned at (57,1.2,8)
movement_trace stage=authority_received monotonic_ns=1 clock=steady_process role=server packet=1502 sample=2 client=2 server_tick=5292 input_tick=1243 epoch=3 buttons=16514
movement_trace stage=authority_decision monotonic_ns=2 clock=steady_process role=server packet=1502 sample=2 client=2 server_tick=5292 input_tick=1243 decision=rejected reason=admission_epoch_or_closed
movement_trace stage=authority_rejected_state monotonic_ns=3 clock=steady_process role=server packet=1502 client=2 server_tick=5292 packet_epoch=3 admission_epoch=6 open=true input_tick=1243 cursor_before=1056 cursor_after=1056 budget_before=7 budget_after=7 budget_tick_before=1729 budget_tick_after=1729
'@
$server += "`n"
$result = Find-EpochDecision $relay $server
if ($null -eq $result -or $result.Packet -ne '1502' -or $result.Sample -ne '2' -or $result.ReopenedEpoch -ne '6') { throw 'Valid receipt did not correlate.' }
if ($null -ne (Find-EpochDecision $relay ($server -replace 'packet=1502 client=2 server_tick=5292 packet_epoch', 'packet=1503 client=2 server_tick=5292 packet_epoch'))) { throw 'Different packet correlated.' }
if ($null -ne (Find-EpochDecision $relay ($server -replace 'input_tick=1243 decision=rejected', 'input_tick=1244 decision=rejected'))) { throw 'Different decision tick correlated.' }
$lines = $server.TrimEnd("`r", "`n") -split "`r?`n"
$outOfOrder = @($lines[0], $lines[1], $lines[3], $lines[2], $lines[4]) -join "`n"
foreach ($broken in @(
    ($server -replace 'cursor_after=1056', 'cursor_after=1057'),
    ($server -replace 'admission_epoch=6', 'admission_epoch=7'),
    ($server + "`nmovement_trace stage=authority_step role=server packet=1502 sample=2"),
    ($server -replace 'Player 2 respawned at', 'Player 3 respawned at'),
    ($outOfOrder + "`n")
)) {
    $threw = $false
    try { Find-EpochDecision $relay $broken | Out-Null } catch { $threw = $true }
    if (!$threw) { throw 'Contradictory receipt was accepted.' }
}
Write-Output '[PASS] epoch decision correlation fixtures'
