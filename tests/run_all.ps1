# Run every regression test suite
$ErrorActionPreference = 'Continue'
$scripts = @(
    'test_basic.ps1',
    'test_compat.ps1',
    'test_gitignore.ps1',
    'test_cherry_rebase.ps1',
    'test_linemerge.ps1',
    'test_subdir.ps1',
    'test_remote.ps1',
    'test_gc_pack.ps1',
    'test_reset_revert.ps1',
    'test_stash_reflog.ps1',
    'test_args.ps1',
    'test_gaps.ps1',
    'test_corrupt.ps1',
    'test_pull.ps1',
    'test_netclone.ps1',
    'test_netpush.ps1',
    'test_netfetch.ps1'
)
$failed = @()
foreach ($s in $scripts) {
    Write-Host ""
    Write-Host "########## $s ##########" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot $s)
    if ($LASTEXITCODE -ne 0) { $failed += $s }
}
Write-Host ""
if ($failed.Count -eq 0) {
    Write-Host "ALL TESTS PASSED" -ForegroundColor Green
} else {
    Write-Host ("FAILED: " + ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
exit 0
