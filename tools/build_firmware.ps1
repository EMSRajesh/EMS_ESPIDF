[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('SelfTest', 'Production', 'Wp4Sim', 'Wp4Sim2Dev', 'Wp4Ratio', 'Wp4Esc')]
    [string]$Mode,

    [string]$Port = 'COM11',
    [switch]$Flash,
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $ProjectRoot

# Wp4Sim = production firmware + CONFIG_MYEMS_CONFIG_TARGET_SIM (DP-WP4 simulation configuration)
# Wp4Esc = Wp4Sim + CONFIG_MYEMS_WP4_ESC_TEST (D8: RS-485 recovery forced to fail; HIL suite esc only)
# Wp4Sim2Dev / Wp4Ratio = Wp4Sim with site lab_bench_2dev / lab_ratio_ct200 (D9)
$configFile = switch ($Mode) { 'SelfTest' { 'sdkconfig.selftest' } 'Wp4Sim' { 'sdkconfig.wp4_sim' } 'Wp4Sim2Dev' { 'sdkconfig.wp4_sim_2dev' } 'Wp4Ratio' { 'sdkconfig.wp4_ratio' } 'Wp4Esc' { 'sdkconfig.wp4_esc' } default { 'sdkconfig.production' } }
$buildDir = switch ($Mode) { 'SelfTest' { 'build_selftest' } 'Wp4Sim' { 'build_wp4_sim' } 'Wp4Sim2Dev' { 'build_wp4_sim_2dev' } 'Wp4Ratio' { 'build_wp4_ratio' } 'Wp4Esc' { 'build_wp4_esc' } default { 'build_production' } }
$configPath = Join-Path $ProjectRoot $configFile

if (-not (Test-Path $configPath)) {
    throw "Configuration file not found: $configPath"
}

$idf = Get-Command idf.py -ErrorAction SilentlyContinue
if (-not $idf) {
    throw 'idf.py was not found. Open the ESP-IDF v6.1 PowerShell environment first.'
}

Write-Host "Building MyEMS mode: $Mode" -ForegroundColor Cyan
Write-Host "Configuration: $configFile"
Write-Host "Build directory: $buildDir"

# Each mode has its own CMake build tree and its own sdkconfig input.
& idf.py -B $buildDir -D "SDKCONFIG=$configPath" build
if ($LASTEXITCODE -ne 0) {
    throw "ESP-IDF build failed for mode $Mode (exit code $LASTEXITCODE)."
}

if ($Flash -or $Monitor) {
    & idf.py -B $buildDir -p $Port flash
    if ($LASTEXITCODE -ne 0) {
        throw "Flashing failed for mode $Mode (exit code $LASTEXITCODE)."
    }
}

if ($Monitor) {
    & idf.py -B $buildDir -p $Port monitor
    if ($LASTEXITCODE -ne 0) {
        throw "Monitor exited with code $LASTEXITCODE."
    }
}

Write-Host "Completed: $Mode" -ForegroundColor Green
