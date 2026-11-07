<#
.SYNOPSIS
    DP-WP4 milestone D0: put the 09_057 baseline under Git, tidy the project
    root, and delete regenerable ESP-IDF build folders.

.DESCRIPTION
    Run from any directory; the script locates the project root from its own
    path (<root>\tools\d0\d0_housekeeping.ps1).

    Without -Apply the script only prints what it would do (dry run).
    With    -Apply it performs, in order:
      1. git init, .gitignore check, baseline commit, tag  com-09_057
      2. git mv of documents, patches and snapshots into docs\ and archive\
      3. concurrency_contract.md de-duplication
      4. .vscode\settings.json clangd path fix
      5. commit  "D0: housekeeping"
      6. deletion of build_wp24_* / build_wp25_* folders (not tracked by Git)

    sdkconfig.* files stay in the project root on purpose: the COM-track build
    scripts (tools\build_wp24_*.ps1) and runners load them by name from there.

    Nothing outside the project root is touched. Steps 1-5 are reversible with
    Git (the baseline is tagged com-09_057). Step 6 deletes build output only;
    every runner rebuilds it.

.PARAMETER Apply
    Perform the changes. Without it, dry run.

.PARAMETER SkipBuildDirDeletion
    Do steps 1-5 only.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\tools\d0\d0_housekeeping.ps1
    powershell -ExecutionPolicy Bypass -File .\tools\d0\d0_housekeeping.ps1 -Apply
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$SkipBuildDirDeletion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

# ---------------------------------------------------------------- helpers ---
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:Planned = 0
$script:Done    = 0
$script:Skipped = 0
$script:D0CommitExists = $false

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "== $Text" -ForegroundColor Cyan
}

# Git arguments are always passed as one explicit array, e.g.
#   Invoke-Git @('commit', '-q', '-m', 'text')
# so that PowerShell never interprets -q, -m or -- as its own parameters.
function Invoke-Git([string[]]$GitArgs) {
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) {
        throw "git $($GitArgs -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Get-GitOutput([string[]]$GitArgs) {
    $out = & git @GitArgs
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($out -join "`n").Trim()
}

function Move-Tracked([string]$Source, [string]$Destination) {
    $src = Join-Path $ProjectRoot $Source
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Host "  skip (not found): $Source" -ForegroundColor DarkGray
        $script:Skipped++
        return
    }
    $dst = Join-Path $ProjectRoot $Destination
    $script:Planned++
    if (-not $Apply) {
        Write-Host "  [dry-run] git mv `"$Source`" `"$Destination`""
        return
    }
    $parent = Split-Path $dst -Parent
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Invoke-Git @('mv', '--', $Source, $Destination)
    Write-Host "  moved: $Source -> $Destination"
    $script:Done++
}

function Get-FolderBytes([string]$Path) {
    $sum = (Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
    if ($null -eq $sum) { return 0 }
    return [int64]$sum
}

# --------------------------------------------------------- preconditions ---
Write-Step "Project root: $ProjectRoot"
if ($Apply) {
    Write-Host 'Mode: APPLY (changes will be made)' -ForegroundColor Yellow
} else {
    Write-Host 'Mode: DRY RUN (no changes). Re-run with -Apply to execute.' -ForegroundColor Green
}

foreach ($required in @('CMakeLists.txt', 'main\app_main.c',
                        'components\modbus_rtu_master\src\modbus_rtu_master.c',
                        '.gitignore')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ProjectRoot $required))) {
        throw "Not the expected project root: missing $required"
    }
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'git was not found in PATH. Install Git for Windows (or open the VS Code terminal that has it) and re-run.'
}
Write-Host "git: $(Get-GitOutput @('--version'))"

Set-Location -LiteralPath $ProjectRoot

$gitDir = Join-Path $ProjectRoot '.git'
$repoExists = Test-Path -LiteralPath $gitDir
if ($repoExists) {
    $hasBaselineTag = -not [string]::IsNullOrEmpty((Get-GitOutput @('tag', '--list', 'com-09_057')))
    if (-not $hasBaselineTag) {
        throw "A Git repository already exists at $ProjectRoot but has no tag com-09_057. Refusing to continue; inspect it first."
    }
    Write-Host 'Existing repository with tag com-09_057 found: baseline step will be skipped.' -ForegroundColor Yellow
    $d0Done = Get-GitOutput @('log', '--format=%s', '--fixed-strings', '--grep=D0: housekeeping', '-n', '1')
    if (-not [string]::IsNullOrEmpty($d0Done)) {
        Write-Host 'Commit "D0: housekeeping" already exists. Steps 1-5 are complete; only build-folder deletion will run.' -ForegroundColor Yellow
        $script:D0CommitExists = $true
    } else {
        # Resuming an interrupted run: continue, but show what is pending.
        $dirty = Get-GitOutput @('status', '--porcelain')
        if ($dirty) {
            Write-Host 'Resuming: these uncommitted changes will be included in the D0 commit:' -ForegroundColor Yellow
            Write-Host $dirty
        }
    }
}

