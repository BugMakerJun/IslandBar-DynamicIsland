# =====================================================================
#  taskbar-recovery-test.ps1 -- prove a hard-killed bar restores Explorer
#
#      powershell -NoProfile -ExecutionPolicy Bypass `
#          -File .\scripts\taskbar-recovery-test.ps1 -Config Debug -Iterations 20
#
#  The script refuses to start while another island_bar is running. It starts
#  the deployed copy from %USERPROFILE%\IslandBar, waits until the guardian
#  has acknowledged and the taskbar is hidden, kills ONLY that process, then
#  requires Shell_TrayWnd to become visible within MaxRecoveryMs.
#
#  ASCII-only on purpose. Do not add non-ASCII text to this script.
# =====================================================================

[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')] [string]$Config = 'Debug',
    [ValidateRange(1,100)] [int]$Iterations = 20,
    [ValidateRange(250,10000)] [int]$MaxRecoveryMs = 2000,
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$SourceExe = Join-Path $Root "build\$Config\src\$Config\island_bar.exe"
$SourceHook = Join-Path $Root "build\$Config\src\$Config\island_tray_hook.dll"
$InstallDir = Join-Path $env:USERPROFILE 'IslandBar'
$Exe = Join-Path $InstallDir 'island_bar.exe'
$Hook = Join-Path $InstallDir 'island_tray_hook.dll'
$InstallLog = Join-Path $InstallDir 'island_bar.log'
$RecoveryFile = Join-Path $InstallDir 'taskbar_recovery.dat'

if (-not (Test-Path $SourceExe)) { throw "no executable at $SourceExe" }
if (-not (Test-Path $SourceHook)) { throw "no hook module at $SourceHook" }
if (Get-Process island_bar -ErrorAction SilentlyContinue) {
    throw 'island_bar is already running; close it normally before this test'
}

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
if ($Deploy) {
    Copy-Item -LiteralPath $SourceExe -Destination $Exe -Force
    Copy-Item -LiteralPath $SourceHook -Destination $Hook -Force
}
if (-not (Test-Path $Exe)) { throw "no deployed executable at $Exe" }

if (-not ('IslandBarRecoveryProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class IslandBarRecoveryProbe {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
}
'@
}

function Test-TaskbarVisible {
    $window = [IslandBarRecoveryProbe]::FindWindow('Shell_TrayWnd', $null)
    return $window -ne [IntPtr]::Zero -and [IslandBarRecoveryProbe]::IsWindowVisible($window)
}

function Test-TaskbarHidden {
    $window = [IslandBarRecoveryProbe]::FindWindow('Shell_TrayWnd', $null)
    return $window -ne [IntPtr]::Zero -and -not [IslandBarRecoveryProbe]::IsWindowVisible($window)
}

function Find-RunLog([int]$ProcessId) {
    $candidates = @(
        $InstallLog,
        (Join-Path $env:TEMP 'island_bar.log'),
        (Join-Path $env:TEMP ("island_bar-{0}.log" -f $ProcessId))
    )
    foreach ($candidate in $candidates) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        try {
            $found = Get-Content -LiteralPath $candidate -ErrorAction Stop |
                Where-Object { $_ -match "--- island_bar starting \(pid $ProcessId\) ---" } |
                Select-Object -First 1
            if ($found) { return $candidate }
        } catch {
            # A log being replaced can be briefly unavailable; the next poll retries.
        }
    }
    return $null
}

function Wait-TaskbarVisible([int]$Milliseconds) {
    $deadline = (Get-Date).AddMilliseconds($Milliseconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-TaskbarVisible) { return $true }
        Start-Sleep -Milliseconds 50
    }
    return (Test-TaskbarVisible)
}

function Restore-DesktopState {
    & (Join-Path $Root 'scripts\restore-desktop.ps1')
}

if (-not (Test-TaskbarVisible)) {
    Write-Host '[recovery] taskbar starts hidden; restoring a clean baseline'
    Restore-DesktopState
}

$maxObservedMs = 0.0
try {
    for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $Exe
        $startInfo.WorkingDirectory = $InstallDir
        $startInfo.UseShellExecute = $true
        $process = [System.Diagnostics.Process]::Start($startInfo)
        if (-not $process) { throw "iteration ${iteration}: process did not start" }

        $startDeadline = (Get-Date).AddSeconds(12)
        $armed = $false
        $runLog = $null
        while ((Get-Date) -lt $startDeadline) {
            if ($process.HasExited) {
                throw "iteration ${iteration}: process exited early code=$($process.ExitCode)"
            }
            if (-not $runLog) { $runLog = Find-RunLog $process.Id }
            if ($runLog) {
                $lines = @(Get-Content -LiteralPath $runLog -ErrorAction SilentlyContinue)
                $armed = ($lines | Where-Object {
                    $_ -match "taskbar guardian: armed owner pid $($process.Id) "
                } | Select-Object -First 1) -ne $null
            }
            # Hide() fails closed unless the guardian acknowledged. A hidden
            # taskbar with the live process is therefore valid proof even while
            # the log file itself is being rolled to its PID-qualified fallback.
            if (Test-TaskbarHidden) {
                $armed = $true
                break
            }
            Start-Sleep -Milliseconds 75
        }
        if (-not $armed) { throw "iteration ${iteration}: guardian never armed" }
        if (-not (Test-TaskbarHidden)) { throw "iteration ${iteration}: taskbar still visible after guardian armed" }

        $killedAt = Get-Date
        $process.Kill()
        if (-not $process.WaitForExit(5000)) {
            throw "iteration ${iteration}: killed process did not exit"
        }

        $visible = Wait-TaskbarVisible $MaxRecoveryMs
        $elapsedMs = ((Get-Date) - $killedAt).TotalMilliseconds
        $maxObservedMs = [Math]::Max($maxObservedMs, $elapsedMs)
        if (-not $visible) {
            throw "iteration ${iteration}: taskbar not restored within $MaxRecoveryMs ms"
        }

        $restoredLine = $null
        $restoreDeadline = (Get-Date).AddSeconds(2)
        while ((Get-Date) -lt $restoreDeadline -and -not $restoredLine) {
            if (-not $runLog) { $runLog = Find-RunLog $process.Id }
            if ($runLog) {
                $restoredLine = Get-Content -LiteralPath $runLog -ErrorAction SilentlyContinue |
                    Where-Object { $_ -match "taskbar guardian: restored reason=owner-terminated pid $($process.Id) " } |
                    Select-Object -First 1
            }
            if (-not $restoredLine) { Start-Sleep -Milliseconds 50 }
        }
        if (-not $restoredLine) {
            throw "iteration ${iteration}: no guardian restore log for pid $($process.Id)"
        }
        if (Test-Path $RecoveryFile) {
            throw "iteration ${iteration}: recovery snapshot was not cleared"
        }
        Write-Host ("[recovery] iteration {0}/{1}: pid {2}, restored in {3:N0} ms" -f `
                    $iteration, $Iterations, $process.Id, $elapsedMs)
    }
} catch {
    if (Get-Process island_bar -ErrorAction SilentlyContinue) {
        Get-Process island_bar | ForEach-Object { $_.Kill(); $_.WaitForExit(5000) | Out-Null }
    }
    Restore-DesktopState
    throw
}

Write-Host ("RESULT: PASS iterations={0} max_recovery_ms={1:N0}" -f $Iterations, $maxObservedMs)
