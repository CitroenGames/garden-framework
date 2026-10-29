param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [int]$ServerPort = 28777,
    [int]$RelayPort = 28778
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Path $RunDirectory -ErrorAction Stop | Out-Null
$run = (Resolve-Path $RunDirectory).Path
$marker = Join-Path $run 'decision.ok'
$serverLog = Join-Path $run 'server.log'; $relayLog = Join-Path $run 'relay.log'
$clientLog = Join-Path $run 'probe.log'; $otherLog = Join-Path $run 'opponent.log'
$attackerScript = Join-Path $run 'opponent.actions.txt'; $probeScript = Join-Path $run 'probe.actions.txt'
# Follow the south corridor around the central solid, then face the probe at short range.
# de_block01.map.json puts the central solid at x=26..38, z=36..42;
# Both endpoints (47,43) and (50,43) are beyond it and clear of the A-site cover.
@('team attackers','move_to_live 24 45 1 12000','move_to_live 40 45 1 12000','move_to_live 47 43 1 12000','wait 12000','aim_fire_at_remote_live 2 60000','input 500 reload','aim_fire_at_remote_live 2 60000','wait 60000') | Set-Content $attackerScript
@('team defenders','move_to_live 50 24 1 12000','move_to_live 50 43 1 12000','wait 1000','epoch_fire_probe 160000','wait 60000') | Set-Content $probeScript
$project = Join-Path $root 'Templates\FPSShooter\FPSShooter.garden'
$children = @()
try {
    $env:GARDEN_MOVEMENT_TRACE = 'epoch-acceptance'; $env:GARDEN_ADMISSION_TRACE = '1'
    $env:FPS_GAME_MODE = 'competitive'; $env:FPS_BOTS = '0'; $env:FPS_WARMUP_SECONDS = '1'; $env:FPS_ROUND_SECONDS = '170'; $env:FPS_FREEZE_SECONDS = '6'
    $children += Start-Process (Join-Path $root 'bin\Server.exe') -ArgumentList @('--project', $project, '--port', $ServerPort) -WorkingDirectory $root -RedirectStandardOutput $serverLog -RedirectStandardError (Join-Path $run 'server.err') -WindowStyle Hidden -PassThru
    $children += Start-Process (Join-Path $root 'bin\EpochRelay.exe') -ArgumentList @($RelayPort, '127.0.0.1', $ServerPort, $marker) -WorkingDirectory $root -RedirectStandardOutput $relayLog -RedirectStandardError (Join-Path $run 'relay.err') -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 3
    $env:FPS_TEST_ACTION_SCRIPT = $attackerScript
    $children += Start-Process (Join-Path $root 'bin\Game.exe') -ArgumentList @('--project', $project, '--connect', '127.0.0.1', '--port', $ServerPort, '-d3d12') -WorkingDirectory $root -RedirectStandardOutput $otherLog -RedirectStandardError (Join-Path $run 'opponent.err') -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 2
    $env:FPS_TEST_ACTION_SCRIPT = $probeScript
    $children += Start-Process (Join-Path $root 'bin\Game.exe') -ArgumentList @('--project', $project, '--connect', '127.0.0.1', '--port', $RelayPort, '-d3d12') -WorkingDirectory $root -RedirectStandardOutput $clientLog -RedirectStandardError (Join-Path $run 'probe.err') -WindowStyle Hidden -PassThru
    & (Join-Path $PSScriptRoot 'Wait-EpochDecision.ps1') -RelayLog $relayLog -ServerLog $serverLog -DecisionMarker $marker -TimeoutSeconds 165
    . (Join-Path $PSScriptRoot 'EpochDecision.ps1')
    $decision = Find-EpochDecision (Get-Content -LiteralPath $relayLog -Raw) (Get-Content -LiteralPath $serverLog -Raw)
    if ($null -eq $decision) { throw 'Correlated rejection disappeared after the decision marker was created.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $missing = @()
    while ([DateTime]::UtcNow -lt $deadline) {
        $relay = Get-Content -LiteralPath $relayLog -Raw
        $client = Get-Content -LiteralPath $clientLog -Raw
        $missing = @()
        if ($relay -notmatch '\[relay\] forwarded open after decision marker') { $missing += 'relay marker-forward receipt' }
        if ($client -notmatch "COMBAT_EVENT accepted killer=1 victim=$($decision.Client)(?: |$)") { $missing += 'client lethal COMBAT_EVENT acceptance' }
        if ($client -notmatch '\[ACCEPT\] epoch_fire_probe input acknowledged') { $missing += 'client input acknowledgement' }
        if ($missing.Count -eq 0) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($missing.Count -ne 0) { throw "Timed out waiting for completion records: $($missing -join ', ')." }
    # Freeze all receipts before reading them together: the trace producers keep
    # writing after the completion records have appeared.
    foreach ($child in $children) {
        if ($child -and !$child.HasExited) { Stop-Process -InputObject $child -ErrorAction SilentlyContinue }
    }
    foreach ($child in $children) {
        if ($child -and !$child.WaitForExit(5000)) { throw "Timed out stopping runner-owned process $($child.Id)." }
    }
    & (Join-Path $PSScriptRoot 'Assert-EpochReceipts.ps1') -RelayLog $relayLog -ServerLog $serverLog -ClientLog $clientLog
} finally {
    foreach ($child in $children) { if ($child -and !$child.HasExited) { Stop-Process -InputObject $child -ErrorAction SilentlyContinue } }
}
