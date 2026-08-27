# Test: argument coverage for parameters never exercised by other suites (ASCII only)
# Covers: commit -a / --amend, add -A, log -n, log <branch>, branch -d,
#         tag -l / tag -d, remote remove, diff <hash> / diff <hash> <hash>
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

function GetTreeOf($dir, $commit) {
    $r = Run $dir @('cat-file','-p',$commit)
    if ($r.Out -match '(?m)^tree ([0-9a-f]{40})') { return $Matches[1] }
    return $null
}

$d = Join-Path $PSScriptRoot ("args_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $d) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $d | Out-Null

# ---------- setup: c1 -> c2 ----------
$r = Run $d @('init')
Set-Content (Join-Path $d 'a.txt') 'v1' -NoNewline
Set-Content (Join-Path $d 'b.txt') 'bee' -NoNewline
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','c1')
$h1 = GetHash $r.Out

Set-Content (Join-Path $d 'a.txt') 'v2' -NoNewline
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','c2')
$h2 = GetHash $r.Out

Check 'setup: two commits created' (($null -ne $h1) -and ($null -ne $h2))

# ---------- 1. commit -a (auto stage modified tracked file) ----------
Set-Content (Join-Path $d 'a.txt') 'v3' -NoNewline
$r = Run $d @('commit','-a','-m','c3-auto')
Check '1. commit -a exits ok' ($r.Code -eq 0)
$h3 = GetHash $r.Out
Check '2. commit -a created new commit' ($null -ne $h3)
$lg = Run $d @('log','--oneline')
Check '3. commit -a staged modification (a.txt=v3 in tree)' ((GetTreeOf $d $h3) -ne (GetTreeOf $d $h2))
$r = Run $d @('status')
Check '4. worktree clean after commit -a' (-not ($r.Out -match 'a\.txt'))

# ---------- 2. commit --amend ----------
Set-Content (Join-Path $d 'a.txt') 'v4' -NoNewline
$r = Run $d @('add','a.txt')
$r = Run $d @('commit','--amend','-m','c3-amended')
Check '5. commit --amend exits ok' ($r.Code -eq 0)
$h3b = GetHash $r.Out
Check '6. amend produces a different commit hash' (($null -ne $h3b) -and ($h3b -ne $h3))
$lg = (Run $d @('log','--oneline')).Out
Check '7. amend replaced message' ($lg -match 'c3-amended')
Check '8. amend removed old message' (-not ($lg -match 'c3-auto'))
$lg2 = (Run $d @('log')).Out
$ccount = ([regex]::Matches($lg2, '(?m)^commit [0-9a-f]{40}')).Count
Check '9. amend keeps commit count at 3' ($ccount -eq 3)

# ---------- 3. commit --amend on repo without commits (negative) ----------
$d_empty = $d + "_empty"
New-Item -ItemType Directory $d_empty | Out-Null
$r = Run $d_empty @('init')
Set-Content (Join-Path $d_empty 'x.txt') 'x' -NoNewline
$r = Run $d_empty @('add','.')
$r = Run $d_empty @('commit','--amend','-m','nope')
Check '10. amend with no HEAD commit fails' ($r.Code -ne 0)

# ---------- 4. add -A ----------
Set-Content (Join-Path $d 'newfile.txt') 'brand new' -NoNewline
$r = Run $d @('add','-A')
Check '11. add -A exits ok' ($r.Code -eq 0)
Check '12. add -A staged the new file' ($r.Out -match 'newfile\.txt')
$dc = (Run $d @('diff','--cached')).Out
Check '13. diff --cached sees new file' ($dc -match 'newfile\.txt')
$r = Run $d @('commit','-m','c4-newfile')
Check '14. commit after add -A ok' ($r.Code -eq 0)

# ---------- 5. log -n <count> ----------
$lg1 = (Run $d @('log','--oneline','-n','1')).Out
$lines1 = ($lg1 -split "`n" | Where-Object { $_ -match '^[0-9a-f]{7} ' }).Count
Check '15. log -n 1 shows exactly 1 line' ($lines1 -eq 1)
$lg2 = (Run $d @('log','--oneline','-n','2')).Out
$lines2 = ($lg2 -split "`n" | Where-Object { $_ -match '^[0-9a-f]{7} ' }).Count
Check '16. log -n 2 shows exactly 2 lines' ($lines2 -eq 2)
Check '17. log -n 1 shows newest commit (c4)' ($lg1 -match 'c4-newfile')

# ---------- 6. log <branch> ----------
$r = Run $d @('branch','side')
$r = Run $d @('checkout','side')
Set-Content (Join-Path $d 'side.txt') 'side change' -NoNewline
$r = Run $d @('add','.'); $r = Run $d @('commit','-m','side-commit')
$r = Run $d @('checkout','master')
$lgm = (Run $d @('log','--oneline','master')).Out
$lgs = (Run $d @('log','--oneline','side')).Out
Check '18. log side shows side-commit' ($lgs -match 'side-commit')
Check '19. log master hides side-commit' (-not ($lgm -match 'side-commit'))
Check '20. log master still shows c1' ($lgm -match 'c1')

# ---------- 7. branch -d ----------
$r = Run $d @('branch','-d','side')
Check '21. branch -d exits ok' ($r.Code -eq 0)
Check '22. branch -d reports deletion' ($r.Out -match 'Deleted branch side')
$br = (Run $d @('branch')).Out
Check '23. deleted branch no longer listed' (-not ($br -match 'side'))
$r = Run $d @('branch','-d','master')
Check '24. cannot delete current branch' ($r.Code -ne 0)

# ---------- 8. tag -l ----------
$r = Run $d @('tag','v1.0')
$r = Run $d @('tag','v2.0')
$tl = (Run $d @('tag','-l')).Out
Check '25. tag -l lists v1.0' ($tl -match 'v1\.0')
Check '26. tag -l lists v2.0' ($tl -match 'v2\.0')

# ---------- 9. tag -d ----------
$r = Run $d @('tag','-d','v1.0')
Check '27. tag -d exits ok' ($r.Code -eq 0)
Check '28. tag -d reports deletion' ($r.Out -match "Deleted tag 'v1.0'")
$tl2 = (Run $d @('tag','-l')).Out
Check '29. deleted tag gone from list' (-not ($tl2 -match 'v1\.0'))
$r = Run $d @('tag','-d','v1.0')
Check '30. tag -d on missing tag fails' ($r.Code -ne 0)

# ---------- 10. remote remove ----------
$r = Run $d @('remote','add','origin',$d)
$rv = (Run $d @('remote','-v')).Out
Check '31. remote add ok' ($rv -match 'origin')
$r = Run $d @('remote','remove','origin')
Check '32. remote remove exits ok' ($r.Code -eq 0)
$rv2 = (Run $d @('remote','-v')).Out
Check '33. removed remote no longer listed' (-not ($rv2 -match 'origin'))
$r = Run $d @('remote','remove','nosuch')
Check '34. remote remove missing fails' ($r.Code -ne 0)
$r = Run $d @('remote','add','origin2',$d)
$r = Run $d @('remote','rm','origin2')
$rv3 = (Run $d @('remote','-v')).Out
Check '35. remote rm alias works' (($r.Code -eq 0) -and (-not ($rv3 -match 'origin2')))

# ---------- 11. diff <tree-hash> and diff <tree> <tree> ----------
$t1 = GetTreeOf $d $h1
$t4 = GetTreeOf $d $h3b
Check '36. setup: tree hashes extracted' (($null -ne $t1) -and ($null -ne $t4))

$r = Run $d @('diff',$t1)
Check '37. diff <tree> vs HEAD exits ok' ($r.Code -eq 0)
Check '38. diff <tree> reports a.txt modified' ($r.Out -match 'a\.txt \(modified\)')
Check '39. diff <tree> reports newfile.txt deleted (HEAD -> t1 direction)' ($r.Out -match 'newfile\.txt \(deleted\)')

$r = Run $d @('diff',$t1,$t4)
Check '40. diff <tree> <tree> exits ok' ($r.Code -eq 0)
Check '41. two-tree diff reports a.txt modified' ($r.Out -match 'a\.txt \(modified\)')

$r = Run $d @('diff','zzzz')
Check '42. diff with bad hash fails' ($r.Code -ne 0)

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $d '*') /s /d 2>$null
attrib -r (Join-Path $d_empty '*') /s /d 2>$null
Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $d_empty -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
exit 0
