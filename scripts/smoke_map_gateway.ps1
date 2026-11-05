# Smoke test for `trafficmcmt_map_gateway`.
#
# Starts the gateway on a loopback port, connects with a real WebSocket client,
# reads frames for a few seconds and prints a summary. Verifies the end-to-end
# path: tracklets -> MapFeed -> WebSocket -> JSON (viewer protocol v1).
#
# Example:
#   powershell -ExecutionPolicy Bypass -File scripts/smoke_map_gateway.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/smoke_map_gateway.ps1 -InputFile build/datasets/aire/cam-01.jsonl -Config configs/map_feed_aire.yaml

[CmdletBinding()]
param(
    [string]$InputFile = "build/datasets/auburn_toomer/combined.jsonl",
    [string]$Config = "configs/map_feed.yaml",
    [int]$Port = 18080,
    [int]$Seconds = 6,
    [string]$Exe = "build/local-release/Release/trafficmcmt_map_gateway.exe"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot

$gateway = $null
$socket = $null
try {
    if (-not (Test-Path $Exe)) {
        throw "Gateway not found: $Exe. Build it with: cmake --build --preset local-release"
    }
    if (-not (Test-Path $InputFile)) {
        throw "Input not found: $InputFile"
    }

    $gatewayArgs = @(
        "--input", $InputFile,
        "--config", $Config,
        "--host", "127.0.0.1",
        "--port", "$Port",
        "--speed", "1"
    )
    $gateway = Start-Process -FilePath $Exe -ArgumentList $gatewayArgs -PassThru -WindowStyle Hidden

    $uri = [Uri]("ws://127.0.0.1:{0}/ws" -f $Port)
    $deadline = (Get-Date).AddSeconds(10)
    while ($true) {
        $socket = New-Object System.Net.WebSockets.ClientWebSocket
        try {
            $socket.ConnectAsync($uri, [Threading.CancellationToken]::None).Wait()
            break
        } catch {
            $socket.Dispose()
            $socket = $null
            if ((Get-Date) -gt $deadline) {
                throw "Cannot connect to $uri"
            }
            Start-Sleep -Milliseconds 200
        }
    }

    $counts = @{}
    $maxSpeed = 0.0
    $buffer = New-Object byte[] 65536
    $segment = New-Object 'System.ArraySegment[byte]' -ArgumentList @(, $buffer)
    $end = (Get-Date).AddSeconds($Seconds)

    while ((Get-Date) -lt $end) {
        $task = $socket.ReceiveAsync($segment, [Threading.CancellationToken]::None)
        if (-not $task.Wait(3000)) {
            break
        }
        $result = $task.Result
        if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
            break
        }
        $text = [Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)
        $frame = $text | ConvertFrom-Json
        if (-not $counts.ContainsKey($frame.type)) {
            $counts[$frame.type] = 0
        }
        $counts[$frame.type] += 1
        if ($frame.type -eq 'tracks_update') {
            foreach ($track in $frame.tracks) {
                if ($null -ne $track.speed_kmh -and [double]$track.speed_kmh -gt $maxSpeed) {
                    $maxSpeed = [double]$track.speed_kmh
                }
            }
        }
    }

    $total = ($counts.Values | Measure-Object -Sum).Sum
    Write-Host ("gateway smoke: frames={0} max_speed_kmh={1}" -f $total, [Math]::Round($maxSpeed, 2))
    $counts.GetEnumerator() | Sort-Object Name | ForEach-Object {
        Write-Host ("  {0}: {1}" -f $_.Name, $_.Value)
    }

    if (-not $counts.ContainsKey('camera_zones')) {
        throw "No camera_zones frame received"
    }
    if (-not $counts.ContainsKey('tracks_update')) {
        throw "No tracks_update frame received"
    }
} finally {
    if ($socket) {
        try { $socket.Dispose() } catch { }
    }
    if ($gateway -and -not $gateway.HasExited) {
        Stop-Process -Id $gateway.Id -Force
    }
    Pop-Location
}
