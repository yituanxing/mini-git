# Test: reset (--hard, pointer move) and revert (ASCII only)
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

function GetHash($out) {
    if ($out -match '\[.* ([0-9a-f]{40})\]') { return $Matches[1] }
    return $null
}

$d = Join-Path $PSScriptRoot ("rr_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $d) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $d | Out-Null

# ---------- setup: c1 -> c2 ----------
$r = Run $d @('init')
Set-Content (Join-Path $d 'a.txt') 'line1'
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','c1')
$h1 = GetHash $r.Out

Set-Content (Join-Path $d 'a.txt') 'line2'
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','c2')
$h2 = GetHash $r.Out
$s1 = $h1.Substring(0,7); $s2 = $h2.Substring(0,7)

Check 'setup: two commits created' (($null -ne $h1) -and ($null -ne $h2))

# ---------- default reset: pointer only ----------
$r = Run $d @('reset',$h1)
Check '1. default reset exits ok' ($r.Code -eq 0)

$r = Run $d @('log','--oneline')
Check '2. reset moves branch pointer (c2 gone from log)' (-not ($r.Out -match [regex]::Escape($s2)))

$wt = Get-Content (Join-Path $d 'a.txt')
Check '3. default reset keeps working tree (a.txt still line2)' ($wt -eq 'line2')

# ---------- reset --hard ----------
$r = Run $d @('reset','--hard',$h1)
Check '4. reset --hard exits ok' ($r.Code -eq 0)

$wt = Get-Content (Join-Path $d 'a.txt')
Check '5. reset --hard restores working tree (a.txt back to line1)' ($wt -eq 'line1')

# ---------- reflog-based recovery ----------
$r = Run $d @('reflog')
Check '6. reflog records the lost commit' ($r.Out -match [regex]::Escape($s2))

$r = Run $d @('reset','--hard',$s2)
Check '7. recover lost commit via short hash' ($r.Code -eq 0)

$r = Run $d @('log','--oneline')
Check '8. lost commit back in log' ($r.Out -match [regex]::Escape($s2))

$wt = Get-Content (Join-Path $d 'a.txt')
Check '9. recovered content correct' ($wt -eq 'line2')

# ---------- revert ----------
Set-Content (Join-Path $d 'a.txt') 'line3'
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','c3')
$h3 = GetHash $r.Out
$s3 = $h3.Substring(0,7)

$r = Run $d @('revert',$h3)
Check '10. revert exits ok' ($r.Code -eq 0)

$wt = Get-Content (Join-Path $d 'a.txt')
Check '11. revert undoes content (a.txt back to line2)' ($wt -eq 'line2')

$r = Run $d @('log','--oneline')
Check '12. revert creates new Revert commit' ($r.Out -match 'Revert')
Check '13. history keeps c3 (no rewrite)' ($r.Out -match [regex]::Escape($s3))

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $d '*') /s /d 2>$null
Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
