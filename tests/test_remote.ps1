$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$mgit = Join-Path $root 'build\mgit.exe'
$base = Join-Path $PSScriptRoot ("tmp_remote_" + (Get-Date -Format 'HHmmss'))

if (Test-Path $base) { Remove-Item $base -Recurse -Force }
New-Item -ItemType Directory -Path $base -Force | Out-Null
Push-Location $base

$pass = 0; $fail = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "  PASS: $name" -ForegroundColor Green; $script:pass++ }
    else { Write-Host "  FAIL: $name" -ForegroundColor Red; $script:fail++ }
}

Write-Host "=== 1. setup: empty server repo ==="
& $mgit init server | Out-Null
Check "server initialized" (Test-Path "server\.git")

Write-Host "=== 2. local repo with commits ==="
New-Item -ItemType Directory -Path "local_a\src\util" -Force | Out-Null
Set-Location local_a
& $mgit init | Out-Null
Set-Content README.md "hello" -NoNewline
Set-Content "src\main.c" "int main(){}" -NoNewline
Set-Content "src\util\helper.c" "void h(){}" -NoNewline
& $mgit add . | Out-Null
& $mgit commit -m "c1" | Out-Null
Set-Content README.md "hello v2" -NoNewline
& $mgit add . | Out-Null
& $mgit commit -m "c2" | Out-Null
Check "local_a has 2 commits" ($LASTEXITCODE -eq 0)

Write-Host "=== 3. remote add / list ==="
& $mgit remote add origin "$base\server" | Out-Null
$rv = (& $mgit remote -v | Out-String)
Check "remote -v shows origin" ($rv -match "origin")
& $mgit remote add origin "$base\server" 2>&1 | Out-Null
Check "duplicate remote rejected" ($LASTEXITCODE -ne 0)

Write-Host "=== 4. push (new branch) ==="
$pout = (& $mgit push origin master 2>&1 | Out-String)
Write-Host $pout
Check "push reports new branch" ($pout -match "new branch")
Check "server has master ref" (Test-Path "$base\server\.git\refs\heads\master")

Write-Host "=== 5. clone from server ==="
Set-Location $base
& $mgit clone server clone_b | Out-Null
Check "clone_b README exists" (Test-Path "clone_b\README.md")
Check "clone_b subdir file exists" (Test-Path "clone_b\src\util\helper.c")
Check "clone_b content correct" ((Get-Content "clone_b\README.md" -Raw) -eq "hello v2")
Set-Location clone_b
$lg = (& $mgit log | Out-String)
Check "clone_b log has c1" ($lg -match "c1")
$cfg = Get-Content .git\config -Raw
Check "clone_b config has origin" ($cfg -match "origin")

Write-Host "=== 6. commit in clone_b and push back ==="
Set-Content README.md "hello v3" -NoNewline
& $mgit add . | Out-Null
& $mgit commit -m "c3 from clone_b" | Out-Null
$pout2 = (& $mgit push origin master 2>&1 | Out-String)
Write-Host $pout2
Check "second push ok" ($pout2 -match "master -> master")

Write-Host "=== 7. fetch in local_a ==="
Set-Location "$base\local_a"
$fout = (& $mgit fetch origin 2>&1 | Out-String)
Write-Host $fout
Check "tracking ref created" (Test-Path ".git\refs\remotes\origin\master")
$trackHash = Get-Content ".git\refs\remotes\origin\master" -Raw
$srvHash = Get-Content "$base\server\.git\refs\heads\master" -Raw
Check "tracking ref matches server" ($trackHash.Trim() -eq $srvHash.Trim())
$cf = (& $mgit cat-file -p $srvHash.Trim() | Out-String)
Check "c3 objects fetched" ($cf -match "c3 from clone_b")

Write-Host "=== 8. non-fast-forward push rejected ==="
Set-Content "src\main.c" "int main(){return 2;}" -NoNewline
& $mgit add . | Out-Null
& $mgit commit -m "divergent commit" | Out-Null
$pout3 = (& $mgit push origin master 2>&1 | Out-String)
Write-Host $pout3
Check "push rejected (non-fast-forward)" ($pout3 -match "non-fast-forward")

Pop-Location

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $base '*') /s /d 2>$null
Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Result: $pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
exit 0
