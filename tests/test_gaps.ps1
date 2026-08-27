# Test: three previously-recorded gaps (ASCII only)
# 1. add -A stages deletions of tracked files
# 2. tag refs resolve by name (loose refs/tags/ prefix + packed-refs tags)
# 3. clone works on a gc-packed repository
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

$base = Join-Path $PSScriptRoot ("gaps_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $base) { Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $base | Out-Null

# ---------- 1. add -A stages deletions ----------
$d1 = Join-Path $base 'del'
New-Item -ItemType Directory $d1 | Out-Null
$r = Run $d1 @('init')
Set-Content (Join-Path $d1 'keep.txt') 'stay' -NoNewline
Set-Content (Join-Path $d1 'gone.txt') 'bye' -NoNewline
$r = Run $d1 @('add','.'); $r = Run $d1 @('commit','-m','c1')

Remove-Item (Join-Path $d1 'gone.txt')
$r = Run $d1 @('add','-A')
Check '1. add -A reports deletion staged' ($r.Out -match "remove 'gone\.txt' \(deleted\)")
$dc = (Run $d1 @('diff','--cached')).Out
Check '2. diff --cached shows gone.txt deleted' ($dc -match 'gone\.txt \(deleted\)')
$r = Run $d1 @('commit','-m','c2-remove-gone')
Check '3. commit with deletion ok' ($r.Code -eq 0)
$lt = (Run $d1 @('ls-tree','HEAD')).Out
Check '4. HEAD tree no longer lists gone.txt' (-not ($lt -match 'gone\.txt'))
Check '5. HEAD tree still lists keep.txt' ($lt -match 'keep\.txt')

# add . must NOT stage deletions (unchanged semantics)
Set-Content (Join-Path $d1 'keep.txt') 'stay2' -NoNewline
Remove-Item (Join-Path $d1 'keep.txt')
$r = Run $d1 @('add','.')
Check '6. add . does not stage deletions' (-not ($r.Out -match 'remove'))
# restore for cleanliness
Set-Content (Join-Path $d1 'keep.txt') 'stay' -NoNewline
$r = Run $d1 @('add','.'); $r = Run $d1 @('commit','-m','c3')

# ---------- 2. resolve tag by name ----------
$d2 = Join-Path $base 'tag'
New-Item -ItemType Directory $d2 | Out-Null
$r = Run $d2 @('init')
Set-Content (Join-Path $d2 'f.txt') 'one' -NoNewline
$r = Run $d2 @('add','.'); $r = Run $d2 @('commit','-m','tagged-commit')
$r = Run $d2 @('tag','v9')
$lg = Run $d2 @('log','--oneline','v9')
Check '7. log resolves loose tag by name' (($lg.Code -eq 0) -and ($lg.Out -match 'tagged-commit'))

# packed-refs containing a tag (simulate git gc output)
$full = (Run $d2 @('log')).Out
$h = $null
if ($full -match '(?m)^commit ([0-9a-f]{40})') { $h = $Matches[1] }
Check '7b. setup: commit hash extracted' ($null -ne $h)
Remove-Item (Join-Path $d2 '.git\refs\tags\v9') -ErrorAction SilentlyContinue
Set-Content (Join-Path $d2 '.git\packed-refs') "# pack-refs with: peeled fully-peeled sorted`n$h refs/tags/v9`n" -NoNewline
$lg2 = Run $d2 @('log','--oneline','v9')
Check '8. log resolves packed tag by name' (($lg2.Code -eq 0) -and ($lg2.Out -match 'tagged-commit'))
$lt2 = Run $d2 @('ls-tree','v9')
Check '9. ls-tree resolves packed tag' (($lt2.Code -eq 0) -and ($lt2.Out -match 'f\.txt'))

# ---------- 3. clone a gc-packed repository ----------
$d3 = Join-Path $base 'src'
New-Item -ItemType Directory $d3 | Out-Null
$r = Run $d3 @('init')
Set-Content (Join-Path $d3 'a.txt') 'v1' -NoNewline
New-Item -ItemType Directory (Join-Path $d3 'sub') | Out-Null
Set-Content (Join-Path $d3 'sub\b.txt') 'nested' -NoNewline
$r = Run $d3 @('add','.'); $r = Run $d3 @('commit','-m','p1')
Set-Content (Join-Path $d3 'a.txt') 'v2' -NoNewline
$r = Run $d3 @('add','.'); $r = Run $d3 @('commit','-m','p2')
$r = Run $d3 @('tag','release')
$r = Run $d3 @('gc')
Check '10. gc ran ok' ($r.Code -eq 0)

$r = Run $base @('clone','src','cloned')
Check '11. clone of packed repo exits ok' ($r.Code -eq 0)
$d4 = Join-Path $base 'cloned'
Check '12. cloned worktree file exists' (Test-Path (Join-Path $d4 'a.txt'))
Check '13. cloned subdir file exists' (Test-Path (Join-Path $d4 'sub\b.txt'))
Check '14. cloned content correct' ((Get-Content (Join-Path $d4 'a.txt') -Raw) -eq 'v2')
$lgc = (Run $d4 @('log','--oneline')).Out
Check '15. clone log reads objects from pack' (($lgc -match 'p1') -and ($lgc -match 'p2'))

# clone_b can commit and diff normally (pack objects readable)
Set-Content (Join-Path $d4 'a.txt') 'v3' -NoNewline
$r = Run $d4 @('add','.'); $r = Run $d4 @('commit','-m','p3-on-clone')
Check '16. commit on packed-clone ok' ($r.Code -eq 0)

# ---------- cleanup ----------
attrib -r (Join-Path $base '*') /s /d 2>$null
Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 }
exit 0
