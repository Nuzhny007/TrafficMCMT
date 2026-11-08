# ReID chunk-split evaluation on the Aire single-camera dataset.
#
# Requires a Release build with protobuf enabled:
#   cmake --build --preset local-release
#
# Example:
#   powershell -ExecutionPolicy Bypass -File scripts/reid_eval_aire.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/reid_eval_aire.ps1 -Json

[CmdletBinding()]
param(
    [string]$Input = "build/datasets/aire/cam-01.jsonl",
    [string]$Exe = "build/local-release/Release/trafficmcmt_reid_eval.exe",
    [int]$MaxRank = 10,
    [double]$MinSimilarity = 0.0,
    [switch]$Json
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {
    if (-not (Test-Path $Exe)) {
        throw "Evaluator not found: $Exe. Build it with: cmake --build --preset local-release"
    }
    if (-not (Test-Path $Input)) {
        throw "Input not found: $Input"
    }

    $arguments = @(
        "--tracklets", $Input,
        "--max-rank", "$MaxRank",
        "--min-similarity", "$MinSimilarity"
    )
    if ($Json) {
        $arguments += "--json"
    }

    & $Exe @arguments
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
