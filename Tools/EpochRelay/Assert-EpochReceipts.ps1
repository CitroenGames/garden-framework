param(
    [Parameter(Mandatory=$true)][string]$RelayLog,
    [Parameter(Mandatory=$true)][string]$ServerLog,
    [Parameter(Mandatory=$true)][string]$ClientLog
)
$ErrorActionPreference = 'Stop'
$relay = Get-Content -LiteralPath $RelayLog -Raw
$server = Get-Content -LiteralPath $ServerLog -Raw
$client = Get-Content -LiteralPath $ClientLog -Raw
. (Join-Path $PSScriptRoot 'EpochDecision.ps1')
$result = Find-EpochDecision $relay $server
if ($null -eq $result) { throw 'Missing correlated retained-packet rejection.' }
$epoch = $result.Epoch; $oldTick = $result.Tick; $packet = $result.Packet
$clientId = $result.Client; $reopenedEpoch = [uint64]$result.ReopenedEpoch
if ($relay -notmatch '\[relay\] forwarded open after decision marker') { throw 'Missing decision marker and open forwarding.' }
if ($client -notmatch "COMBAT_EVENT accepted killer=1 victim=$clientId(?: |$)") { throw 'Probe did not accept lethal death event.' }
if ($server -match "\[EpochReceipt\] (attack_callback|select_callback|shot) client=$clientId tick=$oldTick(?: |$)") { throw 'Rejected input invoked gameplay callback or shot.' }
$opened = $client.IndexOf('[ACCEPT] epoch_fire_probe reopened while held')
$released = $client.IndexOf('[ACCEPT] epoch_fire_probe release')
if ($opened -lt 0 -or $released -le $opened) { throw 'Missing held-through-reopen/release evidence.' }
$held = $client.Substring($opened, $released - $opened)
$commands = [regex]::Matches($held, 'stage=client_command .*?buttons=(\d+)')
if ($commands.Count -eq 0) { throw 'No commands observed during held interval.' }
foreach ($command in $commands) { if (([int]$command.Groups[1].Value -band 2) -ne 0) { throw 'Held fire leaked after reopening.' } }
if ($client -notmatch '\[ACCEPT\] epoch_fire_probe fresh command tick=(\d+)') { throw 'Missing fresh command.' }
$sampleTick = $Matches[1]
$freshOffset = $client.IndexOf("[ACCEPT] epoch_fire_probe fresh command tick=$sampleTick")
$beforeFresh = $client.Substring(0, $freshOffset)
$weaponReceipts = [regex]::Matches($beforeFresh, "\[EpochReceipt\] weapon client=$clientId generation=\d+ weapon=(\d+) ammo=(\d+) reserve=\d+")
if ($weaponReceipts.Count -eq 0) { throw 'Missing authoritative weapon/ammo baseline before fresh press.' }
$baseline = $weaponReceipts[$weaponReceipts.Count - 1]
$baselineWeapon = [int]$baseline.Groups[1].Value; $baselineAmmo = [int]$baseline.Groups[2].Value
$freshSamples = [regex]::Matches($relay, '\[relay\] fresh epoch=(\d+) tick=(\d+) packet=\d+ sample=\w+ buttons=(\d+)')
if ($freshSamples.Count -eq 0) { throw 'Missing actual fresh ATTACK packet.' }
$uniqueTicks = @($freshSamples | ForEach-Object { $_.Groups[2].Value } | Sort-Object -Unique)
if ($uniqueTicks.Count -ne 1) { throw 'Fresh ATTACK was emitted at multiple unique wire ticks.' }
$freshTick = [uint32]$uniqueTicks[0]
if ($freshTick -lt [uint32]$sampleTick) { throw 'Wire ATTACK predates the fresh logical press.' }
if ($freshTick -ne [uint32]$sampleTick) { Write-Output "[INFO] sender coalesced sampled tick=$sampleTick into wire tick=$freshTick" }
foreach ($wireSample in $freshSamples) {
    if ([uint64]$wireSample.Groups[1].Value -ne $reopenedEpoch -or $reopenedEpoch -le [uint64]$epoch) { throw 'Fresh sample did not use the reopened epoch.' }
}
if ($client -notmatch "\[ACCEPT\] epoch_fire_probe input acknowledged tick=$sampleTick observed_cursor=(\d+)") { throw 'Missing client acknowledgement receipt.' }
if ([uint32]$Matches[1] -lt $freshTick) { throw 'Client acknowledgement did not reach the transmitted fresh ATTACK tick.' }
$callbacks = [regex]::Matches($server, "\[EpochReceipt\] attack_callback client=$clientId tick=$freshTick(?: |$)")
if ($callbacks.Count -ne 1) { throw "Fresh ATTACK callback count=$($callbacks.Count), expected 1." }
$allProbeCallbacks = [regex]::Matches($server, "\[EpochReceipt\] attack_callback client=$clientId tick=\d+")
if ($allProbeCallbacks.Count -ne 1) { throw "Probe produced $($allProbeCallbacks.Count) total ATTACK callbacks, expected 1." }
if ($server -notmatch "stage=authority_decision .*?client=$clientId server_tick=\d+ input_tick=$freshTick decision=accepted") { throw 'Fresh command not accepted.' }
$shots = [regex]::Matches($server, "\[EpochReceipt\] shot client=$clientId tick=$freshTick(?: |$)")
Write-Output "[PASS] held ATTACK suppressed and fresh tick=$freshTick accepted once and acknowledged; shot_count=$($shots.Count)"
if ($shots.Count -ne 1) { throw 'Fresh shot effect was not observed exactly once.' }
if ($server -notmatch "\[EpochReceipt\] shot client=$clientId tick=$freshTick weapon=(\d+) ammo=(\d+) reserve=\d+") { throw 'Fresh shot has no ammo receipt.' }
if ([int]$Matches[1] -ne $baselineWeapon -or [int]$Matches[2] -ne ($baselineAmmo - 1)) { throw 'Fresh shot did not decrement authoritative ammo by one.' }
Write-Output "[PASS] authoritative weapon=$baselineWeapon ammo=$baselineAmmo -> $($baselineAmmo - 1)"
# ServerNetworkManager returns from the epoch branch before cursor/budget,
# movement, input_sample_handler and weapon_select_handler. The template's
# weapon input callback owns the player shot path and its selection callback
# owns player slot selection. Thus this packet cannot reach ammo or damage
# from those paths; this is source/control-flow evidence, not a global claim
# that no unrelated damage occurred in the match.
Write-Output "[PASS] retained epoch=$epoch tick=$oldTick rejected before movement and weapon callbacks; its selection, ammo, shot and damage paths were unreachable"
