# WP24-J retry exhaustion dedicated firmware build/flash.
# Run from project root in ESP-IDF v6.1 PowerShell.
$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { throw "IDF_PATH is not set. Activate ESP-IDF v6.1 first." }
$config = Join-Path (Get-Location) "sdkconfig.wp24_j"
if (-not (Test-Path $config)) { throw "Missing $config" }
$configText = Get-Content $config -Raw
foreach ($required in @(
    "CONFIG_MYEMS_MODBUS_SELF_TEST=y",
    "CONFIG_MYEMS_WP24_FAULT_INJECTION=y",
    "CONFIG_MYEMS_WP24_EXTENDED_TEST=5"
)) {
    if ($configText -notmatch [regex]::Escape($required)) { throw "Missing setting: $required" }
}
Write-Host "Reconfiguring WP24-J..." -ForegroundColor Cyan
idf.py -B build_wp24_j -D "SDKCONFIG=sdkconfig.wp24_j" reconfigure
if ($LASTEXITCODE -ne 0) { throw "Reconfigure failed ($LASTEXITCODE)" }
idf.py -B build_wp24_j -D "SDKCONFIG=sdkconfig.wp24_j" build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
$header = Get-Content "build_wp24_j\config\sdkconfig.h" -Raw
if ($header -notmatch "#define CONFIG_MYEMS_WP24_EXTENDED_TEST 5") {
    throw "Generated config does not select WP24-J"
}
Write-Host "WP24-J build complete. Flashing COM11..." -ForegroundColor Cyan
idf.py -B build_wp24_j -D "SDKCONFIG=sdkconfig.wp24_j" -p COM11 flash
if ($LASTEXITCODE -ne 0) { throw "Flash failed ($LASTEXITCODE)" }
Write-Host "WP24-J firmware flashed." -ForegroundColor Green
