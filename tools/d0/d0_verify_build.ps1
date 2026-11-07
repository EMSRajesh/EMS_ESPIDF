<#
.SYNOPSIS
    DP-WP4 milestone D0, test REG-01: prove that the committed sources build
    in Production and SelfTest mode from a fresh clone.

.DESCRIPTION
    Run from an ESP-IDF v6.1 PowerShell (IDF_PATH set, idf.py on PATH).

    1. Clones the repository (committed HEAD only) to a sibling folder of the
       project, so uncommitted local files cannot hide a missing file.
    2. Runs tools\build_firmware.ps1 -Mode Production and -Mode SelfTest in the
       clone.
    3. Records idf.py size for both images and whether the build rewrote any
       tracked sdkconfig file.
    4. Writes a report to tools\test_results\d0_reg01_<timestamp>\ in the
       project (ignored by Git) and deletes the clone unless -KeepClone.

    No flashing. No serial ports are used.

.PARAMETER ClonePath
    Where to clone. Default: <parent of project>\_d0_verify_clone

.PARAMETER KeepClone
    Keep the clone after the run (about 0.5 GB with both build trees).

.EXAMPLE
    .\tools\d0\d0_verify_build.ps1
#>
[CmdletBinding()]
param(
    [string]$ClonePath,
    [switch]$KeepClone
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $ClonePath) {
    $ClonePath = Join-Path (Split-Path $ProjectRoot -Parent) '_d0_verify_clone'
}
$stamp   = Get-Date -Format 'yyyyMMdd_HHmmss'
$outDir  = Join-Path $ProjectRoot "tools\test_results\d0_reg01_$stamp"
$report  = Join-Path $outDir 'report.txt'
$results = [ordered]@{}

function Write-Report([string]$Line) {
    Write-Host $Line
    Add-Content -LiteralPath $report -Value $Line -Encoding UTF8
}

# Git arguments are passed as one explicit array (see d0_housekeeping.ps1).
function Get-GitOutput([string[]]$GitArgs) {
    $out = & git @GitArgs
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($out -join "`n").Trim()
}

# ------------------------------------------------------------ checks ---
if (-not $env:IDF_PATH) { throw 'IDF_PATH is not set. Open the ESP-IDF v6.1 PowerShell first.' }
$IdfPy = Join-Path $env:IDF_PATH 'tools\idf.py'
if (-not (Test-Path -LiteralPath $IdfPy)) { throw "idf.py not found at $IdfPy" }
# ESP-IDF's export script puts its Python virtual environment first; prefer it explicitly.
$PyExe = 'python'
if ($env:IDF_PYTHON_ENV_PATH) {
    $candidate = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
    if (Test-Path -LiteralPath $candidate) { $PyExe = $candidate }
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'git not found on PATH.' }
if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot '.git'))) {
    throw 'No Git repository yet. Run tools\d0\d0_housekeeping.ps1 -Apply first.'
}

New-Item -ItemType Directory -Path $outDir -Force | Out-Null

