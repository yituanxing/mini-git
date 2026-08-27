# Test: line-level three-way merge (ASCII only)
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

$lm = Join-Path $PSScriptRoot ("lm_x1_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $lm) { Remove-Item $lm -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $lm | Out-Null

# base file with 6 lines
$base = "line1`nline2`nline3`nline4`nline5`nline6`n"

$r = Run $lm @('init')
[System.IO.File]::WriteAllText((Join-Path $lm 'f.txt'), $base)
$r = Run $lm @('add','.'); $r = Run $lm @('commit','-m','base')

# scenario 1: disjoint changes auto-merge
$r = Run $lm @('branch','feat')
$r = Run $lm @('checkout','feat')
[System.IO.File]::WriteAllText((Join-Path $lm 'f.txt'), "line1`nline2-FEAT`nline3`nline4`nline5`nline6`n")
$r = Run $lm @('add','.'); $r = Run $lm @('commit','-m','change line2')
$r = Run $lm @('checkout','master')
[System.IO.File]::WriteAllText((Join-Path $lm 'f.txt'), "line1`nline2`nline3`nline4`nline5-MASTER`nline6`n")
$r = Run $lm @('add','.'); $r = Run $lm @('commit','-m','change line5')
$r = Run $lm @('merge','feat')
$content = [System.IO.File]::ReadAllText((Join-Path $lm 'f.txt'))
Check '1. disjoint changes auto-merged' ($r.Code -eq 0)
Check '2. both edits present' (($content -match 'line2-FEAT') -and ($content -match 'line5-MASTER'))
Check '3. no conflict markers' (-not ($content -match '<<<<<<<'))

# scenario 2: identical change
$lm2 = Join-Path $PSScriptRoot ("lm_x2_" + (Get-Date -Format 'HHmmss'))
New-Item -ItemType Directory $lm2 | Out-Null
$r = Run $lm2 @('init')
[System.IO.File]::WriteAllText((Join-Path $lm2 'f.txt'), $base)
$r = Run $lm2 @('add','.'); $r = Run $lm2 @('commit','-m','base')
$r = Run $lm2 @('branch','feat')
$r = Run $lm2 @('checkout','feat')
[System.IO.File]::WriteAllText((Join-Path $lm2 'f.txt'), "line1`nline2`nline3-SAME`nline4`nline5`nline6`n")
$r = Run $lm2 @('add','.'); $r = Run $lm2 @('commit','-m','same change')
$r = Run $lm2 @('checkout','master')
[System.IO.File]::WriteAllText((Join-Path $lm2 'f.txt'), "line1`nline2`nline3-SAME`nline4`nline5`nline6`n")
$r = Run $lm2 @('add','.'); $r = Run $lm2 @('commit','-m','same change too')
$r = Run $lm2 @('merge','feat')
$content = [System.IO.File]::ReadAllText((Join-Path $lm2 'f.txt'))
Check '4. identical changes merge clean' (-not ($content -match '<<<<<<<'))
Check '5. single copy of change' (([regex]::Matches($content, 'line3-SAME')).Count -eq 1)

# scenario 3: true conflict on same line, other region still auto-merged
$lm3 = Join-Path $PSScriptRoot ("lm_x3_" + (Get-Date -Format 'HHmmss'))
New-Item -ItemType Directory $lm3 | Out-Null
$r = Run $lm3 @('init')
[System.IO.File]::WriteAllText((Join-Path $lm3 'f.txt'), $base)
$r = Run $lm3 @('add','.'); $r = Run $lm3 @('commit','-m','base')
$r = Run $lm3 @('branch','feat')
$r = Run $lm3 @('checkout','feat')
[System.IO.File]::WriteAllText((Join-Path $lm3 'f.txt'), "line1`nline2-FEAT`nline3`nline4`nline5`nline6-FEAT`n")
$r = Run $lm3 @('add','.'); $r = Run $lm3 @('commit','-m','feat: line2 + line6')
$r = Run $lm3 @('checkout','master')
[System.IO.File]::WriteAllText((Join-Path $lm3 'f.txt'), "line1`nline2-MASTER`nline3`nline4`nline5`nline6`n")
$r = Run $lm3 @('add','.'); $r = Run $lm3 @('commit','-m','master: line2')
$r = Run $lm3 @('merge','feat')
$content = [System.IO.File]::ReadAllText((Join-Path $lm3 'f.txt'))
Check '6. conflict detected' ($r.Out -match 'CONFLICT')
Check '7. markers present' (($content -match '<<<<<<<') -and ($content -match '>>>>>>>'))
Check '8. both versions in conflict' (($content -match 'line2-FEAT') -and ($content -match 'line2-MASTER'))
Check '9. non-conflict region auto-merged' ($content -match 'line6-FEAT')

# scenario 4: pure insertion vs distant edit
$lm4 = Join-Path $PSScriptRoot ("lm_x4_" + (Get-Date -Format 'HHmmss'))
New-Item -ItemType Directory $lm4 | Out-Null
$r = Run $lm4 @('init')
[System.IO.File]::WriteAllText((Join-Path $lm4 'f.txt'), $base)
$r = Run $lm4 @('add','.'); $r = Run $lm4 @('commit','-m','base')
$r = Run $lm4 @('branch','feat')
$r = Run $lm4 @('checkout','feat')
[System.IO.File]::WriteAllText((Join-Path $lm4 'f.txt'), "line1`nline2`nline3`nINSERTED`nline4`nline5`nline6`n")
$r = Run $lm4 @('add','.'); $r = Run $lm4 @('commit','-m','insert line')
$r = Run $lm4 @('checkout','master')
[System.IO.File]::WriteAllText((Join-Path $lm4 'f.txt'), "line1-MASTER`nline2`nline3`nline4`nline5`nline6`n")
$r = Run $lm4 @('add','.'); $r = Run $lm4 @('commit','-m','edit line1')
$r = Run $lm4 @('merge','feat')
$content = [System.IO.File]::ReadAllText((Join-Path $lm4 'f.txt'))
Check '10. insertion merged clean' (-not ($content -match '<<<<<<<'))
Check '11. both changes kept' (($content -match 'line1-MASTER') -and ($content -match 'INSERTED'))

# scenario 5: cherry-pick with line-level merge
$r = Run $lm @('checkout','feat')
[System.IO.File]::WriteAllText((Join-Path $lm 'g.txt'), "g1`ng2`ng3`n")
$r = Run $lm @('add','.'); $r = Run $lm @('commit','-m','add g')
$r = Run $lm @('branch','other')
$r = Run $lm @('checkout','other')
[System.IO.File]::WriteAllText((Join-Path $lm 'g.txt'), "g1`ng2-OTHER`ng3`n")
$r = Run $lm @('add','.'); $r = Run $lm @('commit','-m','edit g2')
$r = Run $lm @('checkout','feat')
[System.IO.File]::WriteAllText((Join-Path $lm 'g.txt'), "g1`ng2`ng3-FEAT`n")
$r = Run $lm @('add','.'); $r = Run $lm @('commit','-m','edit g3')
$r = Run $lm @('checkout','other')
$r = Run $lm @('cherry-pick','feat')
$content = [System.IO.File]::ReadAllText((Join-Path $lm 'g.txt'))
Check '12. cherry-pick line merge ok' ($r.Code -eq 0)
Check '13. both g edits kept' (($content -match 'g2-OTHER') -and ($content -match 'g3-FEAT'))

# ---------- cleanup (.git objects are read-only: strip flag first) ----------
foreach ($t in @($lm, $lm2, $lm3, $lm4)) {
    if ($t) { attrib -r (Join-Path $t '*') /s /d 2>$null }
}
Remove-Item $lm, $lm2, $lm3, $lm4 -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