# Expected .gitignore content (written by the D0 package; checked, not generated).
$ignoreText = Get-Content -LiteralPath (Join-Path $ProjectRoot '.gitignore') -Raw
foreach ($pattern in @('build_*/', 'tools/test_results/', '__pycache__/')) {
    if ($ignoreText -notmatch [regex]::Escape($pattern)) {
        throw ".gitignore does not contain '$pattern'. Copy the D0 .gitignore into the project root first."
    }
}

# ------------------------------------------------------- 1. Git baseline ---
Write-Step '1. Git baseline (tag com-09_057)'
if ($repoExists) {
    Write-Host '  already done'
} else {
    $userName  = Get-GitOutput @('config', '--global', 'user.name')
    $userEmail = Get-GitOutput @('config', '--global', 'user.email')
    if (-not $Apply) {
        Write-Host '  [dry-run] git init; set branch main'
        if (-not $userName -or -not $userEmail) {
            Write-Host '  [dry-run] git user.name / user.email are not set globally; you will be asked for them'
        }
        Write-Host '  [dry-run] git add -A (excluding the D0 package files); commit "Baseline: COM 09_057 as received"; tag com-09_057'
    } else {
        Invoke-Git @('init')
        Invoke-Git @('symbolic-ref', 'HEAD', 'refs/heads/main')
        if (-not $userName)  { $userName  = Read-Host 'Git author name for this repository' ;  Invoke-Git @('config', 'user.name', $userName) }
        if (-not $userEmail) { $userEmail = Read-Host 'Git author e-mail for this repository'; Invoke-Git @('config', 'user.email', $userEmail) }
        Invoke-Git @('config', 'core.longpaths', 'true')

        Invoke-Git @('add', '-A')
        # The D0 package itself belongs to the D0 commit, not to the baseline.
        foreach ($d0Path in @('tools/d0', 'docs/REPOSITORY_LAYOUT.md', 'docs/D0_CHECKLIST.md', 'archive/README.md')) {
            if (Test-Path -LiteralPath (Join-Path $ProjectRoot $d0Path)) {
                # rm --cached works before the first commit (git reset may not).
                Invoke-Git @('rm', '-r', '-q', '--cached', '--', $d0Path)
            }
        }
        Invoke-Git @('commit', '-q', '-m', 'Baseline: COM 09_057 as received', '-m', 'Source tree of myems_rs485_modbus_rtu_09_057 before DP-WP4 milestone D0. Build folders and test results are excluded by .gitignore.')
        Invoke-Git @('tag', '-a', 'com-09_057', '-m', 'COM baseline 09_057 before DP-WP4')
        Write-Host "  baseline commit $(Get-GitOutput @('rev-parse', '--short', 'HEAD')), tag com-09_057"
    }
}

