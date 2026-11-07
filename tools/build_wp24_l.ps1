# WP24-L retry exhaustion dedicated firmware build/flash.
# Run from project root in ESP-IDF v6.1 PowerShell.
$ErrorActionPreference = "Stop"
if (-not $env:IDF_PATH) { throw "IDF_PATH is not set. Activate ESP-IDF v6.1 first." }
$config = Join-Path (Get-Location) "sdkconfig.wp24_l"
if (-not (Test-Path $config)) { throw "Missing $config" }
$configText = Get-Content $config -Raw
foreach ($required in @(
    "CONFIG_MYEMS_MODBUS_SELF_TEST=y",
    "CONFIG_MYEMS_WP24_FAULT_INJECTION=y",
    "CONFIG_MYEMS_WP24_EXTENDED_TEST=7"
)) {
    if ($configText -notmatch [regex]::Escape($required)) { throw "Missing setting: $required" }
}
Write-Host "Reconfiguring WP24-L..." -ForegroundColor Cyan
idf.py -B build_wp24_l -D "SDKCONFIG=sdkconfig.wp24_l" reconfigure
if ($LASTEXITCODE -ne 0) { throw "Reconfigure failed ($LASTEXITCODE)" }
idf.py -B build_wp24_l -D "SDKCONFIG=sdkconfig.wp24_l" build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
$header = Get-Content "build_wp24_l\config\sdkconfig.h" -Raw
if ($header -notmatch "#define CONFIG_MYEMS_WP24_EXTENDED_TEST 7") {
    throw "Generated config does not select WP24-L"
}
Write-Host "WP24-L build complete. Flashing COM11..." -ForegroundColor Cyan
idf.py -B build_wp24_l -D "SDKCONFIG=sdkconfig.wp24_l" -p COM11 flash
if ($LASTEXITCODE -ne 0) { throw "Flash failed ($LASTEXITCODE)" }
Write-Host "WP24-L firmware flashed." -ForegroundColor Green
