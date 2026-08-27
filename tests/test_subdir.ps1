$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$mgit = Join-Path $root 'build\mgit.exe'
$repo = Join-Path $PSScriptRoot ("tmp_subdir_" + (Get-Date -Format 'HHmmss'))

if (Test-Path $repo) { Remove-Item $repo -Recurse -Force }
New-Item -ItemType Directory -Path $repo -Force | Out-Null
Push-Location $repo

$pass = 0; $fail = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "  PASS: $name" -ForegroundColor Green; $script:pass++ }
    else { Write-Host "  FAIL: $name" -ForegroundColor Red; $script:fail++ }
}

Write-Host "=== 1. init + nested dirs ==="
& $mgit init | Out-Null
New-Item -ItemType Directory -Path "src\util" -Force | Out-Null
Set-Content -Path "README.md" -Value "hello mgit" -NoNewline
Set-Content -Path "src\main.c" -Value "int main(){}" -NoNewline
Set-Content -Path "src\util\helper.c" -Value "void help(){}" -NoNewline

& $mgit add . | Out-Null
& $mgit commit -m "initial with subdirs" | Out-Null
Check "commit ok" ($LASTEXITCODE -eq 0)

Write-Host "=== 2. ls-tree nested ==="
$lsout = (& $mgit ls-tree HEAD | Out-String)
Write-Host $lsout
Check "src tree entry" ($lsout -match "tree\s+[0-9a-f]+\s+src")
Check "README.md entry" ($lsout -match "blob\s+[0-9a-f]+\s+README.md")

Write-Host "=== 3. status clean ==="
$st = (& $mgit status | Out-String)
Check "working tree clean" ($st -match "working tree clean")

Write-Host "=== 4. modify subdir file + diff --cached ==="
Set-Content -Path "src\main.c" -Value "int main(){return 1;}" -NoNewline
& $mgit add src\main.c | Out-Null
$dc = (& $mgit diff --cached | Out-String)
Check "diff --cached shows src/main.c" ($dc -match "src/main.c")
& $mgit commit -m "modify subdir file" | Out-Null
Check "commit2 ok" ($LASTEXITCODE -eq 0)

Write-Host "=== 5. branch switch syncs subdir files ==="
& $mgit checkout -b feature | Out-Null
Set-Content -Path "src\util\helper.c" -Value "void help(){ v2 }" -NoNewline
& $mgit add . | Out-Null
& $mgit commit -m "feature change helper" | Out-Null

& $mgit checkout master | Out-Null
$helper = Get-Content "src\util\helper.c" -Raw
Check "helper.c restored on master" ($helper -eq "void help(){}")

Write-Host "=== 6. merge across branches ==="
& $mgit merge feature | Out-Null
$helper2 = Get-Content "src\util\helper.c" -Raw
Check "helper.c updated after merge" ($helper2 -eq "void help(){ v2 }")

Write-Host "=== 7. checkout recreates deep dirs ==="
& $mgit checkout feature | Out-Null
New-Item -ItemType Directory -Path "deep\a\b" -Force | Out-Null
Set-Content -Path "deep\a\b\f.txt" -Value "deep file" -NoNewline
& $mgit add . | Out-Null
& $mgit commit -m "deep nested file" | Out-Null
& $mgit checkout master | Out-Null
Check "deep file gone on master" (-not (Test-Path "deep\a\b\f.txt"))
& $mgit checkout feature | Out-Null
Check "deep file restored on feature" (Test-Path "deep\a\b\f.txt")

Write-Host "=== 8. untracked dir shown ==="
New-Item -ItemType Directory -Path "newdir" -Force | Out-Null
Set-Content -Path "newdir\n.txt" -Value "n" -NoNewline
$st2 = (& $mgit status | Out-String)
Check "status shows newdir/" ($st2 -match "newdir/")

Pop-Location

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $repo '*') /s /d 2>$null
Remove-Item $repo -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Result: $pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
exit 0
