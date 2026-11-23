# Runtime environment for locally built TrafficMCMT binaries and tests.
#
# The local Release build links a CUDA-enabled OpenCV, protobuf from vcpkg and
# the Multitarget-tracker static libraries. Their DLLs live outside the default
# DLL search path, so running the tests without preparation fails with
# 0xc0000135 (DLL not found).
#
# The script reads the CMake cache of the build tree, derives the directories
# that hold the runtime dependencies (OpenCV, CUDA, cuDNN, OpenVINO/oneTBB,
# vcpkg packages, Multitarget-tracker) and prepends the existing ones to PATH
# for the current process.
#
# Example:
#   .\scripts\dev-env.ps1
#   ctest --test-dir build/local-release -C Release --output-on-failure
#
#   .\scripts\dev-env.ps1 -Show          # print the directories, change nothing
#
# From cmd.exe:
#   powershell -NoProfile -ExecutionPolicy Bypass -Command ^
#     "& .\scripts\dev-env.ps1; ctest --test-dir build/local-release -C Release"

[CmdletBinding()]
param(
    [string]$BuildDir = "build/local-release",
    [string]$Configuration = "Release",
    [string[]]$ExtraPath = @(),
    [switch]$Show
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else {
    Join-Path $RepoRoot $BuildDir
}
$CacheFile = Join-Path $BuildPath "CMakeCache.txt"

if (-not (Test-Path $CacheFile)) {
    throw "CMake cache not found: $CacheFile. Configure the build first, " +
          "e.g. cmake --preset local-release"
}

# CMakeCache.txt holds "NAME:TYPE=VALUE" lines; the last one wins.
function Get-CacheValue {
    param([string]$Cache, [string]$Name)

    if (-not (Test-Path $Cache)) {
        return $null
    }
    $entry = Select-String -Path $Cache -Pattern "^$([regex]::Escape($Name)):[^=]*=" |
        Select-Object -Last 1
    if (-not $entry) {
        return $null
    }
    return ($entry.Line -replace '^[^=]*=', '').Trim()
}

# Package directories come as <prefix>/lib/cmake/<pkg>, <prefix>/share/<pkg>
# (vcpkg) or <prefix>/lib/<pkg>/cmake. Walk up to the install root.
function Get-PackagePrefix {
    param([string]$PackageDir)

    $dir = $PackageDir
    while ($dir) {
        $leaf = Split-Path $dir -Leaf
        if ($leaf -eq 'lib' -or $leaf -eq 'share' -or $leaf -eq 'include') {
            return (Split-Path $dir -Parent)
        }
        $parent = Split-Path $dir -Parent
        if (-not $parent -or $parent -eq $dir) {
            break
        }
        $dir = $parent
    }
    return $null
}

$RuntimeDirs = New-Object System.Collections.Generic.List[string]
$RuntimeSet = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

function Add-RuntimeDir {
    param([string]$Path, [string]$Source)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }
    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        Write-Verbose "not found ($Source): $full"
        return
    }
    if ($RuntimeSet.Add($full)) {
        $RuntimeDirs.Add($full) | Out-Null
    }
}

# --- OpenCV runtime and the CUDA/cuDNN/OpenVINO it was built against --------
$OpenCvDir = Get-CacheValue $CacheFile 'OpenCV_DIR'
$CudnnLib = $null

if ($OpenCvDir) {
    Add-RuntimeDir (Join-Path $OpenCvDir "bin/$Configuration") 'OpenCV build tree'
    Add-RuntimeDir (Join-Path $OpenCvDir "x64/vc17/bin") 'OpenCV install tree'

    $OpenCvCache = Join-Path $OpenCvDir 'CMakeCache.txt'
    $CudnnLib = Get-CacheValue $OpenCvCache 'CUDNN_LIBRARY'
    if ($CudnnLib) {
        # <cudnn>/lib/<cuda-version>/x64/cudnn.lib -> <cudnn>/bin/<cuda-version>/x64
        $CudnnBin = Split-Path $CudnnLib -Parent
        Add-RuntimeDir ($CudnnBin -replace '([\\/])lib([\\/])', '$1bin$2') 'cuDNN'
    }

    $OpenVinoDir = Get-CacheValue $OpenCvCache 'OpenVINO_DIR'
    if ($OpenVinoDir) {
        # <openvino>/runtime/cmake -> <openvino>/runtime
        $OpenVinoRuntime = Split-Path $OpenVinoDir -Parent
        Add-RuntimeDir (Join-Path $OpenVinoRuntime "bin/intel64/$Configuration") 'OpenVINO'
        Add-RuntimeDir (Join-Path $OpenVinoRuntime '3rdparty/tbb/bin') 'oneTBB (OpenVINO)'
    }
} else {
    Write-Warning "OpenCV_DIR is not set in $CacheFile; OpenCV runtime is not added"
}

