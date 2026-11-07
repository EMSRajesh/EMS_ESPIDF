# WP24-K retry exhaustion dedicated firmware build/flash.
# Run from project root in ESP-IDF v6.1 PowerShell.
$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { throw "IDF_PATH is not set. Activate ESP-IDF v6.1 first." }
$config = Join-Path (Get-Location) "sdkconfig.wp24_k"
if (-not (Test-Path $config)) { throw "Missing $config" }
$configText = Get-Content $config -Raw
foreach ($required in @(
    "CONFIG_MYEMS_MODBUS_SELF_TEST=y",
    "CONFIG_MYEMS_WP24_FAULT_INJECTION=y",
    "CONFIG_MYEMS_WP24_EXTENDED_TEST=6"
)) {
    if ($configText -notmatch [regex]::Escape($required)) { throw "Missing setting: $required" }
}
Write-Host "Reconfiguring WP24-K..." -ForegroundColor Cyan
idf.py -B build_wp24_k -D "SDKCONFIG=sdkconfig.wp24_k" reconfigure
if ($LASTEXITCODE -ne 0) { throw "Reconfigure failed ($LASTEXITCODE)" }
idf.py -B build_wp24_k -D "SDKCONFIG=sdkconfig.wp24_k" build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
$header = Get-Content "build_wp24_k\config\sdkconfig.h" -Raw
if ($header -notmatch "#define CONFIG_MYEMS_WP24_EXTENDED_TEST 6") {
    throw "Generated config does not select WP24-K"
}
Write-Host "WP24-K build complete. Flashing COM11..." -ForegroundColor Cyan
idf.py -B build_wp24_k -D "SDKCONFIG=sdkconfig.wp24_k" -p COM11 flash
if ($LASTEXITCODE -ne 0) { throw "Flash failed ($LASTEXITCODE)" }
Write-Host "WP24-K firmware flashed." -ForegroundColor Green
