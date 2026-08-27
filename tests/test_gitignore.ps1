# Test: .gitignore support (ASCII only)
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

$d = Join-Path $PSScriptRoot ("ig_t1_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $d) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $d | Out-Null

$r = Run $d @('init')

# .gitignore content
$gi = "*.o`nbuild/`n*.log`n!important.log`n/root.txt`ntmp?.dat`n"
[System.IO.File]::WriteAllText((Join-Path $d '.gitignore'), $gi)

# files
Set-Content (Join-Path $d 'a.o') 'obj'
Set-Content (Join-Path $d 'b.c') 'src'
Set-Content (Join-Path $d 'debug.log') 'log'
Set-Content (Join-Path $d 'important.log') 'keep'
Set-Content (Join-Path $d 'root.txt') 'root-only'
Set-Content (Join-Path $d 'tmp1.dat') 'tmp'
New-Item -ItemType Directory (Join-Path $d 'build') | Out-Null
Set-Content (Join-Path $d 'build\out.bin') 'bin'
New-Item -ItemType Directory (Join-Path $d 'sub') | Out-Null
Set-Content (Join-Path $d 'sub\root.txt') 'sub-root'
Set-Content (Join-Path $d 'sub\c.c') 'c'

$r = Run $d @('add','.')
Check '1. add . succeeds' ($r.Code -eq 0)
Check '2. a.o ignored' (-not ($r.Out -match "add 'a\.o'"))
Check '3. b.c added' ($r.Out -match "add 'b\.c'")
Check '4. build/ ignored' (-not ($r.Out -match 'out\.bin'))
Check '5. debug.log ignored' (-not ($r.Out -match 'debug\.log'))
Check '6. important.log negated (added)' ($r.Out -match 'important\.log')
Check '7. /root.txt ignored at root' (-not ($r.Out -match "add 'root\.txt'"))
Check '8. sub/root.txt NOT ignored (anchored)' ($r.Out -match 'sub/root\.txt')
Check '9. tmp1.dat matches tmp?.dat' (-not ($r.Out -match 'tmp1\.dat'))
Check '10. .gitignore itself tracked' ($r.Out -match '\.gitignore')

# status should not list ignored files as untracked
Set-Content (Join-Path $d 'c2.o') 'obj2'
$r = Run $d @('status')
Check '11. status hides ignored files' (-not ($r.Out -match 'c2\.o'))
Check '12. status hides build dir' (-not ($r.Out -match 'build/'))

# explicit add of ignored file is skipped
$r = Run $d @('add','a.o')
Check '13. explicit add skipped' ($r.Out -match 'skipped ignored')

# commit works and tree has no ignored files
$r = Run $d @('commit','-m','with gitignore')
$r = Run $d @('ls-tree','HEAD')
Check '14. commit ok' ($r.Code -eq 0)
Check '15. tree has no a.o' (-not ($r.Out -match 'a\.o'))
Check '16. tree has important.log' ($r.Out -match 'important\.log')

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
attrib -r (Join-Path $d '*') /s /d 2>$null
Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
