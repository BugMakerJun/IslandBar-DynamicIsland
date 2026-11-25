# =====================================================================
#  tray-probe-matrix.ps1
#  Runs the independent tray probe outside the project tree and saves raw output.
#  ASCII only: Windows PowerShell 5.1 loads this file without UTF-8 assumptions.
# =====================================================================

param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",
    [ValidateRange(1, 1000)]
    [int]$Samples = 10,
    [ValidateRange(0, 600000)]
    [int]$IntervalMs = 1000,
    [switch]$NoInstall,
    [switch]$NoBroadcast
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$buildRoot = Join-Path $root ("IslandBar\build\{0}\tools\tray_probe\{0}" -f $Config)
$probeSource = Join-Path $buildRoot "tray_probe.exe"
$hookSource = Join-Path $buildRoot "island_tray_probe_hook.dll"

if (-not (Test-Path -LiteralPath $probeSource)) {
    throw "tray_probe.exe not found: $probeSource"
}
if (-not (Test-Path -LiteralPath $hookSource)) {
    throw "island_tray_probe_hook.dll not found: $hookSource"
}

$runDir = Join-Path $env:USERPROFILE "IslandBar\TrayProbe\Matrix"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$probe = Join-Path $runDir "tray_probe.exe"
$hook = Join-Path $runDir "island_tray_probe_hook.dll"
Copy-Item -LiteralPath $probeSource -Destination $probe -Force
Copy-Item -LiteralPath $hookSource -Destination $hook -Force

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outDir = Join-Path $root "docs\review"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$outFile = Join-Path $outDir ("tray-probe-{0}-{1}.log" -f $Config.ToLowerInvariant(), $stamp)

$arguments = @(
    "--details",
    "--samples", $Samples.ToString(),
    "--interval-ms", $IntervalMs.ToString()
)
if ($NoInstall) {
    $arguments += "--no-install"
}
if ($NoBroadcast) {
    $arguments += "--no-broadcast"
}

Write-Host ("tray-probe matrix: config={0} samples={1} interval_ms={2}" -f $Config, $Samples, $IntervalMs)
Write-Host ("raw log: {0}" -f $outFile)

& $probe @arguments 2>&1 | Tee-Object -FilePath $outFile
$probeExit = $LASTEXITCODE

$snapshotLines = @(Get-Content -LiteralPath $outFile |
    Where-Object { $_ -match "^tray-probe: registry=(\d+) live=(\d+) visible=(\d+) duplicates=(\d+) stale=(\d+)" })

$maxDuplicates = 0
$maxStale = 0
foreach ($line in $snapshotLines) {
    $match = [regex]::Match($line, "^tray-probe: registry=(\d+) live=(\d+) visible=(\d+) duplicates=(\d+) stale=(\d+)")
    if (-not $match.Success) {
        continue
    }
    $duplicates = [int]$match.Groups[4].Value
    $stale = [int]$match.Groups[5].Value
    if ($duplicates -gt $maxDuplicates) {
        $maxDuplicates = $duplicates
    }
    if ($stale -gt $maxStale) {
        $maxStale = $stale
    }
}

$latencyText = "N/A"
$latencySamples = 0
$p95 = -1
$latencyLines = @(Get-Content -LiteralPath $outFile |
    Where-Object { $_ -match "^tray-probe: modify-to-paint p50=(N/A|\d+ms) p95=(N/A|\d+)ms samples=(\d+)" })
if ($latencyLines.Count -gt 0) {
    $latencyMatch = [regex]::Match($latencyLines[$latencyLines.Count - 1],
        "^tray-probe: modify-to-paint p50=(N/A|\d+ms) p95=(N/A|\d+)ms samples=(\d+)")
    if ($latencyMatch.Success) {
        $latencyText = $latencyMatch.Groups[2].Value
        $latencySamples = [int]$latencyMatch.Groups[3].Value
        if ($latencyText -ne "N/A") {
            $p95 = [int]$latencyText.Replace("ms", "")
        }
    }
}

$result = "PASS"
$reason = "duplicates=0 stale=0"
if ($probeExit -ne 0) {
    $result = "FAIL"
    $reason = "probe_exit=$probeExit"
} elseif ($snapshotLines.Count -lt $Samples) {
    $result = "FAIL"
    $reason = "snapshots=$($snapshotLines.Count)/$Samples"
} elseif ($maxDuplicates -ne 0) {
    $result = "FAIL"
    $reason = "max_duplicates=$maxDuplicates"
} elseif ($maxStale -ne 0) {
    $result = "FAIL"
    $reason = "max_stale=$maxStale"
} elseif ($p95 -gt 1500) {
    $result = "FAIL"
    $reason = "modify_to_paint_p95=$p95"
}

$summary = "tray-probe matrix: result={0} snapshots={1}/{2} max_duplicates={3} max_stale={4} " -f `
    $result, $snapshotLines.Count, $Samples, $maxDuplicates, $maxStale
$summary += "modify_to_paint_p95={0} latency_samples={1} reason={2}" -f `
    $latencyText, $latencySamples, $reason
Write-Host $summary
Add-Content -LiteralPath $outFile -Value $summary

if ($result -ne "PASS") {
    exit 1
}
exit 0
