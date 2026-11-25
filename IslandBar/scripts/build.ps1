# =====================================================================
#  build.ps1 -- configure, build, and optionally run the smoke test
#
#      powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build.ps1
#      ... -File .\scripts\build.ps1 -Config Release -Clean
#
#  ASCII-only on purpose. Do not add non-ASCII text to this file.
# =====================================================================

[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')] [string]$Config = 'Debug',
    [switch]$Clean,
    [switch]$RunSmoke,
    [switch]$NoSmoke
)

$ErrorActionPreference = 'Stop'

# --- locate workspace ------------------------------------------------
#   Derived from this script's own location, never hardcoded: PowerShell 5.1
#   reads .ps1 with the ANSI codepage, so a non-ASCII path literal would be
#   mangled on the way in.   <root>\scripts\build.ps1  ->  <root>
$Root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
Write-Host "[build] root: $Root"

# Verify the root really holds the project, so cmake never sees a bad path
if (-not (Test-Path (Join-Path $Root 'CMakeLists.txt'))) {
    throw "CMakeLists.txt not found under $Root"
}

# --- locate tools ----------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source

# Prefer the CMake that ships INSIDE the Visual Studio instance.
#
# A separately installed CMake (winget, chocolatey, a manual unzip) ends up earlier on
# PATH and then wins, and because build.ps1 does not initialise the MSVC environment it
# picks the Ninja generator and fails with "No CMAKE_CXX_COMPILER could be found" -- the
# toolchain is only discoverable through the Visual Studio generator. The bundled CMake is
# also the one this project's generator and /MP settings were verified against.
if ($vsPath) {
    $bundled = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $bundled) {
        if ($cmake -and ($cmake -ne $bundled)) {
            Write-Host "[build] ignoring PATH cmake ($cmake) in favour of the VS-bundled one"
        }
        $cmake = $bundled
    }
}
if (-not $cmake) { throw 'cmake not found (neither inside the VS instance nor on PATH)' }
Write-Host "[build] cmake: $cmake"

# The Visual Studio generator knows where the SDK lives, but cmake/WinRt.cmake
# reads WindowsSdkDir from the environment to add the C++/WinRT projection
# include path. A bare PowerShell does not set that variable, which produced
# six configure warnings even though all targets still built through their ABI
# fallbacks. Fill in the installed SDK here so configure is genuinely clean.
if (-not $env:WindowsSdkDir) {
    $sdkInclude = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
    if (Test-Path $sdkInclude) {
        $sdkVersion = Get-ChildItem -LiteralPath $sdkInclude -Directory |
            Sort-Object Name -Descending | Select-Object -First 1
        if ($sdkVersion) {
            $env:WindowsSdkDir = (Split-Path -Parent $sdkInclude) + '\'
            Write-Host "[build] Windows SDK: $($sdkVersion.Name)"
        }
    }
}

# Ninja is only usable when the MSVC environment is already set up, because Ninja does not
# discover the toolchain the way the Visual Studio generator does. Detected rather than
# assumed: without cl.exe on PATH, Ninja configure always fails.
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if ($ninja) {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
        Write-Host '[build] cl.exe is on PATH, so Ninja would work'
    } else {
        Write-Host '[build] ignoring Ninja: cl.exe is not on PATH (run env.ps1 first to use it)'
        $ninja = $null
    }
}

# --- configure -------------------------------------------------------
$buildDir = Join-Path $Root "build\$Config"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "[build] removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

$genArgs = @()
if ($ninja) {
    $genArgs = @('-G', 'Ninja', "-DCMAKE_BUILD_TYPE=$Config")
    Write-Host '[build] generator: Ninja'
} else {
    $genArgs = @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    Write-Host '[build] generator: Visual Studio 17 2022 (MSBuild)'
}

if ($NoSmoke) { $genArgs += '-DISLANDBAR_BUILD_SMOKE=OFF' } else { $genArgs += '-DISLANDBAR_BUILD_SMOKE=ON' }

Write-Host "[build] configuring ..."
& $cmake -S $Root -B $buildDir @genArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with $LASTEXITCODE" }

# --- build -----------------------------------------------------------
Write-Host "[build] compiling ..."

# Do NOT pass --parallel to the MSBuild generator on this machine.
#
# CMake turns --parallel into MSBuild's /m, and /m with more than one node fails
# instantly here: MSBuild prints nothing but its version banner, reports
# "0 warnings, 0 errors", and exits 1. Verified directly, outside this script:
#     MSBuild.exe IslandBar.sln /t:Build /p:Configuration=Debug /m:2  -> exit 1
#     MSBuild.exe IslandBar.sln /t:Build /p:Configuration=Debug /m:1  -> exit 0
# so the multi-node path itself is broken in this environment, not the project.
# Sources still compile in parallel: the top-level CMakeLists.txt adds /MP, which
# parallelises compilation inside a single project.
$parallelJobs = 1

# Move a locked executable aside before linking.
#
# The linker intermittently fails with
#     LINK : fatal error LNK1104: cannot open file '...\island_bar.exe'
# even with no island_bar process running: the handle outlives the process by several seconds
# (an antivirus scan, or the previous instance still tearing down). Retrying does not help --
# the lock lasts far longer than any sensible retry window -- but RENAMING the file does, because
# the lock permits a rename and the linker then creates a brand new file. Without this the build
# fails at random, and a build that fails at random is worse than a slow one.
$exePath = Join-Path $buildDir "src\$Config\island_bar.exe"
if (Test-Path $exePath) {
    $aside = "$exePath.stale"
    Remove-Item $aside -Force -ErrorAction SilentlyContinue
    try {
        Rename-Item $exePath $aside -ErrorAction Stop
        Write-Host '[build] moved the previous executable aside (the file was locked)'
    } catch {
        # Not locked, or the rename failed for another reason: let the linker deal with it.
    }
}

& $cmake --build $buildDir --config $Config --parallel $parallelJobs
if ($LASTEXITCODE -ne 0) {
    # A failed link leaves the previous binary as .stale; put it back so a transient failure
    # does not also destroy the last working build.
    if ((Test-Path "$exePath.stale") -and -not (Test-Path $exePath)) {
        Move-Item "$exePath.stale" $exePath -Force -ErrorAction SilentlyContinue
        Write-Host '[build] link failed; restored the previous executable' -ForegroundColor Yellow
    }
    throw "build failed with $LASTEXITCODE"
}

Remove-Item "$exePath.stale" -Force -ErrorAction SilentlyContinue

Write-Host "[build] OK" -ForegroundColor Green

# --- run smoke -------------------------------------------------------
$smokeExe = Get-ChildItem $buildDir -Recurse -Filter 'island_smoke.exe' -ErrorAction SilentlyContinue |
            Select-Object -First 1

if ($smokeExe) {
    Write-Host "[build] smoke binary: $($smokeExe.FullName)"
    if ($RunSmoke) {
        Write-Host ''
        Write-Host '=== running smoke test ===' -ForegroundColor Cyan
        # The test prints non-ASCII, so force UTF-8 console output
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        & $smokeExe.FullName
        $code = $LASTEXITCODE
        Write-Host ''
        if ($code -eq 0) { Write-Host '=== SMOKE PASS ===' -ForegroundColor Green }
        else             { Write-Host "=== SMOKE FAIL (exit $code) ===" -ForegroundColor Red }
        exit $code
    } else {
        Write-Host "[build] run it with: & '$($smokeExe.FullName)'"
    }
} else {
    Write-Host '[build] no smoke binary produced' -ForegroundColor Yellow
}
