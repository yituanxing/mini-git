# Test: adversarial / corrupted-data robustness (ASCII only)
# Verifies that corrupted repository data causes graceful errors, never crashes.
# Targets the guards added in the review round: delta bounds, inflate loop guard,
# reflog line check, packed-refs parsing, pack/idx validation.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$mgit = Join-Path $root 'build\mgit.exe'
$pass = 0; $fail = 0

# Windows access-violation / stack-overrun exit codes
$CRASH_CODES = @(-1073741819, 3221225477, -1073740791, 3221226487)

function Check($name, $cond) {
    if ($cond) { $script:pass++; Write-Host "[PASS] $name" }
    else { $script:fail++; Write-Host "[FAIL] $name" }
}

function NotCrash($code) {
    return ($CRASH_CODES -notcontains $code)
}

function Run($dir, [string[]]$args2) {
    Push-Location $dir
    $out = & $mgit @args2 2>&1 | Out-String
    $code = $LASTEXITCODE
    Pop-Location
    return @{ Out = $out; Code = $code }
}

$base = Join-Path $PSScriptRoot ("corrupt_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $base) { Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $base | Out-Null

# ---------- R1: reflog with garbage lines ----------
$d1 = Join-Path $base 'reflog'
New-Item -ItemType Directory $d1 | Out-Null
$r = Run $d1 @('init')
Set-Content (Join-Path $d1 'a.txt') 'v1' -NoNewline
$r = Run $d1 @('add','.'); $r = Run $d1 @('commit','-m','c1')
Add-Content (Join-Path $d1 '.git\logs\HEAD') "x"
Add-Content (Join-Path $d1 '.git\logs\HEAD') "too short"
Add-Content (Join-Path $d1 '.git\logs\HEAD') ("garbage" * 200)
$r = Run $d1 @('reflog')
Check 'R1.1 reflog survives garbage lines (no crash)' (NotCrash $r.Code)
Check 'R1.2 reflog still shows valid entry' ($r.Out -match 'c1')
Check 'R1.3 reflog exits ok' ($r.Code -eq 0)

# ---------- R2: truncated loose blob ----------
$d2 = Join-Path $base 'blob'
New-Item -ItemType Directory $d2 | Out-Null
$r = Run $d2 @('init')
Set-Content (Join-Path $d2 'f.txt') ('payload payload payload') -NoNewline
$r = Run $d2 @('add','.'); $r = Run $d2 @('commit','-m','c1')
$bh = (& $mgit hash-object (Join-Path $d2 'f.txt') 2>$null | Out-String).Trim()
$obj = Join-Path $d2 (".git\objects\" + $bh.Substring(0,2) + "\" + $bh.Substring(2))
$bytes = [System.IO.File]::ReadAllBytes($obj)
[System.IO.File]::WriteAllBytes($obj, $bytes[0..([int]($bytes.Length / 2))])
$r = Run $d2 @('cat-file','-p',$bh)
Check 'R2.1 truncated blob: no crash' (NotCrash $r.Code)
Check 'R2.2 truncated blob: graceful error' ($r.Code -ne 0)

# ---------- R3: loose object overwritten with garbage ----------
[System.IO.File]::WriteAllBytes($obj, [byte[]](1..64))
$r = Run $d2 @('cat-file','-t',$bh)
Check 'R3.1 garbage object: no crash' (NotCrash $r.Code)
Check 'R3.2 garbage object: graceful error' ($r.Code -ne 0)

# ---------- R4: corrupt HEAD file ----------
[System.IO.File]::WriteAllBytes((Join-Path $d2 '.git\HEAD'), [System.Text.Encoding]::ASCII.GetBytes("garbage data"))
$r = Run $d2 @('log')
Check 'R4.1 corrupt HEAD: no crash' (NotCrash $r.Code)
Check 'R4.2 corrupt HEAD: graceful error' ($r.Code -ne 0)

# ---------- R5/R6: corrupted pack and idx after gc ----------
$d3 = Join-Path $base 'pack'
New-Item -ItemType Directory $d3 | Out-Null
$r = Run $d3 @('init')
Set-Content (Join-Path $d3 'p.txt') 'pack me 1' -NoNewline
$r = Run $d3 @('add','.'); $r = Run $d3 @('commit','-m','pc1')
$ch = $null
$full = (Run $d3 @('log')).Out
if ($full -match '(?m)^commit ([0-9a-f]{40})') { $ch = $Matches[1] }
Set-Content (Join-Path $d3 'p.txt') 'pack me 2' -NoNewline
$r = Run $d3 @('add','.'); $r = Run $d3 @('commit','-m','pc2')
$r = Run $d3 @('gc')
Check 'R5.0 gc packed the repo' ($r.Code -eq 0)

# R5: truncate the .pack to 30 bytes (idx still claims objects exist)
$packs = Get-ChildItem (Join-Path $d3 '.git\objects\pack\*.pack')
Check 'R5.1 pack file exists after gc' ($packs.Count -ge 1)
$packJunk = [System.Text.Encoding]::ASCII.GetBytes("PACK") + [byte[]](1..26)
[System.IO.File]::WriteAllBytes($packs[0].FullName, $packJunk)
$r = Run $d3 @('cat-file','-p',$ch)
Check 'R5.2 truncated pack: no crash' (NotCrash $r.Code)
Check 'R5.3 truncated pack: graceful error' ($r.Code -ne 0)
$r = Run $d3 @('log')
Check 'R5.4 log on truncated pack: no crash' (NotCrash $r.Code)

# R6: corrupt the .idx (garbage bytes, magic check must reject)
$idxs = Get-ChildItem (Join-Path $d3 '.git\objects\pack\*.idx')
[System.IO.File]::WriteAllBytes($idxs[0].FullName, [byte[]](1..48))
$r = Run $d3 @('cat-file','-p',$ch)
Check 'R6.1 garbage idx: no crash' (NotCrash $r.Code)
Check 'R6.2 garbage idx: graceful error' ($r.Code -ne 0)
$r = Run $d3 @('log')
Check 'R6.3 log with garbage idx: no crash' (NotCrash $r.Code)

# ---------- R7: garbage packed-refs ----------
$d4 = Join-Path $base 'prefs'
New-Item -ItemType Directory $d4 | Out-Null
$r = Run $d4 @('init')
Set-Content (Join-Path $d4 'q.txt') 'qq' -NoNewline
$r = Run $d4 @('add','.'); $r = Run $d4 @('commit','-m','qc1')
[System.IO.File]::WriteAllBytes((Join-Path $d4 '.git\packed-refs'), [byte[]](200,201,202,10,5,5,5,255))
$r = Run $d4 @('log')
Check 'R7.1 garbage packed-refs: no crash' (NotCrash $r.Code)
Check 'R7.2 garbage packed-refs: log still works' (($r.Code -eq 0) -and ($r.Out -match 'qc1'))
$r = Run $d4 @('branch')
Check 'R7.3 branch list unaffected' (($r.Code -eq 0) -and ($r.Out -match 'master'))

# ---------- cleanup ----------
attrib -r (Join-Path $base '*') /s /d 2>$null
Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
exit 0
