<#
.SYNOPSIS
    DP-WP4 milestone D9: run every D9 test one after the other, unattended
    (overnight), and write one summary.

.DESCRIPTION
    Run from the ESP-IDF v6.1 PowerShell in the project root, with the
    ESP32 console on COM11 and the USB-RS485 simulator adapter on COM12.
    NO LIVE METER on the RS-485 bus.

    Order (about 3.5 h, then the soak):
      1. PC tests: host ctest, profile-tool and wp4 Python tests
      2. Wp4Sim2Dev: suites two_dev, load (60 min)
      3. Wp4Ratio:   suite ratio
      4. Wp4Esc:     suite esc (D8 regression)
      5. Production: suite reject
      6. Wp4Sim:     suites d6, d7
      7. Regression: REG-01 (fresh clone of HEAD), REG-02 COM self-test,
                     REG-03 WP24 A-I, REG-05 partition/flash layout
      8. Wp4Sim:     soak dry run (60 min); if it passes, the full soak
                     (24 h by default)
    A failed step does not stop the run; steps that need a firmware which
    failed to build/flash are recorded as SKIPPED. The PC is kept awake while
    the script runs (it does not change any power setting permanently).

    Results: tools\test_results\d9_all_<timestamp>\
      summary.txt      one line per step (updated after every step)
      NN_<step>.log    full output of every step
    Each HIL suite also writes its usual evidence folder (named in summary.txt).

    REG-01 builds a fresh clone of the last COMMIT: commit the D9 files before
    the run so that every result belongs to one commit.

.PARAMETER SoakMinutes
    Length of the full soak. Default 1440 (24 h, Test Spec PERF-005).

.PARAMETER SkipSoak
    Stop after the regression (no dry run, no full soak).

.PARAMETER SkipRegression
    Skip REG-01..05.

.PARAMETER NoDryRun
    Go straight to the full soak without the 60 min dry run.

.EXAMPLE
    .\tools\d9\run_d9_all.ps1
.EXAMPLE
    .\tools\d9\run_d9_all.ps1 -SoakMinutes 480        # 8 h soak only
#>
[CmdletBinding()]
param(
    [string]$ConsolePort = 'COM11',
    [string]$Rs485Port = 'COM12',
    [int]$LoadMinutes = 60,
    [int]$DryRunMinutes = 60,
    [int]$SoakMinutes = 1440,
    [switch]$SkipSoak,
    [switch]$SkipRegression,
    [switch]$NoDryRun
)

$ErrorActionPreference = 'Continue'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $ProjectRoot

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$OutDir = Join-Path $ProjectRoot "tools\test_results\d9_all_$stamp"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Summary = Join-Path $OutDir 'summary.txt'
$script:Results = New-Object System.Collections.ArrayList
$script:StepNo = 0
$env:PYTHONUNBUFFERED = '1'      # live progress from the Python runners

# ------------------------------------------------------------------ helpers
function Write-Summary {
    $lines = @()
    $lines += "DP-WP4 D9 run $stamp"
    $lines += "Commit: $script:Commit   Ports: console $ConsolePort, RS-485 $Rs485Port"
    $lines += ""
    $lines += ('{0,-3} {1,-34} {2,-8} {3,8}  {4}' -f '#', 'Step', 'Result', 'Minutes', 'Details / evidence')
    foreach ($r in $script:Results) {
        $lines += ('{0,-3} {1,-34} {2,-8} {3,8}  {4}' -f $r.No, $r.Name, $r.Result, $r.Minutes, $r.Note)
    }
    $pass = @($script:Results | Where-Object { $_.Result -eq 'PASS' }).Count
    $fail = @($script:Results | Where-Object { $_.Result -eq 'FAIL' }).Count
    $skip = @($script:Results | Where-Object { $_.Result -eq 'SKIPPED' }).Count
    $lines += ""
    $lines += "PASS $pass   FAIL $fail   SKIPPED $skip"
    Set-Content -Path $Summary -Value $lines -Encoding ASCII
}

