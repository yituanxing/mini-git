# Test: gc + packfile + bidirectional compatibility with real git
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$mgit = Join-Path $root 'build\mgit.exe'
$pass = 0; $fail = 0

function Check($name, $cond) {
    if ($cond) { $script:pass++; Write-Host "[PASS] $name" }
    else { $script:fail++; Write-Host "[FAIL] $name" }
}

# ---- 1. mgit gc full cycle ----
$t1 = Join-Path $PSScriptRoot ("tmp_gc1_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $t1) { Remove-Item $t1 -Recurse -Force }
New-Item -ItemType Directory $t1 | Out-Null
Push-Location $t1
& $mgit init 2>&1 | Out-Null
'v1' | Set-Content f.txt
& $mgit add f.txt 2>&1 | Out-Null
& $mgit commit -m c1 2>&1 | Out-Null
'v2' | Set-Content f.txt
& $mgit add f.txt 2>&1 | Out-Null
& $mgit commit -m c2 2>&1 | Out-Null

$before = & $mgit count-objects 2>&1 | Out-String
Check '1. loose objects exist before gc' ($before -match '6 objects')
& $mgit gc 2>&1 | Out-Null
$after = & $mgit count-objects -v 2>&1 | Out-String
Check '2. no loose objects after gc' ($after -match 'count: 0')
Check '3. objects packed' ($after -match 'in-pack: 6')
$lg = & $mgit log --oneline 2>&1 | Out-String
Check '4. mgit log reads from pack' (($lg -match 'c1') -and ($lg -match 'c2'))

git fsck --full 2>&1 | Out-Null
Check '5. git fsck passes on mgit pack' ($LASTEXITCODE -eq 0)
$glog = git log --oneline 2>&1 | Out-String
Check '6. git log reads mgit pack' (($glog -match 'c1') -and ($glog -match 'c2'))
$head = git rev-parse HEAD 2>&1
$cat = & $mgit cat-file -t $head 2>&1
Check '7. mgit cat-file works on packed commit' ($cat -match 'commit')
$short = $head.Substring(0,7)
$cat2 = & $mgit cat-file -t $short 2>&1
Check '8. short hash resolves inside pack' ($cat2 -match 'commit')
Pop-Location

# ---- 2. mgit reads git-generated delta pack + packed-refs ----
$t2 = Join-Path $PSScriptRoot ("tmp_gc2_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $t2) { Remove-Item $t2 -Recurse -Force }
New-Item -ItemType Directory $t2 | Out-Null
Push-Location $t2
git init -q
git config user.email t@example.com
git config user.name tester
[IO.File]::WriteAllText((Join-Path $t2 'a.txt'), (("alpha line" + [char]10) * 300))
git add . 2>&1 | Out-Null
git commit -qm c1
[IO.File]::WriteAllText((Join-Path $t2 'a.txt'), (("alpha line" + [char]10) * 299 + "beta line" + [char]10))
git commit -qam c2
git gc -q

$lg2 = & $mgit log --oneline 2>&1 | Out-String
Check '9. mgit reads packed-refs (git gc)' (($lg2 -match 'c1') -and ($lg2 -match 'c2'))
$blob = (git ls-tree HEAD a.txt) -split '\s+' | Select-Object -Index 2
$blob = $blob.Trim()
$body = & $mgit cat-file -p $blob 2>&1 | Out-String
Check '10. mgit decodes OFS_DELTA blob' ($body -match 'beta line')
$tp = & $mgit cat-file -t $blob 2>&1
Check '11. delta blob type is blob' ($tp -match 'blob')
Pop-Location

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $t1 '*') /s /d 2>$null
attrib -r (Join-Path $t2 '*') /s /d 2>$null
Remove-Item $t1, $t2 -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
