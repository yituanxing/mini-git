# Test: network push over Git Smart HTTP (ASCII only)
# Spins up a local git http-backend CGI server (tests/git_http_server.py),
# then verifies `mgit push` against a http(s) remote end to end:
# receive-pack advertisement, missing-object collection, in-memory pack
# build, report-status parsing, fast-forward check, first push to an
# empty repository, and real-git readability of pushed objects.
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

$base = Join-Path $PSScriptRoot ("netpush_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $base) { Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $base | Out-Null
$port = 8898
$srvProc = $null

try {
    # ---------- setup: source repo built with real git ----------
    $src = Join-Path $base 'src'
    New-Item -ItemType Directory $src | Out-Null
    $r = RunGit $src @('init', '-q', '-b', 'master')
    Set-Content (Join-Path $src 'a.txt') "v1`nshared" -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-m', 'c1')

    # bare repo served by http-backend (receive-pack must be enabled)
    $repos = Join-Path $base 'repos'
    New-Item -ItemType Directory $repos | Out-Null
    $r = RunGit $base @('clone', '-q', '--bare', $src,
                        (Join-Path $repos 'srv.git'))
    Check 'setup: bare repo created' ($r.Code -eq 0)
    $r = RunGit (Join-Path $repos 'srv.git') @('config', 'http.receivepack', 'true')

    # empty bare repo for the first-push scenario
    $r = RunGit $repos @('init', '-q', '--bare', 'empty.git')
    $r = RunGit (Join-Path $repos 'empty.git') @('config', 'http.receivepack', 'true')

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

    # ---------- clone, then push an update ----------
    $r = Run $base @('clone', "http://127.0.0.1:$port/srv.git", 'work')
    Check '1. clone exits ok' ($r.Code -eq 0)
    $work = Join-Path $base 'work'

    Set-Content (Join-Path $work 'b.txt') 'pushed-by-mgit' -NoNewline
    $r = Run $work @('add', 'b.txt')
    $r = Run $work @('commit', '-m', 'push1')
    Check '2. local commit created' ($r.Code -eq 0)

    $r = Run $work @('push')
    Check '3. first push exits ok' ($r.Code -eq 0)
    Check '4. push output shows master update' ($r.Out -match 'master -> master')

    # remote ref really moved?
    $ls = (RunGit $base @('ls-remote', "http://127.0.0.1:$port/srv.git")).Out
    $head = (RunGit $work @('rev-parse', 'HEAD')).Out.Trim()
    Check '5. remote master matches pushed hash' ($ls -match $head.Substring(0, 12))

    # real git can read what mgit pushed
    $r = RunGit $base @('clone', '-q', "http://127.0.0.1:$port/srv.git", 'v1')
    Check '6. real git clone of pushed repo' ($r.Code -eq 0)
    Check '7. pushed file content visible' `
        ((Get-Content (Join-Path $base 'v1\b.txt') -Raw) -eq 'pushed-by-mgit')

    # ---------- second push: existing branch update ----------
    Set-Content (Join-Path $work 'c.txt') 'second' -NoNewline
    $r = Run $work @('add', 'c.txt')
    $r = Run $work @('commit', '-m', 'push2')
    $r = Run $work @('push')
    Check '8. update push exits ok' ($r.Code -eq 0)
    Check '9. update push shows range' ($r.Out -match '\.\.')

    # ---------- non-fast-forward must be refused ----------
    # src 先同步到服务器最新（本地路径，避开沙箱对 git 网络子进程
    # 的偶发拦截），再在其上提交一个 mgit 不知道的提交并推上去，
    # 使 work 的 master 落后于远端 -> mgit push 必须拒绝
    $r = RunGit $src @('pull', '-q', (Join-Path $repos 'srv.git'), 'master')
    Check '10a. setup: src synced to server tip' ($r.Code -eq 0)
    Set-Content (Join-Path $src 'z.txt') 'diverge' -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-m', 'c2-diverge')
    $r = RunGit $src @('push', '-q', (Join-Path $repos 'srv.git'), 'master')
    Check '10. setup: divergent commit pushed by real git' ($r.Code -eq 0)

    $r = Run $work @('push')
    Check '11. non-fast-forward push fails' ($r.Code -ne 0)
    Check '12. failure mentions non-fast-forward' ($r.Out -match 'non-fast-forward')

    # ---------- new branch + everything up-to-date ----------
    $r = Run $work @('branch', 'dev')
    $r = Run $work @('push', 'origin', 'dev')
    Check '13. new branch push exits ok' ($r.Code -eq 0)
    Check '14. output shows new branch' ($r.Out -match 'new branch')

    $r = Run $work @('push', 'origin', 'dev')
    Check '15. repeat push is up-to-date' `
        (($r.Code -eq 0) -and ($r.Out -match 'up-to-date'))

    # ---------- first push into an empty repository ----------
    $r = Run $work @('remote', 'add', 'empty',
                     "http://127.0.0.1:$port/empty.git")
    $r = Run $work @('push', 'empty', 'dev')
    Check '16. push into empty repo exits ok' ($r.Code -eq 0)
    $ls = (RunGit $base @('ls-remote', "http://127.0.0.1:$port/empty.git")).Out
    Check '17. empty repo now has dev branch' ($ls -match 'refs/heads/dev')

    # pushed object database stays readable by real git
    $r = RunGit (Join-Path $repos 'srv.git') @('fsck')
    Check '18. git fsck passes on server repo' ($r.Code -eq 0)
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