function Add-Result([string]$Name, [string]$Result, [double]$Minutes, [string]$Note) {
    $script:StepNo++
    [void]$script:Results.Add([pscustomobject]@{
        No = $script:StepNo; Name = $Name; Result = $Result
        Minutes = ('{0:N1}' -f $Minutes); Note = $Note
    })
    Write-Summary
    $color = switch ($Result) { 'PASS' { 'Green' } 'SKIPPED' { 'Yellow' } default { 'Red' } }
    Write-Host ("    -> {0}  {1}" -f $Result, $Note) -ForegroundColor $color
}

function Skip-Steps([string[]]$Names, [string]$Why) {
    foreach ($n in $Names) { Add-Result $n 'SKIPPED' 0 $Why }
}

# Kind 'exit'  : PASS when the last program exits with 0 and nothing threw.
# Kind 'runner': PASS when the output has 'RESULT: PASS' (BLOCKED rows allowed).
# Kind 'empty' : PASS when the step prints nothing (git diff checks).
# Output is shown live and copied to the step log. -MergeErr also copies the
# error stream (Python, git); it is NOT used for the .ps1 build scripts,
# because Windows PowerShell 5.1 turns redirected native stderr (CMake/idf.py
# warnings) into terminating errors in scripts that use ErrorAction Stop.
function Invoke-Step {
    param([string]$Name, [scriptblock]$Body, [string]$Kind = 'exit', [switch]$MergeErr)
    $n = $script:StepNo + 1
    $log = Join-Path $OutDir (('{0:D2}_{1}.log' -f $n, $Name) -replace '[^A-Za-z0-9_.-]', '_')
    Write-Host ""
    Write-Host ("=== [{0:D2}] {1}   started {2:HH:mm:ss}" -f $n, $Name, (Get-Date)) -ForegroundColor Cyan
    $t0 = Get-Date
    $result = 'FAIL'
    $note = ''
    try {
        $global:LASTEXITCODE = 0
        if ($MergeErr) {
            & $Body 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log | Out-Host
        } else {
            & $Body | Tee-Object -FilePath $log | Out-Host
        }
        $code = $global:LASTEXITCODE
        $text = if (Test-Path $log) { Get-Content -Path $log -Raw } else { '' }
        if ($null -eq $text) { $text = '' }
        switch ($Kind) {
            'runner' {
                $m = [regex]::Match($text, 'RESULT: ([^\r\n]+)')
                $e = [regex]::Match($text, 'Evidence: ([^\r\n]+)')
                $ev = if ($e.Success) { Split-Path $e.Groups[1].Value.Trim() -Leaf } else { '' }
                if ($m.Success) {
                    $note = ($m.Groups[1].Value.Trim() + '  ' + $ev).Trim()
                    if ($m.Groups[1].Value.Trim().StartsWith('PASS')) { $result = 'PASS' }
                } else {
                    $note = "no RESULT line (exit code $code)  $ev"
                }
            }
            'empty' {
                if ([string]::IsNullOrWhiteSpace($text)) { $result = 'PASS' } else { $note = 'differences found (see log)' }
            }
            default {
                if ($code -eq 0) { $result = 'PASS' } else { $note = "exit code $code" }
            }
        }
    } catch {
        $note = $_.Exception.Message
        Add-Content -Path $log -Value "EXCEPTION: $note"
    } finally {
        Set-Location $ProjectRoot      # a step script may have changed folder
    }
    $minutes = ((Get-Date) - $t0).TotalMinutes
    if ($note -eq '') { $note = Split-Path $log -Leaf }
    Add-Result $Name $result $minutes $note
    return ($result -eq 'PASS')
}

function Invoke-Flash([string]$Mode) {
    return (Invoke-Step "flash $Mode" { & .\tools\build_firmware.ps1 -Mode $Mode -Port $ConsolePort -Flash })
}

