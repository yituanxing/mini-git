# One-time cleanup of leftover test directories under tests/.
# MUST run from an ELEVATED (admin) PowerShell, sandbox/normal shells are denied:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\cleanup_tmp.ps1
# .git objects carry restrictive DACLs: takeown + icacls first, then delete.
# If the first run reports leftovers, run it once more (ACL repair is async).
$ErrorActionPreference = 'Continue'
$testsDir = $PSScriptRoot
$pattern = '^(tmp_|pull_t_|netclone_t_|netpush_t_|netfetch_t_|fetch_t_|args_t_|corrupt_t_|gaps_t_|cp_t2_|rb_t2_|ig_t1_|lm_x\d_|rr_t_|st_t_|test_repo_|deep_t_|catf_)|^work$'
$dirs = Get-ChildItem $testsDir -Directory | Where-Object { $_.Name -match $pattern }
Write-Host "found $($dirs.Count) leftover directories"

foreach ($d in $dirs) {
    $p = $d.FullName
    attrib -r (Join-Path $p '*') /s /d 2>$null
    takeown /r /d y /f $p 2>$null | Out-Null
    icacls $p /grant ($env:USERNAME + ':(OI)(CI)F') /t /q 2>$null | Out-Null
    Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $p) { Write-Host "  still exists: $($d.Name)" }
}

$left = Get-ChildItem $testsDir -Directory | Where-Object { $_.Name -match $pattern }
Write-Host "remaining: $($left.Count)"
if ($left.Count -gt 0) {
    Write-Host 'hint: rerun this script from an elevated (admin) PowerShell'
    exit 1
}
Write-Host 'cleanup done'
