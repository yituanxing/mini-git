# Test: network fetch/pull over Git Smart HTTP (ASCII only)
# Spins up a local git http-backend CGI server (tests/git_http_server.py),
# then verifies `mgit fetch` / `mgit pull` against a http(s) remote:
# ref advertisement diffing, want/have negotiation (incremental pack),
# remote-tracking ref updates, FETCH_HEAD, fast-forward and diverged
# merge pulls, tag fetching, and real-git readability of the result.
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

$base = Join-Path $PSScriptRoot ("netfetch_t_" + (Get-Date -Format 'HHmmss'))
if (Test-Path $base) { Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory $base | Out-Null
$port = 8895
$srvProc = $null

try {
    # ---------- setup: source repo built with real git ----------
    $src = Join-Path $base 'src'
    New-Item -ItemType Directory $src | Out-Null
    $r = RunGit $src @('init', '-q', '-b', 'master')
    Set-Content (Join-Path $src 'a.txt') 'v1' -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-q', '-m', 'c1')
    $r = RunGit $src @('tag', 'v1')

    # bare repo served by http-backend
    $repos = Join-Path $base 'repos'
    New-Item -ItemType Directory $repos | Out-Null
    $r = RunGit $base @('clone', '-q', '--bare', $src,
                        (Join-Path $repos 'srv.git'))
    Check 'setup: bare repo created' ($r.Code -eq 0)
    $r = RunGit (Join-Path $repos 'srv.git') @('config', 'http.receivepack', 'true')

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

    # ---------- clone, then a new commit appears upstream ----------
    $r = Run $base @('clone', "http://127.0.0.1:$port/srv.git", 'work')
    Check '1. clone exits ok' ($r.Code -eq 0)
    $work = Join-Path $base 'work'

    Set-Content (Join-Path $src 'a.txt') 'v2' -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-q', '-m', 'c2')
    # push via local path (sandbox may block git network children)
    $r = RunGit $src @('push', '-q', (Join-Path $repos 'srv.git'), 'master')
    Check '2. setup: upstream advanced' ($r.Code -eq 0)

    # ---------- fetch: incremental pull of the delta ----------
    $r = Run $work @('fetch')
    Check '3. fetch exits ok' ($r.Code -eq 0)
    Check '4. fetch shows old..new update' ($r.Out -match '\.\..*master\s+->\s+origin/master')
    Check '5. fetch did not touch worktree' ((Get-Content (Join-Path $work 'a.txt') -Raw) -eq 'v1')
    Check '6. FETCH_HEAD written' (Test-Path (Join-Path $work '.git\FETCH_HEAD'))
    $srvHead = (RunGit (Join-Path $repos 'srv.git') @('rev-parse', 'master')).Out.Trim()
    $trackFile = Join-Path $work '.git\refs\remotes\origin\master'
    Check '7. tracking ref matches upstream' ((Test-Path $trackFile) -and ((Get-Content $trackFile -Raw).Trim() -eq $srvHead))

    # ---------- pull: fast-forward merge ----------
    $r = Run $work @('pull')
    Check '8. pull exits ok' ($r.Code -eq 0)
    Check '9. pull fast-forwarded' ($r.Out -match 'Fast-forward')
    Check '10. worktree updated' ((Get-Content (Join-Path $work 'a.txt') -Raw) -eq 'v2')

    # ---------- diverged pull: merge commit ----------
    Set-Content (Join-Path $src 'c.txt') 'from-src' -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-q', '-m', 'c3-src')
    $r = RunGit $src @('push', '-q', (Join-Path $repos 'srv.git'), 'master')

    Set-Content (Join-Path $work 'd.txt') 'from-work' -NoNewline
    $r = Run $work @('add', 'd.txt')
    $r = Run $work @('commit', '-m', 'c4-work')
    Check '11. local divergent commit' ($r.Code -eq 0)

    $r = Run $work @('pull')
    Check '12. diverged pull exits ok' ($r.Code -eq 0)
    Check '13. merge commit created' ($r.Out -match 'Merge')
    Check '14. both files present' ((Test-Path (Join-Path $work 'c.txt')) -and (Test-Path (Join-Path $work 'd.txt')))

    # ---------- tag fetch ----------
    $r = RunGit $src @('tag', 'v2')
    $r = RunGit $src @('push', '-q', (Join-Path $repos 'srv.git'), 'v2')
    $r = Run $work @('fetch')
    Check '15. tag fetch exits ok' ($r.Code -eq 0)
    Check '16. new tag reported' ($r.Out -match '\[new tag\].*v2')
    Check '17. tag ref stored' (Test-Path (Join-Path $work '.git\refs\tags\v2'))

    # ---------- up-to-date fetch ----------
    $r = Run $work @('fetch')
    Check '18. repeated fetch exits ok' ($r.Code -eq 0)

    # ---------- new branch appears upstream ----------
    $r = RunGit $src @('checkout', '-q', '-b', 'feature')
    Set-Content (Join-Path $src 'f.txt') 'feature-work' -NoNewline
    $r = RunGit $src @('add', '.')
    $r = RunGit $src @('-c', 'user.email=t@t.com', '-c', 'user.name=t',
                       'commit', '-q', '-m', 'c5-feature')
    $r = RunGit $src @('push', '-q', (Join-Path $repos 'srv.git'), 'feature')
    $r = Run $work @('fetch')
    Check '19. new-branch fetch exits ok' ($r.Code -eq 0)
    Check '20. new branch reported' ($r.Out -match '\[new branch\].*feature')
    Check '21. new tracking ref stored' (Test-Path (Join-Path $work '.git\refs\remotes\origin\feature'))

    # ---------- tag on the new branch commit (objects already present) ----------
    $r = RunGit $src @('tag', 'v3')
    $r = RunGit $src @('push', '-q', (Join-Path $repos 'srv.git'), 'v3')
    $r = Run $work @('fetch')
    Check '22. tag-on-branch fetch exits ok' ($r.Code -eq 0)
    Check '23. tag v3 reported' ($r.Out -match '\[new tag\].*v3')

    # ---------- explicit remote argument ----------
    $r = Run $work @('fetch', 'origin')
    Check '24. fetch with explicit remote ok' ($r.Code -eq 0)

    # ---------- negative paths ----------
    $r = Run $work @('fetch', 'nosuchremote')
    Check '25. fetch unknown remote fails' ($r.Code -ne 0)

    $norepo = Join-Path $base 'empty_dir'
    New-Item -ItemType Directory $norepo | Out-Null
    $r = Run $norepo @('fetch')
    Check '26. fetch outside repo fails' ($r.Code -ne 0)

    $r = Run $work @('remote', 'add', 'dead', 'http://127.0.0.1:8800/x.git')
    $r = Run $work @('fetch', 'dead')
    Check '27. fetch unreachable server fails' ($r.Code -ne 0)

    # ---------- result readable by real git ----------
    $r = RunGit $work @('fsck')
    Check '28. real git fsck clean' ($r.Code -eq 0)
    $r = RunGit $work @('log', '--oneline')
    Check '29. real git sees merge history' ($r.Out -match 'c3-src' -and $r.Out -match 'c4-work')
} catch {
    Write-Host "[FAIL] exception: $_"
    $fail++
} finally {
    if ($srvProc) { Stop-Process -Id $srvProc.Id -Force -ErrorAction SilentlyContinue }
    if (Test-Path $base) {
        attrib -r (Join-Path $base '*') /s /d 2>$null
        Remove-Item $base -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "netfetch: $pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
exit 0