Push-Location -LiteralPath $ProjectRoot
try {
    $head   = Get-GitOutput @('rev-parse', '--short', 'HEAD')
    $branch = Get-GitOutput @('rev-parse', '--abbrev-ref', 'HEAD')
    $dirty  = Get-GitOutput @('status', '--porcelain')
    $idfVer = (& cmd.exe /c "`"$PyExe`" `"$IdfPy`" --version 2>&1") -join ' '
} finally {
    Pop-Location
}

Write-Report 'MyEMS D0 / REG-01 fresh-clone build verification'
Write-Report "Date:          $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
Write-Report "Project:       $ProjectRoot"
Write-Report "Commit:        $head ($branch)"
Write-Report "ESP-IDF:       $idfVer"
Write-Report "Clone path:    $ClonePath"
if ($dirty) {
    Write-Report 'NOTE: working tree has uncommitted changes; only committed files are verified:'
    Write-Report $dirty
}

# ------------------------------------------------------------- clone ---
if (Test-Path -LiteralPath $ClonePath) {
    Write-Host "Removing previous clone $ClonePath"
    & cmd.exe /c "rd /s /q `"\\?\$ClonePath`""
}
& git -c core.longpaths=true clone -q --no-local -- "$ProjectRoot" "$ClonePath"
if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }

# ------------------------------------------------------------ builds ---
foreach ($needed in @('sdkconfig.production', 'sdkconfig.selftest', 'partitions_myems_esp32s3_16mb.csv')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ClonePath $needed))) {
        throw "Missing $needed in the clone (is it committed?)"
    }
}

foreach ($mode in @('Production', 'SelfTest')) {
    $log = Join-Path $outDir ("build_{0}.log" -f $mode.ToLower())
    $buildDir = if ($mode -eq 'SelfTest') { 'build_selftest' } else { 'build_production' }
    Write-Host ''
    Write-Host "== Building $mode (log: $log)" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    # Run the build in a child PowerShell and let cmd.exe do the redirection:
    # in Windows PowerShell 5.1, redirected native stderr (cmake/ninja warnings)
    # would otherwise become terminating errors under ErrorActionPreference=Stop.
    # Same arguments as tools\build_firmware.ps1, but invoking idf.py through
    # ESP-IDF's Python so it works whether idf.py is an .exe or a PS function.
    $configFile = if ($mode -eq 'SelfTest') { 'sdkconfig.selftest' } else { 'sdkconfig.production' }
    $configPath = Join-Path $ClonePath $configFile
    Push-Location -LiteralPath $ClonePath
    try {
        & cmd.exe /c "`"$PyExe`" `"$IdfPy`" -B $buildDir -D `"SDKCONFIG=$configPath`" build > `"$log`" 2>&1"
        $exitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    $sw.Stop()
    Get-Content -LiteralPath $log -Tail 15 | Out-Host

    $bin = Join-Path $ClonePath "$buildDir\myems_rs485_modbus_rtu.bin"
    $ok = ($exitCode -eq 0) -and (Test-Path -LiteralPath $bin)

    if ($ok) {
        $sizeFile = Join-Path $outDir ("size_{0}.txt" -f $mode.ToLower())
        Push-Location -LiteralPath $ClonePath
        try {
            & cmd.exe /c "`"$PyExe`" `"$IdfPy`" -B $buildDir size > `"$sizeFile`" 2>&1"
        } finally {
            Pop-Location
        }
    }

    $results[$mode] = $ok
    $binKB = if (Test-Path -LiteralPath $bin) { '{0:N0} KB' -f ((Get-Item -LiteralPath $bin).Length / 1KB) } else { 'n/a' }
    Write-Report ("{0,-11} {1,-4}  time {2,5:N0} s  image {3}" -f $mode, ($(if ($ok) { 'PASS' } else { 'FAIL' })), $sw.Elapsed.TotalSeconds, $binKB)
}

# ESP-IDF writes defaults back into the SDKCONFIG file it was given.
$drift = Get-GitOutput @('-C', $ClonePath, 'status', '--porcelain')
Write-Report ''
if ($drift) {
    Write-Report 'Tracked files changed by the builds (ESP-IDF rewrites the sdkconfig it is given):'
    Write-Report $drift
    Write-Report 'Action: build once in the project, review the sdkconfig diff and commit it, so later builds start clean.'
} else {
    Write-Report 'Tracked files unchanged by the builds.'
}

# ----------------------------------------------------------- cleanup ---
if (-not $KeepClone) {
    & cmd.exe /c "rd /s /q `"\\?\$ClonePath`""
    Write-Host "Clone removed: $ClonePath"
}

$allPass = (@($results.Values | Where-Object { -not $_ }).Count -eq 0) -and ($results.Count -eq 2)
Write-Report ''
Write-Report ("REG-01 RESULT: {0}" -f $(if ($allPass) { 'PASS' } else { 'FAIL' }))
Write-Report "Evidence: $outDir"
if (-not $allPass) { exit 1 }
