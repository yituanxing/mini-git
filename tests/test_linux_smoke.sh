#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
MGIT="$ROOT/build/mgit"
TMP="$(mktemp -d)"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

pass() {
    printf '[PASS] %s\n' "$1"
}

fail() {
    printf '[FAIL] %s\n' "$1" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# 1. Basic Git model + real Git compatibility.
# ---------------------------------------------------------------------------
REPO="$TMP/basic"
mkdir -p "$REPO"
cd "$REPO"

"$MGIT" init >/dev/null
printf 'hello linux\n' > hello.txt

MGIT_HASH="$("$MGIT" hash-object hello.txt | tr -d '\r\n')"
GIT_HASH="$(git hash-object hello.txt | tr -d '\r\n')"
[ "$MGIT_HASH" = "$GIT_HASH" ] || fail "hash-object matches real Git"
pass "hash-object matches real Git"

"$MGIT" add hello.txt >/dev/null
"$MGIT" commit -m "linux c1" >/dev/null
git fsck --full
[ "$(git log -1 --pretty=%s)" = "linux c1" ] || fail "real Git reads mgit commit"
pass "real Git reads mgit commit"

# Exercise POSIX recursive directory creation through tree/object writes.
mkdir -p src/deep
printf 'nested\n' > src/deep/a.txt
"$MGIT" add . >/dev/null
"$MGIT" commit -m "linux nested tree" >/dev/null
git fsck --full
git ls-tree -r --name-only HEAD | grep -qx 'src/deep/a.txt' ||
    fail "nested tree survives real Git"
pass "nested tree survives real Git"

# ---------------------------------------------------------------------------
# 2. Dogfood: Linux mgit manages a complete mgit source snapshot.
# ---------------------------------------------------------------------------
SELF="$TMP/selfhost"
mkdir -p "$SELF"
cp -a "$ROOT/." "$SELF/"
rm -rf "$SELF/.git" "$SELF/build"

cd "$SELF"
"$MGIT" init >/dev/null
"$MGIT" add . >/dev/null
"$MGIT" commit -m "linux: mgit self-host snapshot" >/dev/null
"$MGIT" status | grep -q 'working tree clean' ||
    fail "mgit sees self-hosted tree as clean"
git fsck --full
[ "$(git log -1 --pretty=%s)" = "linux: mgit self-host snapshot" ] ||
    fail "real Git reads self-hosted mgit repository"
[ -z "$(git status --porcelain)" ] ||
    fail "real Git sees self-hosted repository as clean"
pass "mgit self-host snapshot is valid to real Git"

# ---------------------------------------------------------------------------
# 3. Smart HTTP through the Linux libcurl backend.
# ---------------------------------------------------------------------------
SRC="$TMP/http-src"
REPOS="$TMP/http-repos"
mkdir -p "$SRC" "$REPOS"

git -C "$SRC" init -q -b master
printf 'served over smart http\n' > "$SRC/a.txt"
git -C "$SRC" add a.txt
git -C "$SRC" -c user.email=t@t.com -c user.name=t commit -q -m "server c1"
git clone -q --bare "$SRC" "$REPOS/srv.git"
git -C "$REPOS/srv.git" config http.receivepack true

PORT=18998
python3 "$ROOT/tests/git_http_server.py" "$REPOS" "$PORT"     >"$TMP/http-server.log" 2>&1 &
SERVER_PID=$!

python3 - "$PORT" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
for _ in range(80):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.1)
raise SystemExit("Smart HTTP test server did not start")
PY

cd "$TMP"
"$MGIT" clone "http://127.0.0.1:$PORT/srv.git" http-clone >/dev/null
[ "$(cat "$TMP/http-clone/a.txt")" = "served over smart http" ] ||
    fail "libcurl clone checks out expected content"
git -C "$TMP/http-clone" fsck --full
pass "libcurl Smart HTTP clone works"

printf 'pushed from linux mgit\n' > "$TMP/http-clone/pushed.txt"
cd "$TMP/http-clone"
"$MGIT" add pushed.txt >/dev/null
"$MGIT" commit -m "linux push" >/dev/null
"$MGIT" push >/dev/null

VERIFY="$TMP/http-verify"
git clone -q "http://127.0.0.1:$PORT/srv.git" "$VERIFY"
[ "$(cat "$VERIFY/pushed.txt")" = "pushed from linux mgit" ] ||
    fail "libcurl push is readable by real Git"
git -C "$REPOS/srv.git" fsck --full
pass "libcurl Smart HTTP push works"

printf '\nLINUX SMOKE PASSED\n'