if ($script:D0CommitExists) {
    Write-Step '2-5. Reorganisation and D0 commit'
    Write-Host '  already done'
} else {
    # --------------------------------------------------- 2. Reorganise files ---
    Write-Step '2. Move documents, patches and snapshots'

    # Guides that are still used to run COM tests.
    $toDocsComTests = @(
        'MODBUS_DUAL_MODE_SELF_TEST_GUIDE.md',
        'STAGE3_RELIABILITY_TEST_INSTRUCTIONS.md',
        'README_WP2_STALE_RESPONSE_TEST.md',
        'README_WP24_CASE_SELECTION.md',
        'README_WP24_M.md',
        'README_WP25B2_TEST.md',
        'README_WP25C_TEST.md',
        'README_WP25D_TEST.md',
        'README_WP25E_STACK_INSTRUMENTATION.md',
        'WP24_ALL_A_TO_I_GUIDE.md',
        'WP24_L_README.txt'
    )
    # Current architecture/reference documents.
    $toDocs = @(
        'docs_architecture.md',
        'STATIC_MEMORY_AND_MODBUS_SERVICES.md',
        'MyEMS ESP32-S3 Modbus RTU Project Report.docx'
    )
    # Change notes of finished COM work.
    $toArchiveHistory = @(
        'CORRECTIONS_AND_BUILD.md',
        'PROJECT_REVIEW_AND_CHANGELOG.md',
        'README_STATIC_MEMORY_NEXT_STEPS.md',
        'README_WP2_2.md',
        'README_WP2.5-C.txt',
        'README_WP25B1.md',
        'README_WP25B2.md',
        'README_WP25E1.md',
        'STAGE1_SOURCE_AUDIT_AND_CHANGES.md',
        'WP2_2_IMPLEMENTATION_NOTES.md',
        'WP24_ALL_IMPLEMENTATION_NOTES.md',
        'WP24_ASSERTION_CORRECTION.md',
        'WP24_BUILD_CACHE_CHECK_FIX.md',
        'WP24_BUILD_CONFIG_CORRECTION.md',
        'PATCH_MANIFEST.txt',
        'PATCH_MANIFEST_WP25E.txt',
        'PATCH_NOTES.txt'
    )
    # Patches and diffs already applied to the source tree.
    $toArchivePatches = @(
        'firmware_wp24_m.patch',
        'WP24_J_SOURCE.patch',
        'WP25B1_HEADER.diff',
        'WP25B1_MASTER.diff',
        'WP25B2_MASTER.diff',
        'WP25B2_TEST.diff',
        'WP25E1_SOURCE.diff'
    )
    # Old copies and delivered packages (the live versions are in main\, components\, tools\).
    $toArchiveSnapshots = @(
        'backup_before_wp24_m',
        'wp24_m_package',
        'simulator_source.py',
        'myems_wp24_m_boundary_test.py'
    )
    $toArchiveLogs = @(
        'tools\wp22_simulator.log',
        'tools\wp22_targeted_delay.log',
        'tools\wp22_targeted_delay_1000.log'
    )

    foreach ($f in $toDocsComTests)     { Move-Tracked $f (Join-Path 'docs\com_tests' $f) }
    foreach ($f in $toDocs)             { Move-Tracked $f (Join-Path 'docs' $f) }
    foreach ($f in $toArchiveHistory)   { Move-Tracked $f (Join-Path 'archive\com_history' $f) }
    foreach ($f in $toArchivePatches)   { Move-Tracked $f (Join-Path 'archive\patches' $f) }
    foreach ($f in $toArchiveSnapshots) { Move-Tracked $f (Join-Path 'archive\snapshots' $f) }
    foreach ($f in $toArchiveLogs)      { Move-Tracked $f (Join-Path 'archive\test_logs' (Split-Path $f -Leaf)) }

    # --------------------------------------- 3. concurrency_contract de-dup ---
    Write-Step '3. Keep one concurrency_contract.md (in docs\)'
    $rootCc = Join-Path $ProjectRoot 'concurrency_contract.md'
    $copyCc = Join-Path $ProjectRoot 'concurrency_contract - Copy.md'
    $docsCc = Join-Path $ProjectRoot 'docs\concurrency_contract.md'

    # Decide about the "- Copy" file before anything moves: compare it with the
    # newest contract (root copy if present, otherwise the one already in docs\).
    $copyExists = Test-Path -LiteralPath $copyCc
    $copyIdentical = $false
    if ($copyExists) {
        $reference = $null
        if (Test-Path -LiteralPath $rootCc)     { $reference = $rootCc }
        elseif (Test-Path -LiteralPath $docsCc) { $reference = $docsCc }
        if ($null -ne $reference) {
            $copyIdentical = (Get-FileHash -LiteralPath $copyCc).Hash -eq (Get-FileHash -LiteralPath $reference).Hash
        }
    }

    if (Test-Path -LiteralPath $rootCc) {
        # The root copy is the newer one (WP2.5 reservation design); the docs\ copy predates it.
        if (Test-Path -LiteralPath $docsCc) {
            Move-Tracked 'docs\concurrency_contract.md' 'archive\com_history\concurrency_contract_pre_wp25.md'
        }
        Move-Tracked 'concurrency_contract.md' 'docs\concurrency_contract.md'
    } else {
        Write-Host '  root copy not found; nothing to promote' -ForegroundColor DarkGray
    }
    if ($copyExists) {
        if ($copyIdentical) {
            $script:Planned++
            if ($Apply) {
                Invoke-Git @('rm', '-q', '--', 'concurrency_contract - Copy.md')
                $script:Done++
                Write-Host '  removed identical duplicate: concurrency_contract - Copy.md'
            } else {
                Write-Host '  [dry-run] git rm "concurrency_contract - Copy.md" (identical duplicate)'
            }
        } else {
            # Not identical: keep it, but out of the way.
            Move-Tracked 'concurrency_contract - Copy.md' 'archive\com_history\concurrency_contract_copy.md'
        }
    }

    # ------------------------------------------------ 4. VS Code clangd path ---
    Write-Step '4. .vscode\settings.json: point clangd at this project''s build_production'
    $vsPath = Join-Path $ProjectRoot '.vscode\settings.json'
    if (Test-Path -LiteralPath $vsPath) {
        $vs = Get-Content -LiteralPath $vsPath -Raw
        $pattern = '--compile-commands-dir=[^"]*'
        $replacement = '--compile-commands-dir=${workspaceFolder}/build_production'
        if ($vs -match [regex]::Escape($replacement)) {
            Write-Host '  already correct'
        } elseif ($vs -match $pattern) {
            $script:Planned++
            if ($Apply) {
                $vs = [regex]::Replace($vs, $pattern, { param($m) $replacement })
                [System.IO.File]::WriteAllText($vsPath, $vs, (New-Object System.Text.UTF8Encoding($false)))
                Invoke-Git @('add', '--', '.vscode/settings.json')
                $script:Done++
                Write-Host '  updated'
            } else {
                Write-Host "  [dry-run] replace old 09_053 compile-commands path with $replacement"
            }
        } else {
            Write-Host '  no compile-commands-dir entry; unchanged' -ForegroundColor DarkGray
        }
    }

    # ------------------------------------------------------- 5. D0 commit ---
    Write-Step '5. Commit "D0: housekeeping"'
    if ($Apply) {
        Invoke-Git @('add', '-A')
        $staged = Get-GitOutput @('diff', '--cached', '--name-only')
        if ($staged) {
            Invoke-Git @('commit', '-q', '-m', 'D0: housekeeping', '-m', 'Move test guides to docs/com_tests, reference docs to docs/, change notes, patches and old snapshots to archive/. Keep sdkconfig.* in the root (used by COM build scripts). Add .gitignore, D0 scripts and checklist.')
            Write-Host "  commit $(Get-GitOutput @('rev-parse', '--short', 'HEAD'))"
        } else {
            Write-Host '  nothing to commit'
        }
    } else {
        Write-Host '  [dry-run] git add -A; git commit -m "D0: housekeeping"'
    }
}

