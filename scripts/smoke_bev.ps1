# Stage 8 DoD: BEV projection, overlap deduplication, speed/course, world output.
#
# Scenario:
#   * synthetic stream: `Objects` cars moving east at `SpeedMps`, each seen by
#     two cameras (cam-01 -> cam-02) with a `HandoffMs` handoff and a small
#     temporal `GapMs` between the segments;
#   * bounding boxes are bottom-centre, so the projected point is the car's
#     ground position; the stream carries no world coordinates (valid=false),
#     they are produced by the homography on ingest;
#   * the same scenario is replayed for 1920x1080 and 1280x720 with
#     resolution-specific normalised homographies.
#
# DoD:
#   * projection error on control points < 0.5 m;
#   * overlap duplicates suppressed >= 90% of the expected ones;
#   * speed accuracy within +/-10%;
#   * camera resolution does not change the st-ReID partition;
#   * GeoJSON and CSV world outputs are written in the documented format.
#
# Requires a Release build:
#   cmake --build --preset local-release
#
# Example:
#   powershell -ExecutionPolicy Bypass -File scripts/smoke_bev.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/smoke_bev.ps1 -Objects 16 -SpeedMps 3

[CmdletBinding()]
param(
    [string]$OutDir = "build/smoke_bev",
    [string]$MasterExe = "build/local-release/Release/trafficmcmt_master.exe",
    [int]$Objects = 8,
    [double]$SpeedMps = 2.0,
    [int]$HandoffMs = 8000,
    [int]$GapMs = 100,
    [int]$DurationMs = 16000,
    [int]$StepMs = 100,
    [int]$DuplicateWindowMs = 500,
    [double]$DuplicateDistanceM = 1.5,
    [double]$MaxProjectionErrorM = 0.5,
    [double]$SpeedTolerance = 0.10,
    [double]$MinDedupRatio = 0.90
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {
    if (-not (Test-Path $MasterExe)) {
        throw "Master not found: $MasterExe. Build it with: cmake --build --preset local-release"
    }
    if ($Objects -le 0) {
        throw "Objects must be > 0"
    }
    if ($StepMs -le 0 -or $DurationMs -le $HandoffMs) {
        throw "invalid timeline: need StepMs > 0 and DurationMs > HandoffMs"
    }

    New-Item -ItemType Directory -Force $OutDir | Out-Null
    $OutDir = (Resolve-Path $OutDir).Path
    $slash = { param([string]$Path) return $Path.Replace('\', '/') }

    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $writeText = {
        param([string]$Path, [string]$Text)
        [System.IO.File]::WriteAllText($Path, $Text, $utf8)
    }

    # Lane spacing and world extent are tied to the synthetic homography:
    # x_norm = east / 100, y_norm = 1 - north / 100.
    $worldExtent = 100.0
    $laneSpacing = 5.0
    $objectWidthNorm = 0.02
    $objectHeightNorm = 0.08

    function New-Samples {
        param([int]$From, [int]$To, [double]$Lane, [int]$Width, [int]$Height)
        $samples = New-Object System.Collections.Generic.List[object]
        $boxWidth = [int][Math]::Round($objectWidthNorm * $Width)
        $boxHeight = [int][Math]::Round($objectHeightNorm * $Height)
        for ($t = $From; $t -le $To; $t += $StepMs) {
            $east = $SpeedMps * ($t / 1000.0)
            $north = $laneSpacing * $Lane
            $u = $east / $worldExtent
            $v = 1.0 - $north / $worldExtent
            $x = [int][Math]::Round($u * $Width - $boxWidth / 2.0)
            $y = [int][Math]::Round($v * $Height - $boxHeight)
            $samples.Add([pscustomobject]@{
                    t  = $t
                    bx = $x
                    by = $y
                    bw = $boxWidth
                    bh = $boxHeight
                })
        }
        return $samples
    }

    function Add-Tracklet {
        param($Builder, [string]$Camera, [int]$LocalId, $Samples, $Embedding)
        $observations = foreach ($sample in $Samples) {
            '{"timestamp_ms":"' + $sample.t + '","bbox":{"x":' + $sample.bx + ',"y":' + $sample.by +
            ',"w":' + $sample.bw + ',"h":' + $sample.bh + ',"score":0.9},' +
            '"world":{"x":0,"y":0,"valid":false}}'
        }
        $line = '{"schema_version":1,"session_id":"smoke-bev","camera_id":"' + $Camera +
        '","local_track_id":"' + $LocalId + '","sequence":"0","begin_ms":"' + $Samples[0].t +
        '","end_ms":"' + $Samples[$Samples.Count - 1].t + '","observations":[' +
        ($observations -join ',') + '],"embedding":{"encoding":"ENCODING_FLOAT32","dim":' +
        $Objects + ',"model_id":1,"values":[' + ($Embedding -join ',') +
        ']},"type":"OBJECT_TYPE_CAR","quality":0.9,"kind":"MESSAGE_KIND_TRACKLET"}'
        [void]$Builder.AppendLine($line)
    }

    function New-Dataset {
        param([int]$Width, [int]$Height, [string]$Path)
        $builder = New-Object System.Text.StringBuilder
        for ($k = 0; $k -lt $Objects; $k++) {
            $embedding = for ($i = 0; $i -lt $Objects; $i++) { if ($i -eq $k) { 1 } else { 0 } }
            Add-Tracklet $builder "cam-01" $k (New-Samples -From 0 -To $HandoffMs -Lane $k -Width $Width -Height $Height) $embedding
            Add-Tracklet $builder "cam-02" $k (New-Samples -From ($HandoffMs + $GapMs) -To $DurationMs -Lane $k -Width $Width -Height $Height) $embedding
        }
        & $writeText $Path $builder.ToString()
    }

    function New-Calibration {
        param([int]$Width, [int]$Height, [string]$Path)
        $text = @"
calibrations:
  - camera_id: cam-01
    homography: [100, 0, 0, 0, -100, 100, 0, 0, 1]
    image_width: $Width
    image_height: $Height
    homography_normalized: true
  - camera_id: cam-02
    homography: [100, 0, 0, 0, -100, 100, 0, 0, 1]
    image_width: $Width
    image_height: $Height
    homography_normalized: true
"@
        & $writeText $Path $text
    }

    function New-Config {
        param([string]$Path, [string]$DataPath, [string]$CalibrationPath, [string]$ResultsPath,
            [string]$WorldPath, [string]$WorldFormat)
        $data = & $slash $DataPath
        $calibration = & $slash $CalibrationPath
        $results = & $slash $ResultsPath
        $world = & $slash $WorldPath
        $text = @"
master:
  mode: offline
  window_ms: 30000
  window_overlap_ms: 10000
  commit_lag_ms: 15000
  warmup_ms: 0
  retention_ms: 120000
ingest:
  source: file
  max_lateness_ms: 5000
  strict: false
  file:
    path: $data
    speed: 1.0
    format: jsonl
reid:
  embedding_dim: $Objects
  index: brute
  top_k: 20
  min_similarity: 0.5
  exclude_same_camera: true
st:
  camera_links: ""
  min_transition_ms: 0
  max_transition_ms: 120000
  gate:
    mode: bev
    max_speed_mps: 30.0
    max_distance_m: 200.0
flow:
  solver: ssp
  link_reward: 1.0
  max_candidates_per_node: 15
  max_component_nodes: 20000
output:
  sinks: [jsonl]
  jsonl:
    path: $results
calibration:
  file: $calibration
  reload_interval_ms: 0
  overwrite_world: false
bev:
  enabled: true
  duplicate_distance_m: $DuplicateDistanceM
  duplicate_window_ms: $DuplicateWindowMs
  speed_window_ms: 1000
  smooth_window: 0
  fallback_to_zone: true
  world_output:
    format: $WorldFormat
    path: $world
log:
  level: warn
  console: true
"@
        & $writeText $Path $text
    }

    function Invoke-Master {
        param([string]$Config, [string]$Results, [string]$World, [string]$Stats, [string]$WorldFormat)
        $arguments = @("--config", $Config, "--out", $Results, "--world-out", $World,
            "--stats-out", $Stats)
        if ($WorldFormat) {
            $arguments += @("--world-format", $WorldFormat)
        }
        $output = & $MasterExe @arguments 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "master failed ($LASTEXITCODE): $($output -join "`n")"
        }
        return Get-Content -Raw $Stats | ConvertFrom-Json
    }

    function Read-GeoJson {
        param([string]$Path)
        $records = New-Object System.Collections.Generic.List[object]
        foreach ($line in (Get-Content $Path)) {
            if ([string]::IsNullOrWhiteSpace($line)) {
                continue
            }
            $records.Add(($line | ConvertFrom-Json))
        }
        return $records
    }

    function Get-Partition {
        param([string]$Path)
        $groups = [ordered]@{}
        foreach ($line in (Get-Content $Path)) {
            if ([string]::IsNullOrWhiteSpace($line)) {
                continue
            }
            $update = $line | ConvertFrom-Json
            $keys = foreach ($segment in $update.segments) {
                "$($segment.camera_id)/$($segment.local_track_id)/$($segment.sequence)"
            }
            $groups[[string]$update.global_id] = (($keys | Sort-Object) -join ',')
        }
        return (($groups.Values | Sort-Object) -join '|')
    }

    function Measure-Projection {
        param($Records)
        $maxError = 0.0
        foreach ($record in $Records) {
            $east = [double]$record.properties.east
            $north = [double]$record.properties.north
            $time = [double]$record.properties.ts_ms
            $expectedEast = $SpeedMps * $time / 1000.0
            $best = [double]::MaxValue
            for ($lane = 0; $lane -lt $Objects; $lane++) {
                $expectedNorth = $laneSpacing * $lane
                $distance = [Math]::Sqrt(
                    [Math]::Pow($east - $expectedEast, 2) + [Math]::Pow($north - $expectedNorth, 2))
                if ($distance -lt $best) {
                    $best = $distance
                }
            }
            if ($best -gt $maxError) {
                $maxError = $best
            }
        }
        return $maxError
    }

    $resolutions = @(
        [pscustomobject]@{ Name = "1920x1080"; Width = 1920; Height = 1080 },
        [pscustomobject]@{ Name = "1280x720"; Width = 1280; Height = 720 }
    )

    $runs = @{}
    foreach ($resolution in $resolutions) {
        $tag = $resolution.Name
        $dataPath = Join-Path $OutDir "data-$tag.jsonl"
        $calibrationPath = Join-Path $OutDir "calibration-$tag.yaml"
        $configPath = Join-Path $OutDir "master-$tag.yaml"
        $resultsPath = Join-Path $OutDir "results-$tag.jsonl"
        $worldPath = Join-Path $OutDir "world-$tag.geojsonl"
        $statsPath = Join-Path $OutDir "stats-$tag.json"

        Write-Host ("[projection] {0}: generating synthetic stream" -f $tag)
        New-Dataset -Width $resolution.Width -Height $resolution.Height -Path $dataPath
        New-Calibration -Width $resolution.Width -Height $resolution.Height -Path $calibrationPath
        New-Config -Path $configPath -DataPath $dataPath -CalibrationPath $calibrationPath `
            -ResultsPath $resultsPath -WorldPath $worldPath -WorldFormat "geojson"

        Write-Host ("[projection] {0}: running master" -f $tag)
        $stats = Invoke-Master -Config $configPath -Results $resultsPath -World $worldPath `
            -Stats $statsPath -WorldFormat "geojson"
        $records = Read-GeoJson $worldPath
        $runs[$tag] = [pscustomobject]@{
            Data       = $dataPath
            Config     = $configPath
            Results    = $resultsPath
            World      = $worldPath
            Stats      = $stats
            Records    = $records
            Partition  = Get-Partition $resultsPath
            MaxError   = Measure-Projection $records
        }
    }

    Write-Host "[output] CSV world export"
    $csvPath = Join-Path $OutDir "world-csv.csv"
    $csvStatsPath = Join-Path $OutDir "stats-csv.json"
    $csvResultsPath = Join-Path $OutDir "results-csv.jsonl"
    $csvConfigPath = Join-Path $OutDir "master-csv.yaml"
    $base = $runs[$resolutions[0].Name]
    New-Config -Path $csvConfigPath -DataPath $base.Data -CalibrationPath (Join-Path $OutDir "calibration-$($resolutions[0].Name).yaml") `
        -ResultsPath $csvResultsPath -WorldPath $csvPath -WorldFormat "csv"
    $csvStats = Invoke-Master -Config $csvConfigPath -Results $csvResultsPath -World $csvPath `
        -Stats $csvStatsPath -WorldFormat "csv"

    $failures = @()

    $expectedDuplicatesPerObject = [int][Math]::Floor(($DuplicateWindowMs - $GapMs) / $StepMs) + 1
    $expectedDuplicatesPerObject = [Math]::Max(0, $expectedDuplicatesPerObject)

    foreach ($resolution in $resolutions) {
        $tag = $resolution.Name
        $run = $runs[$tag]
        $stats = $run.Stats

        if ($null -eq $stats.world) {
            $failures += ("{0}: stats have no world section" -f $tag)
            continue
        }
        if ([int]$stats.world.projected -le 0) {
            $failures += ("{0}: no observations were projected" -f $tag)
        }
        if ([int]$stats.world.projection_failures -ne 0) {
            $failures += ("{0}: {1} projection failures" -f $tag, $stats.world.projection_failures)
        }
        if ([int]$stats.world.missing_world -ne 0) {
            $failures += ("{0}: {1} observations left without world coordinates" -f $tag, $stats.world.missing_world)
        }
        if ($null -eq $stats.world_output) {
            $failures += ("{0}: stats have no world_output section" -f $tag)
        }
        elseif ([int]$stats.world_output.samples_in -le 0) {
            $failures += ("{0}: world sink received no samples" -f $tag)
        }

        if ($run.Records.Count -eq 0) {
            $failures += ("{0}: world output is empty" -f $tag)
            continue
        }

        if ($run.MaxError -gt $MaxProjectionErrorM) {
            $failures += ("{0}: projection error {1:F3} m > {2:F3} m" -f $tag, $run.MaxError, $MaxProjectionErrorM)
        }

        $groups = $run.Records | Group-Object { $_.properties.global_id }
        if ($groups.Count -ne $Objects) {
            $failures += ("{0}: {1} global trajectories, expected {2}" -f $tag, $groups.Count, $Objects)
        }
        foreach ($group in $groups) {
            $sample = $group.Group[0]
            $speed = [double]$sample.properties.speed_mps
            $relative = if ($SpeedMps -gt 0) { [Math]::Abs($speed - $SpeedMps) / $SpeedMps } else { 0.0 }
            if ($relative -gt $SpeedTolerance + 1e-9) {
                $failures += ("{0}: global_id={1} speed {2:F3} m/s deviates {3:P1} from {4:F3} m/s" -f `
                        $tag, $group.Name, $speed, $relative, $SpeedMps)
            }
            $duplicates = [int]$sample.properties.duplicates_suppressed
            if ($expectedDuplicatesPerObject -gt 0) {
                $ratio = $duplicates / [double]$expectedDuplicatesPerObject
                if ($ratio -lt $MinDedupRatio - 1e-9) {
                    $failures += ("{0}: global_id={1} suppressed {2}/{3} duplicates" -f `
                            $tag, $group.Name, $duplicates, $expectedDuplicatesPerObject)
                }
            }
            $cameras = @($group.Group | ForEach-Object { $_.properties.camera } | Sort-Object -Unique)
            if ($cameras.Count -lt 2) {
                $failures += ("{0}: global_id={1} was not linked across cameras" -f $tag, $group.Name)
            }
        }
    }

    $reference = $runs[$resolutions[0].Name]
    $scaled = $runs[$resolutions[1].Name]
    if ($reference.Partition -ne $scaled.Partition) {
        $failures += "camera resolution changed the st-ReID partition"
    }

    $positions = @{}
    foreach ($record in $reference.Records) {
        $positions["$($record.properties.tracklet_id)|$($record.properties.ts_ms)"] = $record.properties
    }
    $maxDelta = 0.0
    $common = 0
    foreach ($record in $scaled.Records) {
        $key = "$($record.properties.tracklet_id)|$($record.properties.ts_ms)"
        if (-not $positions.ContainsKey($key)) {
            continue
        }
        $common++
        $delta = [Math]::Sqrt(
            [Math]::Pow([double]$record.properties.east - [double]$positions[$key].east, 2) +
            [Math]::Pow([double]$record.properties.north - [double]$positions[$key].north, 2))
        if ($delta -gt $maxDelta) {
            $maxDelta = $delta
        }
    }
    if ($common -eq 0) {
        $failures += "no common world samples between resolutions"
    }
    elseif ($maxDelta -gt $MaxProjectionErrorM) {
        $failures += ("world positions differ between resolutions by {0:F3} m > {1:F3} m" -f `
                $maxDelta, $MaxProjectionErrorM)
    }

    if (-not (Test-Path $csvPath)) {
        $failures += "CSV world output was not created"
    }
    else {
        $csvLines = @(Get-Content $csvPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
        $header = "global_id,ts_ms,tracklet_id,camera,type,east,north,latitude,longitude,speed_mps,course_deg,is_final,trajectory_begin_ms,trajectory_end_ms,duplicates_suppressed"
        if ($csvLines.Count -eq 0) {
            $failures += "CSV world output is empty"
        }
        elseif ($csvLines[0] -ne $header) {
            $failures += "CSV world output has an unexpected header"
        }
        if ($csvLines.Count -le 1) {
            $failures += "CSV world output has no data rows"
        }
        if ($null -eq $csvStats.world_output -or [int]$csvStats.world_output.samples_in -le 0) {
            $failures += "CSV run recorded no world sink samples"
        }
    }

    Write-Host ""
    Write-Host "Stage 8 DoD summary"
    Write-Host ("  objects / cameras     : {0} / 2" -f $Objects)
    Write-Host ("  expected speed        : {0:F3} m/s" -f $SpeedMps)
    foreach ($resolution in $resolutions) {
        $run = $runs[$resolution.Name]
        $group = $run.Records | Group-Object { $_.properties.global_id } | Select-Object -First 1
        $speed = if ($group) { [double]$group.Group[0].properties.speed_mps } else { 0.0 }
        Write-Host ("  {0,-9} error={1:F3} m speed={2:F3} m/s samples={3} suppressed={4}" -f `
                $resolution.Name, $run.MaxError, $speed, $run.Records.Count,
            $(if ($run.Stats.world_output) { $run.Stats.world_output.duplicates_suppressed } else { 0 }))
    }
    Write-Host ("  expected duplicates   : {0} per object (window {1} ms, gap {2} ms)" -f `
            $expectedDuplicatesPerObject, $DuplicateWindowMs, $GapMs)
    Write-Host ("  resolution delta      : {0:F3} m (common samples {1})" -f $maxDelta, $common)
    Write-Host ("  partition equal       : {0}" -f $(if ($reference.Partition -eq $scaled.Partition) { "yes" } else { "no" }))
    Write-Host ("  CSV output            : {0}" -f $(if ((Test-Path $csvPath) -and $csvLines.Count -gt 1) { "yes" } else { "no" }))

    if ($failures.Count -gt 0) {
        Write-Host ""
        Write-Host "FAILED:"
        foreach ($failure in $failures) {
            Write-Host ("  - {0}" -f $failure)
        }
        exit 1
    }

    Write-Host ""
    Write-Host "Stage 8 DoD: PASSED"
    exit 0
}
finally {
    Pop-Location
}
