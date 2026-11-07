# WP24 dedicated test firmware: one build and one flash.
# Run from an ESP-IDF v6.1 PowerShell environment in the project root.
$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) {
    throw "IDF_PATH is not set. Activate ESP-IDF v6.1 first."
}

$idfPy = Join-Path $env:IDF_PATH "tools\idf.py"
if (-not (Test-Path $idfPy)) {
    throw "idf.py not found at $idfPy"
}

$config = Join-Path (Get-Location) "sdkconfig.wp24_all"
if (-not (Test-Path $config)) {
    throw "Dedicated WP24 config not found: $config"
}

$configText = Get-Content $config -Raw
foreach ($required in @(
    "CONFIG_MYEMS_MODBUS_SELF_TEST=y",
    "CONFIG_MYEMS_WP24_FAULT_INJECTION=y",
    "CONFIG_MYEMS_WP24_EXTENDED_TEST=4"
)) {
    if ($configText -notmatch [regex]::Escape($required)) {
        throw "WP24 config missing required setting: $required"
    }
}

Write-Host "Reconfiguring dedicated WP24 A-I build..." -ForegroundColor Cyan
idf.py -B build_wp24_all -D "SDKCONFIG=sdkconfig.wp24_all" reconfigure
if ($LASTEXITCODE -ne 0) { throw "WP24 reconfigure failed ($LASTEXITCODE)" }

Write-Host "Building WP24 A-I firmware..." -ForegroundColor Cyan
idf.py -B build_wp24_all -D "SDKCONFIG=sdkconfig.wp24_all" build
if ($LASTEXITCODE -ne 0) { throw "WP24 build failed ($LASTEXITCODE)" }

$cache = Join-Path (Get-Location) "build_wp24_all\CMakeCache.txt"
if (-not (Test-Path $cache)) { throw "CMakeCache missing after build" }
$sdkconfigLine = Select-String -Path $cache -Pattern '^SDKCONFIG:[^=]*=(.*)$' | Select-Object -First 1
if (-not $sdkconfigLine) {
    throw "SDKCONFIG entry not found in CMakeCache. Refusing to flash."
}
$selectedSdkconfig = $sdkconfigLine.Matches[0].Groups[1].Value.Trim().Replace('/', '\\')
$selectedSdkconfigName = Split-Path $selectedSdkconfig -Leaf
if ($selectedSdkconfigName -ne "sdkconfig.wp24_all") {
    throw "Build cache selects '$selectedSdkconfig', not sdkconfig.wp24_all. Refusing to flash."
}

Write-Host "Flashing dedicated WP24 A-I firmware to COM11..." -ForegroundColor Cyan
idf.py -B build_wp24_all -D "SDKCONFIG=sdkconfig.wp24_all" -p COM11 flash
if ($LASTEXITCODE -ne 0) { throw "WP24 flash failed ($LASTEXITCODE)" }

Write-Host "WP24 A-I firmware build and flash completed." -ForegroundColor Green
Write-Host "Next run the campaign:"
Write-Host "python .\tools\myems_wp24_all_tests.py --esp-port COM11 --rs485-port COM12 --baud 9600 --parity E --stopbits 1"
