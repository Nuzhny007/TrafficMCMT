# Stage 7 DoD: clock synchronisation, out-of-order delivery and online mode.
#
# Scenario:
#   1. offline run  -> offline.jsonl (pseudo ground truth, identity = global_id)
#   2. online run on the clean stream -> baseline IDF1 relative to offline
#   3. tracklet_perturb: delivery-order jitter of +/-2 s and 5% adjacent swaps
#   4. online run on the perturbed stream -> perturbed IDF1
#   5. the same perturbed run is repeated and the JSONL outputs are compared
#      byte for byte (`online.clock = event`)
#
# DoD:
#   * online baseline matches offline (IDF1 drop <= 2%);
#   * perturbation does not drop IDF1 by more than 2% relative to the baseline;
#   * the watermark advances and windows are committed (it does not stick);
#   * the replay is byte-for-byte deterministic.
#
# Requires a Release build with protobuf enabled:
#   cmake --build --preset local-release
#
# Example:
#   powershell -ExecutionPolicy Bypass -File scripts/smoke_online.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/smoke_online.ps1 -JitterMs 2000 -Reorder 0.05

[CmdletBinding()]
param(
    [string]$Dataset = "build/datasets/auburn_toomer/combined.jsonl",
    [string]$OutDir = "build/smoke_online",
    [string]$MasterExe = "build/local-release/Release/trafficmcmt_master.exe",
    [string]$PerturbExe = "build/local-release/Release/trafficmcmt_tracklet_perturb.exe",
    [int]$JitterMs = 2000,
    [double]$Reorder = 0.05,
    [int]$Seed = 42,
    [int]$WindowMs = 30000,
    [int]$WindowOverlapMs = 10000,
    [int]$CommitLagMs = 15000,
    [int]$WarmupMs = 10000,
    [int]$MaxLatenessMs = 2000,
    [double]$MaxDropRatio = 0.02
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {
    if (-not (Test-Path $MasterExe)) {
        throw "Master not found: $MasterExe. Build it with: cmake --build --preset local-release"
    }
    if (-not (Test-Path $PerturbExe)) {
        throw "Perturbator not found: $PerturbExe. Build it with: cmake --build --preset local-release"
    }
    if (-not (Test-Path $Dataset)) {
        throw "Dataset not found: $Dataset. Prepare it with scripts/run_auburn_toomer.ps1"
    }

    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $OutDir = (Resolve-Path $OutDir).Path

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $writeText = {
        param([string]$Path, [string]$Text)
        [System.IO.File]::WriteAllText($Path, $Text, $utf8)
    }

    function New-Config {
        param([string]$Path, [string]$Mode, [string]$InputPath)
        $text = @"
master:
  mode: $Mode
  window_ms: $WindowMs
  window_overlap_ms: $WindowOverlapMs
  commit_lag_ms: $CommitLagMs
  warmup_ms: $WarmupMs
  retention_ms: 120000
  watermark:
    silent_timeout_ms: 5000
  clock_sync:
    enabled: true
    window_samples: 120
    min_samples: 5
    max_offset_ms: 10000
    max_drift_ppm: 1000
    assumed_transport_ms: 250
    outlier_sigma: 3.0
  online:
    clock: event
    idle_timeout_ms: 0
    max_events: 0
ingest:
  source: file
  max_lateness_ms: $MaxLatenessMs
  strict: false
  file:
    path: $InputPath
    speed: 1.0
    format: auto
reid:
  embedding_dim: 512
  index: brute
  top_k: 20
  min_similarity: 0.0
  exclude_same_camera: true
st:
  gate:
    mode: off
flow:
  solver: ssp
  link_reward: 1.0
  max_candidates_per_node: 15
  max_component_nodes: 20000
log:
  level: warn
  console: true
"@
        & $writeText $Path $text
    }

    function Invoke-Master {
        param([string]$Config, [string]$Out, [string]$Stats, [string]$GroundTruth)
        $arguments = @("--config", $Config, "--out", $Out, "--stats-out", $Stats)
        if ($GroundTruth) {
            $arguments += @("--gt", $GroundTruth)
        }
        $output = & $MasterExe @arguments 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "master failed ($LASTEXITCODE): $($output -join "`n")"
        }
    }

    function Read-Stats {
        param([string]$Path)
        return Get-Content -Raw $Path | ConvertFrom-Json
    }

    function Get-Idf1 {
        param($Stats)
        if ($null -eq $Stats.metrics) {
            return $null
        }
        return [double]$Stats.metrics.idf1
    }

    $datasetPath = (Resolve-Path $Dataset).Path
    $offlineConfig = Join-Path $OutDir "offline.yaml"
    $onlineCleanConfig = Join-Path $OutDir "online-clean.yaml"
    $onlinePertConfig = Join-Path $OutDir "online-perturbed.yaml"

    $offlineOut = Join-Path $OutDir "offline.jsonl"
    $offlineStats = Join-Path $OutDir "offline.stats.json"
    $cleanOut = Join-Path $OutDir "online-clean.jsonl"
    $cleanStats = Join-Path $OutDir "online-clean.stats.json"
    $perturbedInput = Join-Path $OutDir "perturbed.jsonl"
    $perturbedOut = Join-Path $OutDir "online-perturbed.jsonl"
    $perturbedStats = Join-Path $OutDir "online-perturbed.stats.json"
    $repeatOut = Join-Path $OutDir "online-perturbed.repeat.jsonl"
    $repeatStats = Join-Path $OutDir "online-perturbed.repeat.stats.json"

    Write-Host "[1/5] offline baseline -> $offlineOut"
    New-Config -Path $offlineConfig -Mode "offline" -InputPath $datasetPath
    Invoke-Master -Config $offlineConfig -Out $offlineOut -Stats $offlineStats
    $offline = Read-Stats $offlineStats
    Write-Host ("      groups={0} updates={1}" -f $offline.gallery, $offline.results_published)

    Write-Host "[2/5] online on the clean stream"
    New-Config -Path $onlineCleanConfig -Mode "online" -InputPath $datasetPath
    Invoke-Master -Config $onlineCleanConfig -Out $cleanOut -Stats $cleanStats -GroundTruth $offlineOut
    $clean = Read-Stats $cleanStats
    $cleanIdf1 = Get-Idf1 $clean
    if ($null -eq $cleanIdf1) {
        throw "online clean run produced no metrics"
    }
    Write-Host ("      idf1={0:F4} windows_solved={1} windows_committed={2} late={3} too_late={4}" -f `
        $cleanIdf1, $clean.windows_solved, $clean.windows_committed, $clean.late, $clean.too_late)

    Write-Host ("[3/5] perturb: jitter=+/-{0} ms, reorder={1:P0}, seed={2}" -f $JitterMs, $Reorder, $Seed)
    $perturbOutput = & $PerturbExe --in $datasetPath --out $perturbedInput `
        --jitter-ms $JitterMs --reorder $Reorder --seed $Seed 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "tracklet_perturb failed ($LASTEXITCODE): $($perturbOutput -join "`n")"
    }
    Write-Host ("      {0}" -f ($perturbOutput -join " "))

    Write-Host "[4/5] online on the perturbed stream"
    New-Config -Path $onlinePertConfig -Mode "online" -InputPath $perturbedInput
    Invoke-Master -Config $onlinePertConfig -Out $perturbedOut -Stats $perturbedStats -GroundTruth $offlineOut
    $perturbed = Read-Stats $perturbedStats
    $perturbedIdf1 = Get-Idf1 $perturbed
    if ($null -eq $perturbedIdf1) {
        throw "online perturbed run produced no metrics"
    }
    Write-Host ("      idf1={0:F4} windows_solved={1} windows_committed={2} late={3} too_late={4}" -f `
        $perturbedIdf1, $perturbed.windows_solved, $perturbed.windows_committed, $perturbed.late, $perturbed.too_late)

    Write-Host "[5/5] deterministic replay of the perturbed stream"
    Invoke-Master -Config $onlinePertConfig -Out $repeatOut -Stats $repeatStats -GroundTruth $offlineOut
    $repeat = Read-Stats $repeatStats
    $repeatIdf1 = Get-Idf1 $repeat

    $failures = @()

    $cleanDrop = if ($cleanIdf1 -gt 0.0) { ($cleanIdf1 - $perturbedIdf1) / $cleanIdf1 } else { 0.0 }
    if ($cleanDrop -gt $MaxDropRatio + 1e-9) {
        $failures += ("perturbation drop {0:P2} > {1:P2}" -f $cleanDrop, $MaxDropRatio)
    }
    if ($cleanIdf1 -lt 0.98) {
        $failures += ("online baseline idf1 {0:F4} < 0.98 relative to offline" -f $cleanIdf1)
    }
    if ($clean.windows_committed -le 0) {
        $failures += "online clean run committed no windows (watermark stuck)"
    }
    if ($perturbed.windows_committed -le 0) {
        $failures += "online perturbed run committed no windows (watermark stuck)"
    }
    if ($perturbed.too_late -gt 0) {
        $failures += ("online perturbed run dropped {0} too-late messages" -f $perturbed.too_late)
    }

    $cleanBytes = [System.IO.File]::ReadAllBytes($perturbedOut)
    $repeatBytes = [System.IO.File]::ReadAllBytes($repeatOut)
    $byteEqual = ($cleanBytes.Length -eq $repeatBytes.Length)
    if ($byteEqual) {
        for ($i = 0; $i -lt $cleanBytes.Length; ++$i) {
            if ($cleanBytes[$i] -ne $repeatBytes[$i]) {
                $byteEqual = $false
                break
            }
        }
    }
    if (-not $byteEqual) {
        $failures += "perturbed replay is not byte-for-byte deterministic"
    }
    if ([math]::Abs($repeatIdf1 - $perturbedIdf1) -gt 1e-12) {
        $failures += "repeat idf1 differs from the first perturbed run"
    }

    $mergesClean = [System.IO.File]::ReadAllBytes("$perturbedOut.merges.jsonl")
    $mergesRepeat = [System.IO.File]::ReadAllBytes("$repeatOut.merges.jsonl")
    $mergesEqual = ($mergesClean.Length -eq $mergesRepeat.Length)
    if ($mergesEqual) {
        for ($i = 0; $i -lt $mergesClean.Length; ++$i) {
            if ($mergesClean[$i] -ne $mergesRepeat[$i]) {
                $mergesEqual = $false
                break
            }
        }
    }
    if (-not $mergesEqual) {
        $failures += "merge stream is not byte-for-byte deterministic"
    }

    Write-Host ""
    Write-Host "Stage 7 DoD summary"
    Write-Host ("  offline groups        : {0}" -f $offline.gallery)
    Write-Host ("  online clean IDF1     : {0:F4}" -f $cleanIdf1)
    Write-Host ("  online perturbed IDF1 : {0:F4}" -f $perturbedIdf1)
    Write-Host ("  relative drop         : {0:P2} (limit {1:P2})" -f $cleanDrop, $MaxDropRatio)
    Write-Host ("  late / too_late       : {0} / {1}" -f $perturbed.late, $perturbed.too_late)
    Write-Host ("  windows committed     : {0}" -f $perturbed.windows_committed)
    Write-Host ("  deterministic replay  : {0}" -f $(if ($byteEqual -and $mergesEqual) { "yes" } else { "no" }))

    if ($failures.Count -gt 0) {
        Write-Host ""
        Write-Host "FAILED:"
        foreach ($failure in $failures) {
            Write-Host ("  - {0}" -f $failure)
        }
        exit 1
    }

    Write-Host ""
    Write-Host "Stage 7 DoD: PASSED"
    exit 0
}
finally {
    Pop-Location
}
