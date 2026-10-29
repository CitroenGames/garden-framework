# Shared correlation for the retained primary and its authority rejection.
# Return $null while receipts are incomplete; throw on contradictory completed receipts.
function Find-EpochDecision([string]$Relay, [string]$Server) {
    $held = [regex]::Match($Relay, '(?m)^\[relay\] held epoch=(\d+) tick=(\d+) buttons=(\d+) weapon_select=(\d+) forward=(\S+) right=\S+\r?$')
    if (!$held.Success) { return $null }
    $epoch = $held.Groups[1].Value; $tick = $held.Groups[2].Value
    $buttons = [uint32]$held.Groups[3].Value; $selection = [uint32]$held.Groups[4].Value
    $forward = 0.0
    if (($buttons -band 2) -eq 0 -or $selection -eq 0 -or
        ![double]::TryParse($held.Groups[5].Value, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$forward) -or [double]::IsNaN($forward) -or [double]::IsInfinity($forward) -or $forward -eq 0) {
        throw 'Retained primary does not contain attack, selection and forward movement.'
    }
    $release = [regex]::Match($Relay, '(?m)^\[relay\] released held_epoch=(\d+) server_epoch=(\d+)\r?$')
    if (!$release.Success) { return $null }
    if ($release.Groups[1].Value -ne $epoch -or [uint64]$release.Groups[2].Value -le [uint64]$epoch) { throw 'Release epoch does not match the retained command.' }
    $reopened = $release.Groups[2].Value
    $received = [regex]::Matches($Server, "(?m)stage=authority_received [^\r\n]*?packet=(\d+) sample=(\d+) client=(\d+) server_tick=(\d+) input_tick=$tick epoch=$epoch(?=\s|$)")
    foreach ($receipt in $received) {
        $packet = $receipt.Groups[1].Value; $sample = $receipt.Groups[2].Value; $client = $receipt.Groups[3].Value
        $decision = [regex]::Match($Server, "(?m)stage=authority_decision [^\r\n]*?packet=$packet sample=$sample client=$client server_tick=\d+ input_tick=$tick decision=rejected reason=admission_epoch_or_closed(?=\s|$)")
        $state = [regex]::Match($Server, "(?m)stage=authority_rejected_state [^\r\n]*?packet=$packet client=$client server_tick=\d+ packet_epoch=$epoch admission_epoch=(\d+) open=true input_tick=$tick cursor_before=(\d+) cursor_after=(\d+) budget_before=(\d+) budget_after=(\d+) budget_tick_before=(\d+) budget_tick_after=(\d+)\r?\n")
        if (!$decision.Success -or !$state.Success) { continue }
        if ($decision.Index -le $receipt.Index -or $state.Index -le $decision.Index) { throw 'Rejection receipts are out of order.' }
        if ($state.Groups[1].Value -ne $reopened -or [uint64]$state.Groups[1].Value -le [uint64]$epoch) { throw 'Rejection did not occur in the released open epoch.' }
        if ($state.Groups[2].Value -ne $state.Groups[3].Value -or $state.Groups[4].Value -ne $state.Groups[5].Value -or $state.Groups[6].Value -ne $state.Groups[7].Value) { throw 'Rejected packet changed cursor or budget.' }
        if ([regex]::IsMatch($Server, "(?m)stage=authority_step [^\r\n]*?packet=$packet(?=\s|$)")) { throw 'Rejected packet caused a movement step.' }
        $kill = [regex]::Match($Server, "Player $client killed by player 1 sequence=\d+")
        if (!$kill.Success) { throw 'Admission closed without the required observed lethal kill.' }
        $respawn = [regex]::Match($Server.Substring($kill.Index), "Player $client respawned at")
        if (!$respawn.Success -or ($kill.Index + $respawn.Index) -ge $receipt.Index) { throw 'Missing post-kill respawn before retained packet receipt.' }
        return [pscustomobject]@{ Epoch=$epoch; Tick=$tick; Packet=$packet; Sample=$sample; Client=$client; ReopenedEpoch=$reopened }
    }
    return $null
}