function Invoke-Suite([string]$Suite, [string[]]$Extra = @()) {
    # $Suite, $Extra, $ConsolePort, $Rs485Port are visible to the block when Invoke-Step runs it
    $sb = { & python tools/wp4/myems_wp4_runner.py --suite $Suite --console-port $ConsolePort --rs485-port $Rs485Port @Extra }
    return (Invoke-Step "suite $Suite" $sb 'runner' -MergeErr)
}

# Keep the PC awake while the script runs (no permanent power setting is changed).
try {
    Add-Type -Namespace MyEms -Name Power -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint SetThreadExecutionState(uint esFlags);
'@ -ErrorAction Stop
    [void][MyEms.Power]::SetThreadExecutionState([uint32]'0x80000001')   # ES_CONTINUOUS | ES_SYSTEM_REQUIRED
    $script:AwakeSet = $true
} catch {
    Write-Host "Could not prevent sleep automatically: set Windows to never sleep for this run." -ForegroundColor Yellow
    $script:AwakeSet = $false
}

Start-Transcript -Path (Join-Path $OutDir 'transcript.log') | Out-Null
try {
    # -------------------------------------------------------------- preflight
    $script:Commit = (& git rev-parse --short HEAD 2>$null)
    $dirty = (& git status --porcelain --untracked-files=no 2>$null)
    Write-Host "DP-WP4 D9 run $stamp, commit $script:Commit, results in $OutDir" -ForegroundColor Cyan
    if ($dirty) {
        Write-Host "WARNING: uncommitted changes. REG-01 builds the last commit ($script:Commit), not these changes." -ForegroundColor Yellow
    }
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        throw 'idf.py not found: open the ESP-IDF v6.1 PowerShell first.'
    }
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        throw 'python not found on PATH.'
    }
    $ports = $null
    try { $ports = [System.IO.Ports.SerialPort]::GetPortNames() } catch { $ports = $null }
    if ($ports) {
        foreach ($p in @($ConsolePort, $Rs485Port)) {
            if ($ports -notcontains $p) { throw "Serial port $p not found (present: $($ports -join ', ')). Check the cables." }
        }
    }
    if ($ConsolePort -ne 'COM11' -and -not $SkipRegression) {
        Write-Host "NOTE: tools\build_wp24_all.ps1 always flashes COM11 (REG-03)." -ForegroundColor Yellow
    }
    $soakText = if ($SkipSoak) { 'no soak' } elseif ($NoDryRun) { "$SoakMinutes min soak" } else { "$DryRunMinutes min dry run + $SoakMinutes min soak" }
    Write-Host "Plan: PC tests, two_dev, load ($LoadMinutes min), ratio, esc, reject, d6, d7, regression, $soakText" -ForegroundColor Cyan
    Write-Host "Make sure no serial monitor is open on $ConsolePort / $Rs485Port." -ForegroundColor Cyan

    # ------------------------------------------------------------- PC tests
    Invoke-Step 'host tests (ctest)' {
        & cmake -S test/host -B build_host -G Ninja -DCMAKE_C_COMPILER=gcc
        if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
        & cmake --build build_host
        if ($LASTEXITCODE -ne 0) { throw 'host build failed' }
        & ctest --test-dir build_host --output-on-failure
    } | Out-Null
    Invoke-Step 'profile tool tests' { & python -m unittest discover -s tools/profile/tests } -MergeErr | Out-Null
    Invoke-Step 'wp4 runner/simulator tests' { & python -m unittest discover -s tools/wp4/tests } -MergeErr | Out-Null

    # ------------------------------------------------------ two meters, load
    if (Invoke-Flash 'Wp4Sim2Dev') {
        Invoke-Suite 'two_dev' | Out-Null
        Invoke-Suite 'load' @('--duration-min', "$LoadMinutes") | Out-Null
    } else { Skip-Steps @('suite two_dev', 'suite load') 'flash Wp4Sim2Dev failed' }

    # ---------------------------------------------------------------- ratio
    if (Invoke-Flash 'Wp4Ratio') { Invoke-Suite 'ratio' | Out-Null }
    else { Skip-Steps @('suite ratio') 'flash Wp4Ratio failed' }

    # ------------------------------------------------------- esc (D8), reject
    if (Invoke-Flash 'Wp4Esc') { Invoke-Suite 'esc' | Out-Null }
    else { Skip-Steps @('suite esc') 'flash Wp4Esc failed' }
    if (Invoke-Flash 'Production') { Invoke-Suite 'reject' | Out-Null }
    else { Skip-Steps @('suite reject') 'flash Production failed' }

    # ---------------------------------------------------------------- d6, d7
    if (Invoke-Flash 'Wp4Sim') {
        Invoke-Suite 'd6' | Out-Null
        Invoke-Suite 'd7' | Out-Null
    } else { Skip-Steps @('suite d6', 'suite d7') 'flash Wp4Sim failed' }

    # ------------------------------------------------------------ regression
    if (-not $SkipRegression) {
        Invoke-Step 'REG-01 fresh-clone build' { & .\tools\d0\d0_verify_build.ps1 } | Out-Null
        Invoke-Step 'REG-02 COM self-test' {
            & python .\tools\myems_run_full_rtu_test_v5_1.py --console-port $ConsolePort --rs485-port $Rs485Port
        } -MergeErr | Out-Null
        if (Invoke-Step 'REG-03 build+flash WP24 A-I' { & .\tools\build_wp24_all.ps1 }) {
            Invoke-Step 'REG-03 WP24 A-I campaign' {
                & python .\tools\myems_wp24_all_tests.py --esp-port $ConsolePort --rs485-port $Rs485Port
            } -MergeErr | Out-Null
        } else { Skip-Steps @('REG-03 WP24 A-I campaign') 'WP24 build/flash failed' }
        Invoke-Step 'REG-05 partition table unchanged' {
            & git diff --stat com-09_057 HEAD -- partitions_myems_esp32s3_16mb.csv
        } 'empty' -MergeErr | Out-Null
        Invoke-Step 'REG-05 flash settings unchanged' {
            & git diff com-09_057 HEAD -- sdkconfig.production 2>&1 | Select-String -Pattern 'PARTITION|FLASHSIZE|FLASHMODE|FLASHFREQ|fatal|error'
        } 'empty' | Out-Null
    }

    # ------------------------------------------------------------------ soak
    if (-not $SkipSoak) {
        if (Invoke-Flash 'Wp4Sim') {
            $go = $true
            if (-not $NoDryRun) {
                $go = Invoke-Suite 'soak' @('--duration-min', "$DryRunMinutes")
                if (-not $go) { Skip-Steps @('suite soak (full)') 'dry run did not pass: fix it before 24 h' }
            }
            if ($go) {
                Write-Host ("Full soak: {0} min, ends about {1:ddd HH:mm}" -f $SoakMinutes, (Get-Date).AddMinutes($SoakMinutes + 2)) -ForegroundColor Cyan
                Invoke-Step 'suite soak (full)' {
                    & python tools/wp4/myems_wp4_runner.py --suite soak --duration-min $SoakMinutes --console-port $ConsolePort --rs485-port $Rs485Port
                } 'runner' -MergeErr | Out-Null
            }
        } else { Skip-Steps @('suite soak') 'flash Wp4Sim failed' }
    } else {
        # leave the board with the normal simulation firmware
        Invoke-Flash 'Wp4Sim' | Out-Null
    }
} catch {
    Write-Host "STOPPED: $($_.Exception.Message)" -ForegroundColor Red
    Add-Result 'preflight / script' 'FAIL' 0 $_.Exception.Message
} finally {
    Write-Summary
    if ($script:AwakeSet) { [void][MyEms.Power]::SetThreadExecutionState([uint32]'0x80000000') }
    Stop-Transcript | Out-Null
}

Write-Host ""
Get-Content $Summary | ForEach-Object { Write-Host $_ }
Write-Host ""
Write-Host "Summary: $Summary"
$failed = @($script:Results | Where-Object { $_.Result -eq 'FAIL' }).Count
if ($failed -gt 0) { exit 1 } else { exit 0 }
