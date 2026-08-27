# mini-git compatibility test
$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent $PSScriptRoot
$MGIT = Join-Path $ROOT 'build\mgit.exe'
$WORK = Join-Path $PSScriptRoot ("tmp_compat_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $WORK) { Remove-Item $WORK -Recurse -Force }
New-Item -ItemType Directory $WORK | Out-Null
Push-Location $WORK
$GIT = "git.exe"
$PASS = 0; $FAIL = 0; $TOTAL = 0

function DoTest($name, $ok) {
    $script:TOTAL++
    if ($ok) {
        Write-Host "  [$($script:TOTAL)] PASS  $name" -ForegroundColor Green
        $script:PASS++
    } else {
        Write-Host "  [$($script:TOTAL)] FAIL  $name" -ForegroundColor Red
        $script:FAIL++
    }
}

Write-Host ""
Write-Host "========================================"
Write-Host "  mini-git Compatibility Test"
Write-Host "========================================"
Write-Host ""

# ============================================================
# Test 1: Hash Consistency
# ============================================================
Write-Host "[1/5] Hash Consistency" -ForegroundColor Yellow

foreach ($d in @("chash1","chash2")) { Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue }

New-Item -ItemType Directory chash1 | Out-Null
Push-Location chash1
& $MGIT init 2>&1 | Out-Null
[byte[]]$content = [System.Text.Encoding]::UTF8.GetBytes("hello world`n")
[System.IO.File]::WriteAllBytes("$PWD\test.txt", $content)
$mgit_hash = (& $MGIT hash-object test.txt 2>&1 | Select-String "^[0-9a-f]{40}" | ForEach-Object { $_.Matches[0].Value })
Pop-Location

New-Item -ItemType Directory chash2 | Out-Null
Push-Location chash2
& $GIT init 2>&1 | Out-Null
& $GIT config core.autocrlf false 2>&1 | Out-Null
[System.IO.File]::WriteAllBytes("$PWD\test.txt", $content)
$git_hash = (& $GIT hash-object test.txt 2>&1 | Select-String "^[0-9a-f]{40}" | ForEach-Object { $_.Matches[0].Value })
Pop-Location

DoTest "Same content => same blob hash ($mgit_hash)" ($mgit_hash -eq $git_hash)

[byte[]]$bin = @(0,1,2,3,128,255,254,253,100,200)
Push-Location chash1
[System.IO.File]::WriteAllBytes("$PWD\binary.bin", $bin)
$mgit_bin = (& $MGIT hash-object binary.bin 2>&1 | Select-String "^[0-9a-f]{40}" | ForEach-Object { $_.Matches[0].Value })
Pop-Location
Push-Location chash2
[System.IO.File]::WriteAllBytes("$PWD\binary.bin", $bin)
$git_bin = (& $GIT hash-object binary.bin 2>&1 | Select-String "^[0-9a-f]{40}" | ForEach-Object { $_.Matches[0].Value })
Pop-Location

DoTest "Binary content => same blob hash ($mgit_bin)" ($mgit_bin -eq $git_bin)

# ============================================================
# Test 2: mgit reads git objects
# ============================================================
Write-Host ""
Write-Host "[2/5] mgit reads git objects" -ForegroundColor Yellow

Remove-Item -Recurse -Force cread_g -ErrorAction SilentlyContinue
New-Item -ItemType Directory cread_g | Out-Null
Push-Location cread_g

& $GIT init 2>&1 | Out-Null
& $GIT config core.autocrlf false 2>&1 | Out-Null
[byte[]]$fc = [System.Text.Encoding]::UTF8.GetBytes("content from git`n")
[System.IO.File]::WriteAllBytes("$PWD\file1.txt", $fc)
& $GIT add file1.txt 2>&1 | Out-Null
& $GIT -c user.name="Tester" -c user.email="t@t.com" commit -m "git commit" 2>&1 | Out-Null
$git_commit = (& $GIT rev-parse HEAD 2>&1).Trim()
$git_tree = (& $GIT rev-parse "HEAD^{tree}" 2>&1).Trim()
$git_blob = (& $GIT hash-object file1.txt 2>&1).Trim()

$mgit_log = & $MGIT log 2>&1 | Out-String
DoTest "mgit log parses git commit ($($git_commit.Substring(0,7)))" ($mgit_log -match $git_commit.Substring(0,7))
DoTest "mgit log shows author" ($mgit_log -match "Tester")

$mgit_ls = & $MGIT ls-tree $git_tree 2>&1 | Out-String
DoTest "mgit ls-tree parses git tree" ($mgit_ls -match "file1.txt")

$mgit_cat = & $MGIT cat-file -p $git_blob 2>&1 | Out-String
DoTest "mgit cat-file reads git blob" ($mgit_cat -match "content from git")

Pop-Location

# ============================================================
# Test 3: git reads mgit objects
# ============================================================
Write-Host ""
Write-Host "[3/5] git reads mgit objects" -ForegroundColor Yellow

Remove-Item -Recurse -Force cread_m -ErrorAction SilentlyContinue
New-Item -ItemType Directory cread_m | Out-Null
Push-Location cread_m

& $MGIT init 2>&1 | Out-Null
[byte[]]$fc2 = [System.Text.Encoding]::UTF8.GetBytes("content from mgit`n")
[System.IO.File]::WriteAllBytes("$PWD\mgit_file.txt", $fc2)
& $MGIT add mgit_file.txt 2>&1 | Out-Null
$commit_out = & $MGIT commit -m "mgit commit" 2>&1 | Out-String
$mgit_commit = ($commit_out | Select-String "\[.* ([0-9a-f]{40})\]" | ForEach-Object { $_.Matches[0].Groups[1].Value })

$git_cat = & $GIT cat-file -p $mgit_commit 2>&1 | Out-String
DoTest "git cat-file parses mgit commit" ($git_cat -match "tree" -and $git_cat -match "mgit commit")

$git_ls = & $GIT ls-tree "$mgit_commit^{tree}" 2>&1 | Out-String
DoTest "git ls-tree parses mgit tree" ($git_ls -match "mgit_file.txt")

$mgit_blob = (& $MGIT hash-object mgit_file.txt 2>&1 | Select-String "^[0-9a-f]{40}" | ForEach-Object { $_.Matches[0].Value })
$git_blob_c = & $GIT cat-file -p $mgit_blob 2>&1 | Out-String
DoTest "git cat-file reads mgit blob" ($git_blob_c -match "content from mgit")

$git_log = & $GIT log --oneline 2>&1 | Out-String
DoTest "git log shows mgit commit" ($git_log -match "mgit commit")

Pop-Location

# ============================================================
# Test 4: Index interop
# ============================================================
Write-Host ""
Write-Host "[4/5] Index interop" -ForegroundColor Yellow

Remove-Item -Recurse -Force cindex -ErrorAction SilentlyContinue
New-Item -ItemType Directory cindex | Out-Null
Push-Location cindex

& $GIT init 2>&1 | Out-Null
& $GIT config core.autocrlf false 2>&1 | Out-Null
[byte[]]$fa = [System.Text.Encoding]::UTF8.GetBytes("aaa`n")
[System.IO.File]::WriteAllBytes("$PWD\a.txt", $fa)
& $GIT add a.txt 2>&1 | Out-Null
& $GIT -c user.name="T" -c user.email="t@t.com" commit -m "init" 2>&1 | Out-Null

[byte[]]$fb = [System.Text.Encoding]::UTF8.GetBytes("bbb`n")
[System.IO.File]::WriteAllBytes("$PWD\b.txt", $fb)
& $GIT add b.txt 2>&1 | Out-Null
$st1 = & $MGIT status 2>&1 | Out-String
DoTest "mgit reads git Index (sees b.txt)" ($st1 -match "b.txt")

[byte[]]$fcc = [System.Text.Encoding]::UTF8.GetBytes("ccc`n")
[System.IO.File]::WriteAllBytes("$PWD\c.txt", $fcc)
& $MGIT add c.txt 2>&1 | Out-Null
$st2 = & $GIT status --porcelain 2>&1 | Out-String
DoTest "git reads mgit-updated Index (sees c.txt)" ($st2 -match "c.txt")

Pop-Location

# ============================================================
# Test 5: Ref system interop
# ============================================================
Write-Host ""
Write-Host "[5/5] Ref system interop" -ForegroundColor Yellow

Remove-Item -Recurse -Force crefs -ErrorAction SilentlyContinue
New-Item -ItemType Directory crefs | Out-Null
Push-Location crefs

& $MGIT init 2>&1 | Out-Null
[byte[]]$ff = [System.Text.Encoding]::UTF8.GetBytes("fff`n")
[System.IO.File]::WriteAllBytes("$PWD\f.txt", $ff)
& $MGIT add f.txt 2>&1 | Out-Null
& $MGIT commit -m "mgit init" 2>&1 | Out-Null

& $MGIT tag mytag 2>&1 | Out-Null
& $MGIT branch feature 2>&1 | Out-Null
$git_tags = & $GIT tag -l 2>&1 | Out-String
$git_br = & $GIT branch 2>&1 | Out-String
DoTest "git reads mgit tag" ($git_tags -match "mytag")
DoTest "git reads mgit branch" ($git_br -match "feature")

& $GIT tag gittag 2>&1 | Out-Null
& $GIT branch gitbranch 2>&1 | Out-Null
$mgit_tags = & $MGIT tag 2>&1 | Out-String
$mgit_br = & $MGIT branch 2>&1 | Out-String
DoTest "mgit reads git tag" ($mgit_tags -match "gittag")
DoTest "mgit reads git branch" ($mgit_br -match "gitbranch")

Pop-Location

# ============================================================
# Cleanup
# ============================================================
foreach ($d in @("chash1","chash2","cread_g","cread_m","cindex","crefs")) {
    Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
}

# ============================================================
# Summary
# ============================================================
Write-Host ""
Write-Host "========================================"
$color = if ($FAIL -eq 0) { "Green" } else { "Red" }
Write-Host "  Result: $PASS/$TOTAL passed" -ForegroundColor $color
if ($FAIL -gt 0) { Write-Host "  Failed: $FAIL" -ForegroundColor Red }
Write-Host "========================================"
Write-Host ""
Pop-Location

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $WORK '*') /s /d 2>$null
Remove-Item $WORK -Recurse -Force -ErrorAction SilentlyContinue

if ($FAIL -gt 0) { exit 1 }
exit 0
