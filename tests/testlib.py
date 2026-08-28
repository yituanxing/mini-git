"""Small cross-platform test helpers for mgit.

Standard library only.  The goal is to make the same repository-level tests
run locally and in CI on Windows and Linux.
"""

from __future__ import annotations

import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MGIT = ROOT / "build" / ("mgit.exe" if os.name == "nt" else "mgit")
GIT = shutil.which("git") or "git"


class TestFailure(AssertionError):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)
    print(f"[PASS] {message}")


def run(argv, cwd: Path | str | None = None, *, check_rc: bool = False,
        input_bytes: bytes | None = None) -> subprocess.CompletedProcess:
    p = subprocess.run(
        [str(x) for x in argv],
        cwd=str(cwd) if cwd is not None else None,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check_rc and p.returncode != 0:
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        raise TestFailure(
            f"command failed ({p.returncode}): {' '.join(map(str, argv))}\n"
            f"stdout:\n{out}\nstderr:\n{err}"
        )
    return p


def text(p: subprocess.CompletedProcess) -> str:
    return (p.stdout + p.stderr).decode("utf-8", "replace")


def out(p: subprocess.CompletedProcess) -> str:
    return p.stdout.decode("utf-8", "replace")


def mgit(cwd: Path | str, *args: str, check_rc: bool = True):
    p = run([MGIT, *args], cwd=cwd, check_rc=check_rc)
    # Real Git is our oracle. Disable platform newline conversion so oracle
    # operations do not mutate bytes behind mgit's back on Windows.
    if args and args[0] == "init" and p.returncode == 0:
        run([GIT, "config", "core.autocrlf", "false"], cwd=cwd, check_rc=True)
    return p


def git(cwd: Path | str, *args: str, check_rc: bool = True):
    return run([GIT, *args], cwd=cwd, check_rc=check_rc)


def write(path: Path, data: bytes | str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8", newline="")
    else:
        path.write_bytes(data)


def remove_tree(path: Path) -> None:
    if not path.exists():
        return

    def onerror(func, target, exc_info):
        try:
            os.chmod(target, stat.S_IWRITE | stat.S_IREAD)
            func(target)
        except OSError:
            raise exc_info[1]

    shutil.rmtree(path, onerror=onerror)


@contextmanager
def tempdir(prefix: str):
    path = Path(tempfile.mkdtemp(prefix=f"mgit-{prefix}-"))
    try:
        yield path
    finally:
        remove_tree(path)


def init_git_repo(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    git(path, "init", "-q", "-b", "master")
    git(path, "config", "core.autocrlf", "false")


def git_commit(path: Path, message: str) -> None:
    git(path, "-c", "user.name=Tester", "-c", "user.email=t@t.com",
        "commit", "-q", "-m", message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


class GitHttpServer:
    def __init__(self, repos_root: Path):
        self.repos_root = repos_root
        self.port = free_port()
        self.proc: subprocess.Popen | None = None

    @property
    def base_url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def __enter__(self):
        server = ROOT / "tests" / "git_http_server.py"
        self.proc = subprocess.Popen(
            [sys.executable, str(server), str(self.repos_root), str(self.port), GIT],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.time() + 8.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise TestFailure("Git Smart HTTP test server exited early")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.2):
                    return self
            except OSError:
                time.sleep(0.05)

        raise TestFailure("Git Smart HTTP test server did not become reachable")

    def __exit__(self, exc_type, exc, tb):
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=3)
