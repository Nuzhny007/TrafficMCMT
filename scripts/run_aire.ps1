# Single-camera Edge smoke test on data/aire.
#
# Requires a Release build:
#   cmake --build --preset local-release
#
# Example:
#   powershell -ExecutionPolicy Bypass -File scripts/run_aire.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/run_aire.ps1 -MaxFrames 600

[CmdletBinding()]
param(
    [int]$StartFrame = 0,
    [int]$MaxFrames = 300,
    [string]$Detector = "configs/detectors/coco_yolov8s.ini",
    [long]$TimeBaseMs = 1620599040000,
    [string]$Exe = "build/local-release/Release/trafficmcmt_mtt_export.exe"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {
    if (-not (Test-Path $Exe)) {
        throw "Exporter not found: $Exe. Build it with: cmake --build --preset local-release"
    }

    $OutDir = Join-Path $RepoRoot "build/datasets/aire"
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    $Out = Join-Path $OutDir "cam-01.jsonl"

    $ExporterArgs = @(
        "--video", "../data/aire/Relaxing_highway_traffic.mp4",
        "--out", $Out,
        "--camera", "cam-01",
        "--session", "aire-$TimeBaseMs",
        "--detector-settings", $Detector,
        "--white-list", "person;bicycle;car;motorbike;bus;truck",
        "--geo-bind", "../data/aire/geo_bind.ini",
        "--reid-person-model", "../Multitarget-tracker/data/reid/osnet_x0_25_msmt17.onnx",
        "--reid-input", "128x256",
        "--start-epoch-ms", "$TimeBaseMs",
        "--start-frame", "$StartFrame",
        "--max-frames", "$MaxFrames"
    )

    & $Exe @ExporterArgs
    if ($LASTEXITCODE -ne 0) {
        throw "mtt_export failed with exit code $LASTEXITCODE"
    }

    $Inspector = Join-Path (Split-Path -Parent $Exe) "trafficmcmt_tracklet_inspect.exe"
    if (Test-Path $Inspector) {
        & $Inspector $Out
    }

    Write-Host ""
    Write-Host "Aire dataset written to: $OutDir"
} finally {
    Pop-Location
}