# --- CUDA toolkit (cublas and friends needed by opencv_dnn) ----------------
$CudaVersion = $null
if ($CudnnLib -and $CudnnLib -match '[\\/]lib[\\/]([0-9]+\.[0-9]+)[\\/]') {
    $CudaVersion = $Matches[1]
}

$CudaRoot = Join-Path ${env:ProgramFiles} 'NVIDIA GPU Computing Toolkit/CUDA'
if (Test-Path $CudaRoot) {
    $CudaCandidates = @()
    if ($CudaVersion) {
        $CudaCandidates += Join-Path $CudaRoot "v$CudaVersion"
    }
    $CudaCandidates += Get-ChildItem $CudaRoot -Directory |
        Where-Object { $_.Name -match '^v[0-9]+(\.[0-9]+)*$' } |
        Sort-Object { [version]($_.Name -replace '^v', '') } -Descending |
        ForEach-Object { $_.FullName }

    foreach ($candidate in $CudaCandidates) {
        $binDir = Join-Path $candidate "bin/x64"
        if (-not (Test-Path $binDir)) {
            $binDir = Join-Path $candidate "bin"
        }
        if (Test-Path (Join-Path $binDir 'cublas64_*.dll')) {
            Add-RuntimeDir $binDir 'CUDA toolkit'
            break
        }
    }
}

# --- Package runtimes: spdlog, yaml-cpp, protobuf, gtest, ... --------------
$PrefixPaths = Get-CacheValue $CacheFile 'CMAKE_PREFIX_PATH'
if ($PrefixPaths) {
    foreach ($prefix in ($PrefixPaths -split ';')) {
        if ($prefix) {
            Add-RuntimeDir (Join-Path $prefix 'bin') "CMAKE_PREFIX_PATH ($prefix)"
        }
    }
}

foreach ($package in 'protobuf_DIR', 'spdlog_DIR', 'yaml-cpp_DIR') {
    $packageDir = Get-CacheValue $CacheFile $package
    if ($packageDir) {
        $prefix = Get-PackagePrefix $packageDir
        if ($prefix) {
            Add-RuntimeDir (Join-Path $prefix 'bin') $package
        }
    }
}

# --- Multitarget-tracker ---------------------------------------------------
$MttDir = Get-CacheValue $CacheFile 'MTTracking_DIR'
if ($MttDir) {
    $prefix = Get-PackagePrefix $MttDir
    if ($prefix) {
        Add-RuntimeDir (Join-Path $prefix 'bin') 'Multitarget-tracker'
    }
}

# --- Extra directories from the caller -------------------------------------
foreach ($path in $ExtraPath) {
    $full = if ([System.IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $RepoRoot $path }
    Add-RuntimeDir $full 'ExtraPath'
}

if ($Show) {
    Write-Host "Runtime directories for $BuildPath ($Configuration):"
    if ($RuntimeDirs.Count -eq 0) {
        Write-Host "  (none found)"
    }
    foreach ($dir in $RuntimeDirs) {
        Write-Host "  $dir"
    }
    return
}

if ($RuntimeDirs.Count -eq 0) {
    Write-Warning "No runtime directories found for $BuildPath ($Configuration)"
    return
}

$CurrentPath = $env:PATH
$Keep = foreach ($path in ($CurrentPath -split ';')) {
    if ($path -and -not $RuntimeSet.Contains($path.TrimEnd('\', '/'))) {
        $path
    }
}
$env:PATH = (@($RuntimeDirs) + @($Keep)) -join ';'

foreach ($dir in $RuntimeDirs) {
    Write-Verbose "added to PATH: $dir"
}

Write-Host "PATH updated with $($RuntimeDirs.Count) runtime directories."
