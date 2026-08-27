# Test: pull command (fetch + merge combo) (ASCII only)
# Covers: fast-forward pull, diverged pull (merge commit), already up-to-date,
# conflict pull, explicit remote arg, negative paths.
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$mgit = Join-Path $root 'build\mgit.exe'
$pass = 0; $fail = 0

function Check($name, $cond) {
    if ($cond) { $script:pass++; Write-Host "[PASS] $name" }
    else { $script:fail++; Write-Host "[FAIL] $name" }
}

function Run($dir, [string[]]$args2) {
    Push-Location $dir
    $out = & $mgit @args2 2>&1 | Out-String
    $code = $LASTEXITCODE
    Pop-Location
    return @{ Out = $out; Code = $code }
}

$base = Join-Path $PSScriptRoot ("pull_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $base) { Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $base | Out-Null

# ---------- setup: server repo + clone ----------
$srv = Join-Path $base 'server'
New-Item -ItemType Directory $srv | Out-Null
$r = Run $srv @('init')
Set-Content (Join-Path $srv 'a.txt') 'line1' -NoNewline
$r = Run $srv @('add','.'); $r = Run $srv @('commit','-m','c1')

$r = Run $base @('clone','server','cl')
Check 'setup: clone exits ok' ($r.Code -eq 0)
$cl = Join-Path $base 'cl'

# ---------- 1. fast-forward pull ----------
Set-Content (Join-Path $srv 'a.txt') 'line2' -NoNewline
$r = Run $srv @('add','.'); $r = Run $srv @('commit','-m','c2-server')

$r = Run $cl @('pull')
Check '1. pull exits ok' ($r.Code -eq 0)
Check '2. pull reports fast-forward' ($r.Out -match 'Fast-forward')
Check '3. worktree updated after pull' ((Get-Content (Join-Path $cl 'a.txt') -Raw) -eq 'line2')
$lg = (Run $cl @('log','--oneline')).Out
Check '4. log shows server commit' ($lg -match 'c2-server')

# ---------- 2. already up to date ----------
$r = Run $cl @('pull')
Check '5. second pull exits ok' ($r.Code -eq 0)
Check '6. second pull says up to date' ($r.Out -match 'Already up to date')

# ---------- 3. diverged pull => merge commit ----------
Set-Content (Join-Path $cl 'b.txt') 'from-clone' -NoNewline
$r = Run $cl @('add','.'); $r = Run $cl @('commit','-m','c3-clone')
Set-Content (Join-Path $srv 'c.txt') 'from-server' -NoNewline
$r = Run $srv @('add','.'); $r = Run $srv @('commit','-m','c4-server')

$r = Run $cl @('pull')
Check '7. diverged pull exits ok' ($r.Code -eq 0)
Check '8. diverged pull creates merge commit' ($r.Out -match 'Merge commit')
Check '9. both files present after merge' ((Test-Path (Join-Path $cl 'b.txt')) -and (Test-Path (Join-Path $cl 'c.txt')))
$lg = (Run $cl @('log','--oneline')).Out
Check '10. log has both sides' (($lg -match 'c3-clone') -and ($lg -match 'c4-server'))
$full = (Run $cl @('log')).Out
Check '11. merge commit has two parents' ($full -match 'Merge:')

# ---------- 4. explicit remote + branch args ----------
$r = Run $cl @('pull','origin','master')
Check '12. pull origin master exits ok' ($r.Code -eq 0)
Check '13. explicit pull up to date' ($r.Out -match 'Already up to date')

# ---------- 5. pull with conflict ----------
Set-Content (Join-Path $srv 'a.txt') 'server-change' -NoNewline
$r = Run $srv @('add','.'); $r = Run $srv @('commit','-m','c5-server-a')
Set-Content (Join-Path $cl 'a.txt') 'clone-change' -NoNewline
$r = Run $cl @('add','.'); $r = Run $cl @('commit','-m','c6-clone-a')

$r = Run $cl @('pull')
Check '14. conflict pull exits nonzero' ($r.Code -ne 0)
Check '15. conflict reported' ($r.Out -match 'CONFLICT')
$content = Get-Content (Join-Path $cl 'a.txt') -Raw
Check '16. conflict markers written' (($content -match '<<<<<<<') -and ($content -match '>>>>>>>'))

# resolve conflict and commit (recovery path)
Set-Content (Join-Path $cl 'a.txt') 'resolved' -NoNewline
$r = Run $cl @('add','.'); $r = Run $cl @('commit','-m','resolve-conflict')
Check '17. commit after conflict resolution ok' ($r.Code -eq 0)
$r = Run $cl @('pull')
Check '18. pull after resolve up to date' (($r.Code -eq 0) -and ($r.Out -match 'Already up to date'))

# ---------- 6. negative paths ----------
$r = Run $cl @('pull','nosuchremote')
Check '19. pull unknown remote fails' ($r.Code -ne 0)

$norepo = Join-Path $base 'empty_dir'
New-Item -ItemType Directory $norepo | Out-Null
$r = Run $norepo @('pull')
Check '20. pull outside repo fails' ($r.Code -ne 0)

# pull for a branch that does not exist on remote
$r = Run $cl @('pull','origin','ghost')
Check '21. pull missing remote branch fails gracefully' (($r.Code -ne 0) -and ($r.Out -match 'no tracking branch'))

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $base '*') /s /d 2>$null
Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
exit 0
