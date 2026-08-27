# mini-git ?????????
# ????????Git ???????

$ErrorActionPreference = "Continue"
$ROOT = Split-Path -Parent $PSScriptRoot
$MGIT = Join-Path $ROOT "build\mgit.exe"
$TEST_DIR = Join-Path $PSScriptRoot ("test_repo_" + (Get-Date -Format 'HHmmss'))

function Cleanup {
    if (Test-Path $TEST_DIR) {
        # .git objects are read-only: strip flag before deleting
        attrib -r (Join-Path $TEST_DIR '*') /s /d 2>$null
        Remove-Item -Recurse -Force $TEST_DIR -ErrorAction SilentlyContinue
    }
}

function Test-Init {
    Write-Host "`n=== Test: init ===" -ForegroundColor Cyan
    Cleanup
    New-Item -ItemType Directory -Force -Path $TEST_DIR | Out-Null
    
    Push-Location $TEST_DIR
    & $MGIT init
    $result = Test-Path ".git/objects"
    Pop-Location
    
    if ($result) {
        Write-Host "PASS: init created .git directory" -ForegroundColor Green
    } else {
        Write-Host "FAIL: init did not create .git directory" -ForegroundColor Red
    }
}

function Test-HashObject {
    Write-Host "`n=== Test: hash-object ===" -ForegroundColor Cyan
    Push-Location $TEST_DIR
    
    # ?????????
    "hello world" | Out-File -FilePath "test.txt" -Encoding ascii -NoNewline
    
    # ??mgit ??????
    $mgit_hash = (& $MGIT hash-object "test.txt").Trim()
    Write-Host "mgit hash: $mgit_hash"
    
    # ?????git ???????????????
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $git_hash = (git hash-object "test.txt").Trim()
        Write-Host "git hash:  $git_hash"
        
        if ($mgit_hash -eq $git_hash) {
            Write-Host "PASS: hash matches git" -ForegroundColor Green
        } else {
            Write-Host "FAIL: hash does not match git" -ForegroundColor Red
        }
    } else {
        Write-Host "SKIP: git not available" -ForegroundColor Yellow
    }
    
    Pop-Location
}

function Test-HashObjectWrite {
    Write-Host "`n=== Test: hash-object -w ===" -ForegroundColor Cyan
    Push-Location $TEST_DIR
    
    # ??????
    $hash = (& $MGIT hash-object -w "test.txt").Trim()
    Write-Host "Written object: $hash"
    
    # ????????????????
    $obj_path = ".git/objects/" + $hash.Substring(0, 2) + "/" + $hash.Substring(2)
    if (Test-Path $obj_path) {
        Write-Host "PASS: object file created" -ForegroundColor Green
    } else {
        Write-Host "FAIL: object file not found at $obj_path" -ForegroundColor Red
    }
    
    # ??cat-file ???
    $content = (& $MGIT cat-file $hash)
    Write-Host "Read back: $content"
    
    if ($content -eq "hello world") {
        Write-Host "PASS: cat-file read correct content" -ForegroundColor Green
    } else {
        Write-Host "FAIL: cat-file read wrong content" -ForegroundColor Red
    }
    
    Pop-Location
}

function Test-GitCompatibility {
    Write-Host "`n=== Test: Git compatibility ===" -ForegroundColor Cyan
    
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Host "SKIP: git not available" -ForegroundColor Yellow
        return
    }
    
    Push-Location $TEST_DIR
    
    # ?????git ??????
    "git written content" | Out-File -FilePath "git_file.txt" -Encoding ascii -NoNewline
    $git_hash = (git hash-object -w "git_file.txt").Trim()
    Write-Host "Git wrote object: $git_hash"
    
    # ??mgit ???
    $content = (& $MGIT cat-file $git_hash)
    Write-Host "mgit read: $content"
    
    if ($content -eq "git written content") {
        Write-Host "PASS: mgit can read git objects" -ForegroundColor Green
    } else {
        Write-Host "FAIL: mgit cannot read git objects" -ForegroundColor Red
    }
    
    Pop-Location
}

function Test-CatFile {
    Write-Host "`n=== Test: cat-file options ===" -ForegroundColor Cyan
    Push-Location $TEST_DIR
    
    $hash = (& $MGIT hash-object "test.txt").Trim()
    
    # ??? -t (???)
    $type = (& $MGIT cat-file -t $hash).Trim()
    if ($type -eq "blob") {
        Write-Host "PASS: cat-file -t returns 'blob'" -ForegroundColor Green
    } else {
        Write-Host "FAIL: cat-file -t returned '$type'" -ForegroundColor Red
    }
    
    # ??? -s (???)
    $size = (& $MGIT cat-file -s $hash).Trim()
    if ($size -eq "11") {
        Write-Host "PASS: cat-file -s returns correct size" -ForegroundColor Green
    } else {
        Write-Host "FAIL: cat-file -s returned '$size'" -ForegroundColor Red
    }
    
    Pop-Location
}

# ????????
Write-Host "========================================" -ForegroundColor White
Write-Host "mini-git Test Suite" -ForegroundColor White
Write-Host "========================================" -ForegroundColor White

# ????mgit ??????
if (-not (Test-Path $MGIT)) {
    Write-Host "ERROR: mgit not found. Please run 'make' first." -ForegroundColor Red
    exit 1
}

Test-Init
Test-HashObject
Test-HashObjectWrite
Test-CatFile
Test-GitCompatibility

Write-Host "`n========================================" -ForegroundColor White
Write-Host "Tests completed" -ForegroundColor White
Write-Host "========================================" -ForegroundColor White

Cleanup
