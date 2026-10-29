param(
    [Parameter(Mandatory=$true)][string]$RelayLog,
    [Parameter(Mandatory=$true)][string]$ServerLog,
    [Parameter(Mandatory=$true)][string]$DecisionMarker,
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'EpochDecision.ps1')
if (Test-Path -LiteralPath $DecisionMarker) { throw 'Marker already exists.' }
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
while ([DateTime]::UtcNow -lt $deadline) {
    if ((Test-Path -LiteralPath $RelayLog) -and (Test-Path -LiteralPath $ServerLog)) {
        $relay = Get-Content -LiteralPath $RelayLog -Raw
        $server = Get-Content -LiteralPath $ServerLog -Raw
        if ($null -ne $relay -and $null -ne $server) {
            $result = Find-EpochDecision $relay $server
            if ($null -ne $result) {
                New-Item -ItemType File -Path $DecisionMarker -ErrorAction Stop | Out-Null
                Write-Output "[PASS] correlated rejection packet=$($result.Packet) client=$($result.Client) old_epoch=$($result.Epoch) new_epoch=$($result.ReopenedEpoch) tick=$($result.Tick)"
                exit 0
            }
        }
    }
    Start-Sleep -Milliseconds 100
}
throw 'Timed out waiting for the correlated rejection while server admission is open.'
