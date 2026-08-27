# Test: network clone over Git Smart HTTP (ASCII only)
# Spins up a local git http-backend CGI server (tests/git_http_server.py),
# then verifies `mgit clone http://...` end to end:
# advertisement parsing, upload-pack request, side-band pack reception,
# unpack to loose objects, refs/HEAD landing and worktree checkout.
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$mgit = Join-Path $root 'build\mgit.exe'
$server = Join-Path $PSScriptRoot 'git_http_server.py'
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

function RunGit($dir, [string[]]$args2) {
    Push-Location $dir
    $out = & git @args2 2>&1 | Out-String
    $code = $LASTEXITCODE
    Pop-Location
    return @{ Out = $out; Code = $code }
}

$base = Join-Path $PSScriptRoot ("netclone_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $base) { Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $base | Out-Null
$port = 8899
$srvProc = $null

try {
    # ---------- setup: source repo built with real git ----------
    $src = Join-Path $base 'src'
    New-Item -ItemType Directory $src | Out-Null
    $r = RunGit $src @('init', '-q', '-b', 'master')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '--allow-empty', '-m', 'c1')
    Set-Content (Join-Path $src 'a.txt') "v1`nshared content" -NoNewline
    Set-Content (Join-Path $src 'b.txt') 'other' -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-m', 'c2')
    Set-Content (Join-Path $src 'a.txt') "v2`nshared content" -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-m', 'c3')
    $r = RunGit $src @('tag', 'v1.0')
    $r = RunGit $src @('checkout', '-q', '-b', 'feature')
    Set-Content (Join-Path $src 'f.txt') 'feature-file' -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-m', 'c4-feature')
    $r = RunGit $src @('checkout', '-q', 'master')

    # bare repo served by http-backend
    $repos = Join-Path $base 'repos'
    New-Item -ItemType Directory $repos | Out-Null
    $r = RunGit $base @('clone', '-q', '--bare', $src,
                        (Join-Path $repos 'srv.git'))
    Check 'setup: bare repo created' ($r.Code -eq 0)

    # ---------- start local Smart HTTP server ----------
    $srvProc = Start-Process python -ArgumentList @($server, $repos, $port) `
        -PassThru -WindowStyle Hidden
    $up = $false
    foreach ($i in 1..40) {
        Start-Sleep -Milliseconds 250
        try {
            $c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $port)
            $c.Close(); $up = $true; break
        } catch { }
    }
    Check 'setup: http server reachable' $up
    if (-not $up) { throw 'server never came up' }

    # ---------- clone over http ----------
    $r = Run $base @('clone', "http://127.0.0.1:$port/srv.git", 'cl')
    Check '1. clone exits ok' ($r.Code -eq 0)
    $cl = Join-Path $base 'cl'
    Check '2. clone dir created' (Test-Path (Join-Path $cl '.git'))

    Check '3. HEAD points at master' `
        ((Get-Content (Join-Path $cl '.git\HEAD') -Raw).Trim() -eq 'ref: refs/heads/master')
    Check '4. local branch master landed' `
        (Test-Path (Join-Path $cl '.git\refs\heads\master'))
    Check '5. tracking refs landed' `
        ((Test-Path (Join-Path $cl '.git\refs\remotes\origin\master')) -and
         (Test-Path (Join-Path $cl '.git\refs\remotes\origin\feature')))
    Check '6. tag landed' (Test-Path (Join-Path $cl '.git\refs\tags\v1.0'))

    Check '7. worktree file a.txt' `
        ((Get-Content (Join-Path $cl 'a.txt') -Raw) -eq "v2`nshared content")
    Check '8. worktree file b.txt' `
        ((Get-Content (Join-Path $cl 'b.txt') -Raw) -eq 'other')

    $lg = (Run $cl @('log', '--oneline')).Out
    Check '9. log shows all master commits' (($lg -match 'c3') -and ($lg -match 'c1'))
    $st = (Run $cl @('status')).Out
    Check '10. status clean after clone' ($st -match 'working tree clean')
    $rm = (Run $cl @('remote', '-v')).Out
    Check '11. remote origin recorded' ($rm -match "http://127.0.0.1:$port/srv.git")

    # real git can read the cloned object database (index aside)
    Rename-Item (Join-Path $cl '.git\index') 'index.bak' -ErrorAction SilentlyContinue
    $r = RunGit $cl @('fsck')
    Check '12. git fsck passes on clone' ($r.Code -eq 0)
    Move-Item (Join-Path $cl '.git\index.bak') (Join-Path $cl '.git\index') `
        -ErrorAction SilentlyContinue

    # clone via URL with .git suffix into explicit directory
    $r = Run $base @('clone', "http://127.0.0.1:$port/srv.git", 'cl2')
    Check '13. second clone exits ok' ($r.Code -eq 0)
    Check '14. second clone has files' (Test-Path (Join-Path $base 'cl2\a.txt'))

    # destination exists -> refuse
    $r = Run $base @('clone', "http://127.0.0.1:$port/srv.git", 'cl2')
    Check '15. clone into existing dir fails' ($r.Code -ne 0)
} finally {
    if ($srvProc) {
        Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue
    }
    attrib -r (Join-Path $base '*') /s /d 2>$null
    Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ""
Write-Host "PASS: $pass  FAIL: $fail"
if ($fail -gt 0) { exit 1 } else { exit 0 }
