# Two-camera Edge test on data/auburn_toomer/sync_videos.
#
# Both cameras are processed with the same time base and the same ENU origin,
# so their tracklet timestamps and world coordinates are directly comparable.
# The default window starts at 60 s (frame 1800 at 30 fps), where the black
# Jeep and pedestrians are visible from both viewpoints.
#
# Requires a Release build:
#   cmake --build --preset local-release
#
# Example:
#   powershell -ExecutionPolicy Bypass -File scripts/run_auburn_toomer.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/run_auburn_toomer.ps1 -MaxFrames 600
#   powershell -ExecutionPolicy Bypass -File scripts/run_auburn_toomer.ps1 -Full

[CmdletBinding()]
param(
    [int]$StartFrame = 1800,
    [int]$MaxFrames = 300,
    [switch]$Full,
    [string]$Detector = "configs/detectors/coco_yolov8s.ini",
    [long]$TimeBaseMs = 1620599040000,
    [string]$GeoOrigin = "32.606456,-85.481891",
    [string]$Exe = "build/local-release/Release/trafficmcmt_mtt_export.exe"
)

$ErrorActionPreference = "Stop"
if ($Full) {
    $StartFrame = 0
    $MaxFrames = 0
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {
    if (-not (Test-Path $Exe)) {
        throw "Exporter not found: $Exe. Build it with: cmake --build --preset local-release"
    }

    $OutDir = Join-Path $RepoRoot "build/datasets/auburn_toomer"
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    $Cameras = @(
        @{ Id = "cam-01"; Video = "../data/auburn_toomer/sync_videos/1_2021-05-09_22_24.mp4";
           Geo = "../data/auburn_toomer/sync_videos/1_2021-05-09_22_24.ini" },
        @{ Id = "cam-02"; Video = "../data/auburn_toomer/sync_videos/2_2021-05-09_22_24.mp4";
           Geo = "../data/auburn_toomer/sync_videos/2_2021-05-09_22_24.ini" }
    )

    $Outputs = @()
    foreach ($Camera in $Cameras) {
        $Out = Join-Path $OutDir ("{0}.jsonl" -f $Camera.Id)
        $Outputs += $Out

        $ExporterArgs = @(
            "--video", $Camera.Video,
            "--out", $Out,
            "--camera", $Camera.Id,
            "--session", "auburn-toomer-$TimeBaseMs",
            "--detector-settings", $Detector,
            "--white-list", "person;bicycle;car;motorbike;bus;truck",
            "--geo-bind", $Camera.Geo,
            "--geo-origin", $GeoOrigin,
            "--reid-person-model", "../Multitarget-tracker/data/reid/osnet_x0_25_msmt17.onnx",
            "--reid-input", "128x256",
            "--start-epoch-ms", "$TimeBaseMs",
            "--start-frame", "$StartFrame",
            "--max-frames", "$MaxFrames"
        )

        Write-Host ("=== {0}: {1}" -f $Camera.Id, $Camera.Video)
        & $Exe @ExporterArgs
        if ($LASTEXITCODE -ne 0) {
            throw "mtt_export failed for $($Camera.Id) with exit code $LASTEXITCODE"
        }
    }

    # JSONL concatenation is valid JSONL: one tracklet per line.
    $Combined = Join-Path $OutDir "combined.jsonl"
    if (Test-Path $Combined) {
        Remove-Item $Combined
    }
    foreach ($Output in $Outputs) {
        Get-Content $Output | Add-Content $Combined
    }

    $Inspector = Join-Path (Split-Path -Parent $Exe) "trafficmcmt_tracklet_inspect.exe"
    if (Test-Path $Inspector) {
        foreach ($Output in ($Outputs + $Combined)) {
            Write-Host ""
            Write-Host ("=== inspect: {0}" -f $Output)
            & $Inspector $Output
        }
    }

    Write-Host ""
    Write-Host "Auburn dataset written to: $OutDir"
} finally {
    Pop-Location
}
