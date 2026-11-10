<#
.SYNOPSIS
    End-to-end ZeroMQ smoke test: broker + N synthetic Edge cameras + consumer.

.DESCRIPTION
    Starts the XSUB/XPUB broker, the Edge simulator and a tracklet consumer,
    then verifies that no messages were lost and reports consumer RSS
    stability. The default run (10 cameras x 100 tracklets/s x 60 s) is the
    short version of the stage 6 DoD; pass -Duration 3600 for the full hour.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/smoke_zmq.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/smoke_zmq.ps1 -Duration 3600 -BuildDir build/zmq-check
#>
[CmdletBinding()]
param(
    [int]$Duration = 60,
    [int]$Cameras = 10,
    [double]$Rate = 100.0,
    [string]$BuildDir = "build/zmq-check",
    [string]$Configuration = "Release",
    [int]$FrontendPort = 5555,
    [int]$BackendPort = 5556
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$binDir = Join-Path $repoRoot (Join-Path $BuildDir $Configuration)
$brokerExe = Join-Path $binDir "trafficmcmt_broker.exe"
$simulatorExe = Join-Path $binDir "trafficmcmt_edge_simulator.exe"
$inspectExe = Join-Path $binDir "trafficmcmt_tracklet_inspect.exe"

foreach ($exe in @($brokerExe, $simulatorExe, $inspectExe)) {
    if (-not (Test-Path $exe)) {
        throw "Missing $exe. Build the ZMQ configuration first, e.g. " +
              "cmake -S . -B $BuildDir -DTRAFFICMCMT_WITH_ZMQ=ON"
    }
}

$logDir = Join-Path $repoRoot (Join-Path $BuildDir "smoke")
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$brokerOut = Join-Path $logDir "broker.log"
$simulatorOut = Join-Path $logDir "edge_simulator.log"
$inspectOut = Join-Path $logDir "tracklet_inspect.log"

$broker = $null
$simulator = $null
$inspect = $null
$rssSamples = New-Object System.Collections.Generic.List[double]

function Stop-Child($process) {
    if ($null -ne $process -and -not $process.HasExited) {
        try {
            $process.Kill()
            $process.WaitForExit(5000) | Out-Null
        } catch {
            # The process may have already exited between the check and Kill.
        }
    }
}

try {
    $frontend = "tcp://127.0.0.1:$FrontendPort"
    $backend = "tcp://127.0.0.1:$BackendPort"

    Write-Host "broker: $frontend -> $backend"
    $broker = Start-Process -FilePath $brokerExe `
        -ArgumentList @("--frontend", "tcp://*:$FrontendPort", "--backend", "tcp://*:$BackendPort",
                        "--duration", "$($Duration + 15)", "--stats-interval", "10") `
        -RedirectStandardOutput $brokerOut -PassThru -NoNewWindow

    Start-Sleep -Milliseconds 700

    # Start the consumer first so its subscription is registered before the
    # first tracklets are published.
    Write-Host "consumer: $backend"
    $inspect = Start-Process -FilePath $inspectExe `
        -ArgumentList @("--source", "zmq", "--endpoint", $backend,
                        "--duration", "$($Duration + 5)", "--timeout-ms", "1000",
                        "--stats-interval", "10") `
        -RedirectStandardOutput $inspectOut -PassThru -NoNewWindow

    Start-Sleep -Milliseconds 700

    Write-Host "edge simulator: $Cameras cameras x $Rate tracklets/s for $Duration s"
    $simulator = Start-Process -FilePath $simulatorExe `
        -ArgumentList @("--endpoint", $frontend, "--cameras", "$Cameras", "--rate", "$Rate",
                        "--duration", "$Duration", "--stats-interval", "10") `
        -RedirectStandardOutput $simulatorOut -PassThru -NoNewWindow

    $deadline = (Get-Date).AddSeconds($Duration + 30)
    while ((Get-Date) -lt $deadline) {
        if ($null -ne $inspect -and -not $inspect.HasExited) {
            try {
                $rssSamples.Add((Get-Process -Id $inspect.Id).WorkingSet64)
            } catch {
                # Process exited between the check and Get-Process.
            }
        }
        if ($null -ne $simulator -and $simulator.HasExited -and $null -ne $inspect -and $inspect.HasExited) {
            break
        }
        Start-Sleep -Seconds 2
    }

    if ($null -ne $simulator -and -not $simulator.HasExited) {
        throw "edge simulator did not finish in time"
    }
    if ($null -ne $inspect -and -not $inspect.HasExited) {
        throw "tracklet consumer did not finish in time"
    }

    $simulatorLog = Get-Content $simulatorOut -Raw
    $inspectLog = Get-Content $inspectOut -Raw

    Write-Host ""
    Write-Host "--- edge simulator ---"
    Write-Host $simulatorLog.Trim()
    Write-Host ""
    Write-Host "--- consumer ---"
    Write-Host $inspectLog.Trim()
    Write-Host ""

    $received = 0
    $missing = 0
    $rejected = 0
    $malformed = 0
    if ($inspectLog -match "zmq_received:\s+(\d+)") { $received = [int64]$Matches[1] }
    if ($inspectLog -match "zmq_missing_total:\s+(\d+)") { $missing = [int64]$Matches[1] }
    if ($inspectLog -match "zmq_rejected:\s+(\d+)") { $rejected = [int64]$Matches[1] }
    if ($inspectLog -match "zmq_malformed:\s+(\d+)") { $malformed = [int64]$Matches[1] }

    $failures = 0
    if ($simulatorLog -match "failures=(\d+)") { $failures = [int64]$Matches[1] }

    Write-Host ("received={0} missing_total={1} rejected={2} malformed={3} send_failures={4}" -f `
        $received, $missing, $rejected, $malformed, $failures)

    if ($rssSamples.Count -ge 6) {
        # Ignore the warm-up fifth, then compare the mean of the first and the
        # second half of the steady window. A single min/max pair is dominated
        # by the bounded duplicate-detection cache (it fills and clears), while
        # the mean is a fair "is RSS stable?" measure.
        $steady = @($rssSamples | Select-Object -Skip ([math]::Floor($rssSamples.Count / 5)))
        $half = [math]::Floor($steady.Count / 2)
        $firstMean = ($steady[0..($half - 1)] | Measure-Object -Average).Average
        $secondMean = ($steady[$half..($steady.Count - 1)] | Measure-Object -Average).Average
        $min = ($steady | Measure-Object -Minimum).Minimum
        $max = ($steady | Measure-Object -Maximum).Maximum
        $spread = if ($firstMean -gt 0) { [math]::Abs($secondMean - $firstMean) / $firstMean * 100.0 } else { 0.0 }
        Write-Host ("consumer RSS: first_half={0:N2} MiB second_half={1:N2} MiB drift={2:N2}% (min={3:N1} max={4:N1} MiB)" -f `
            ($firstMean / 1MB), ($secondMean / 1MB), $spread, ($min / 1MB), ($max / 1MB))
        if ($spread -gt 5.0) {
            Write-Warning "consumer RSS drift is above 5% (stage 6 DoD target)"
        }
    }

    $ok = $true
    if ($received -le 0) {
        Write-Error "No tracklets were received" -ErrorAction Continue
        $ok = $false
    }
    if ($missing -ne 0) {
        Write-Error "missing_total is $missing, expected 0" -ErrorAction Continue
        $ok = $false
    }
    if ($rejected -ne 0 -or $malformed -ne 0) {
        Write-Error "rejected=$rejected malformed=$malformed, expected 0" -ErrorAction Continue
        $ok = $false
    }
    if ($failures -ne 0) {
        Write-Error "edge simulator reported $failures send failures" -ErrorAction Continue
        $ok = $false
    }

    if (-not $ok) {
        exit 1
    }

    Write-Host "smoke_zmq: OK"
    exit 0
}
finally {
    Stop-Child $simulator
    Stop-Child $inspect
    Stop-Child $broker
}