# --------------------------------------------- 6. Delete build folders ---
Write-Step '6. Delete regenerable ESP-IDF build folders'
$buildDirs = @(Get-ChildItem -LiteralPath $ProjectRoot -Directory -Force |
               Where-Object { $_.Name -like 'build_wp24*' -or $_.Name -like 'build_wp25*' -or $_.Name -eq 'build' })
if ($SkipBuildDirDeletion) {
    Write-Host '  skipped (-SkipBuildDirDeletion)'
} elseif ($buildDirs.Count -eq 0) {
    Write-Host '  none found'
} else {
    $total = [int64]0
    foreach ($d in $buildDirs) {
        $bytes = Get-FolderBytes $d.FullName
        $total += $bytes
        $script:Planned++
        if ($Apply) {
            # \\?\ prefix avoids MAX_PATH failures inside deep ESP-IDF build trees.
            & cmd.exe /c "rd /s /q `"\\?\$($d.FullName)`""
            if (Test-Path -LiteralPath $d.FullName) {
                Write-Host ("  FAILED to delete {0} (close VS Code / serial monitors using it and re-run)" -f $d.Name) -ForegroundColor Red
            } else {
                Write-Host ("  deleted {0,-32} {1,8:N0} MB" -f $d.Name, ($bytes / 1MB))
                $script:Done++
            }
        } else {
            Write-Host ("  [dry-run] delete {0,-32} {1,8:N0} MB" -f $d.Name, ($bytes / 1MB))
        }
    }
    Write-Host ("  total: {0:N0} MB" -f ($total / 1MB))
}

# ------------------------------------------------------------- summary ---
Write-Step 'Summary'
$drive = (Get-Item -LiteralPath $ProjectRoot).PSDrive
Write-Host ("Free space on {0}: {1:N1} GB" -f $drive.Name, ($drive.Free / 1GB))
if ($Apply) {
    Write-Host "Actions done: $script:Done of $script:Planned planned; $script:Skipped skipped (not found)."
    Write-Host ''
    Get-GitOutput @('log', '--oneline', '--decorate', '-n', '3') | Write-Host
    Write-Host ''
    Write-Host 'Next: run tools\d0\d0_verify_build.ps1 from an ESP-IDF v6.1 PowerShell (REG-01).' -ForegroundColor Green
} else {
    Write-Host "Planned actions: $script:Planned; skipped (not found): $script:Skipped"
    Write-Host 'Dry run only. Re-run with -Apply to make these changes.' -ForegroundColor Green
}
