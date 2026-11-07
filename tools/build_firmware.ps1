[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('SelfTest', 'Production')]
    [string]$Mode,

    [string]$Port = 'COM11',
    [switch]$Flash,
    [switch]$Monitor
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $ProjectRoot

$configFile = if ($Mode -eq 'SelfTest') { 'sdkconfig.selftest' } else { 'sdkconfig.production' }
$buildDir = if ($Mode -eq 'SelfTest') { 'build_selftest' } else { 'build_production' }
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
