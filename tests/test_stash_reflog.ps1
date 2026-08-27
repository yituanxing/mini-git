# Test: stash (push/pop/list/drop) and reflog records (ASCII only)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
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

$d = Join-Path $PSScriptRoot ("st_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $d) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $d | Out-Null

# ---------- setup: one commit ----------
$r = Run $d @('init')
Set-Content (Join-Path $d 'a.txt') 'base'
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','c1')
if ($r.Out -match '\[.* ([0-9a-f]{40})\]') { $h1 = $Matches[1] } else { $h1 = $null }

# ---------- stash push / pop ----------
Set-Content (Join-Path $d 'a.txt') 'dirty1'
$r = Run $d @('stash','push')
Check '1. stash push exits ok' ($r.Code -eq 0)
Check '2. push reports saved stash' ($r.Out -match 'Saved')

$wt = Get-Content (Join-Path $d 'a.txt')
Check '3. working tree clean after push' ($wt -eq 'base')

$r = Run $d @('stash','list')
Check '4. stash list shows stash@{0}' ($r.Out -match 'stash@\{0\}')

$r = Run $d @('stash','pop')
Check '5. stash pop exits ok' ($r.Code -eq 0)

$wt = Get-Content (Join-Path $d 'a.txt')
Check '6. pop restores changes' ($wt -eq 'dirty1')

$r = Run $d @('stash','list')
Check '7. stash list empty after pop' (-not ($r.Out -match 'stash@\{0\}'))

# ---------- two stashes + drop ----------
Set-Content (Join-Path $d 'a.txt') 'dirty2'
$r = Run $d @('stash','push')
Set-Content (Join-Path $d 'a.txt') 'dirty3'
$r = Run $d @('stash','push')

$r = Run $d @('stash','list')
Check '8. two stashes recorded' (($r.Out -match 'stash@\{0\}') -and ($r.Out -match 'stash@\{1\}'))

$r = Run $d @('stash','drop')
Check '9. stash drop exits ok' ($r.Code -eq 0)

$r = Run $d @('stash','list')
Check '10. one stash left after drop' (-not ($r.Out -match 'stash@\{1\}'))

$r = Run $d @('stash','pop')
$wt = Get-Content (Join-Path $d 'a.txt')
Check '11. pop restores remaining stash (dirty2)' ($wt -eq 'dirty2')

# ---------- reflog records ----------
$r = Run $d @('reflog')
Check '12. reflog has commit record' ($r.Out -match 'commit:')
Check '13. reflog entry format old -> new' ($r.Out -match '[0-9a-f]{7} -> [0-9a-f]{7}')

Set-Content (Join-Path $d 'a.txt') 'for-c2'
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','c2')
$r = Run $d @('branch','feat')
$r = Run $d @('checkout','feat')

$r = Run $d @('reflog')
Check '14. reflog records checkout move' ($r.Out -match 'checkout')

# ---------- reflog recovery after reset --hard ----------
$r = Run $d @('checkout','master')
$r = Run $d @('log','--oneline')
if ($r.Out -match '([0-9a-f]{7}) c2') { $s2 = $Matches[1] } else { $s2 = $null }

$r = Run $d @('reset','--hard',$h1)
Check '15. reset --hard back to c1 ok' ($r.Code -eq 0)

$r = Run $d @('reflog')
Check '16. reflog keeps record after reset' ($r.Out -match 'reset')

if ($null -ne $s2) {
    $r = Run $d @('reset','--hard',$s2)
    $wt = Get-Content (Join-Path $d 'a.txt')
    Check '17. recover c2 via reflog short hash' (($r.Code -eq 0) -and ($wt -eq 'for-c2'))
} else {
    Check '17. recover c2 via reflog short hash' $false
}

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $d '*') /s /d 2>$null
Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
