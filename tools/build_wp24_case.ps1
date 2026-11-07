param(
    [ValidateSet("A", "B", "C", "D", "E", "F")]
    [string]$Case = "A"
)
$ErrorActionPreference = "Stop"

if (-not $env:IDF_PATH) { throw "IDF_PATH is not set. Activate ESP-IDF v6.1 first." }
$idfPy = Join-Path $env:IDF_PATH "tools\idf.py"
if (-not (Test-Path $idfPy)) { throw "idf.py not found at $idfPy" }
$config = Join-Path (Get-Location) "sdkconfig.wp24_case"
if (-not (Test-Path $config)) { throw "Isolated WP24 config not found: $config" }

$caseValue = [int][char]$Case - [int][char]'A' + 1
$configText = Get-Content $config -Raw
if ($configText -notmatch "CONFIG_MYEMS_WP24_CASE_SELECTION=") {
    throw "sdkconfig.wp24_case is missing CONFIG_MYEMS_WP24_CASE_SELECTION"
}
$configText = [regex]::Replace($configText, "(?m)^CONFIG_MYEMS_WP24_CASE_SELECTION=\d+\s*$", "CONFIG_MYEMS_WP24_CASE_SELECTION=$caseValue")
Set-Content -Path $config -Value $configText -NoNewline -Encoding ASCII

foreach ($required in @(
    "CONFIG_MYEMS_MODBUS_SELF_TEST=y",
    "CONFIG_MYEMS_WP24_FAULT_INJECTION=y",
    "CONFIG_MYEMS_WP24_EXTENDED_TEST=4",
    "CONFIG_MYEMS_WP24_CASE_SELECTION=$caseValue"
)) {
    if ((Get-Content $config -Raw) -notmatch [regex]::Escape($required)) {
        throw "WP24 isolated config missing required setting: $required"
    }
}

$buildDir = "build_wp24_case"
Write-Host "Configuring isolated WP24-$Case firmware..." -ForegroundColor Cyan
idf.py -B $buildDir -D "SDKCONFIG=sdkconfig.wp24_case" reconfigure
if ($LASTEXITCODE -ne 0) { throw "WP24 reconfigure failed ($LASTEXITCODE)" }

Write-Host "Building isolated WP24-$Case firmware..." -ForegroundColor Cyan
idf.py -B $buildDir -D "SDKCONFIG=sdkconfig.wp24_case" build
if ($LASTEXITCODE -ne 0) { throw "WP24 build failed ($LASTEXITCODE)" }

$header = Join-Path (Get-Location) "$buildDir\config\sdkconfig.h"
if (-not (Test-Path $header)) { throw "Generated sdkconfig.h missing: $header" }
$headerText = Get-Content $header -Raw
if ($headerText -notmatch "#define CONFIG_MYEMS_WP24_CASE_SELECTION $caseValue") {
    throw "Generated config does not select WP24-$Case. Refusing to flash."
}

Write-Host "Flashing isolated WP24-$Case firmware to COM11..." -ForegroundColor Cyan
idf.py -B $buildDir -D "SDKCONFIG=sdkconfig.wp24_case" -p COM11 flash
if ($LASTEXITCODE -ne 0) { throw "WP24 flash failed ($LASTEXITCODE)" }

Write-Host "WP24-$Case firmware built and flashed." -ForegroundColor Green
$caseSuffix = switch ($Case) {
    A { "drop_timeout" }
    B { "late_response_quarantine" }
    C { "bad_crc_recovery" }
    D { "short_frame_recovery" }
    E { "wrong_slave_recovery" }
    F { "valid_exception_no_recovery" }
}
$runnerCase = "WP24_${Case}_${caseSuffix}"
Write-Host "Run only this case with:"
Write-Host "python .\tools\myems_wp24_recovery_test.py --only $runnerCase --console-port COM11 --rs485-port COM12 --baud 9600 --build-dir build_wp24_case"
