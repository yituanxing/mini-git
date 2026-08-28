#!/usr/bin/env python3
"""Cross-platform mgit integration tests.

Usage:
    python tests/run_all.py
    python tests/run_all.py basic compat
    python tests/run_all.py network dogfood

The suite intentionally stays small and explicit.  Legacy PowerShell tests
remain temporarily as a Windows regression safety net while cases migrate.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

from testlib import (
    GIT,
    MGIT,
    ROOT,
    GitHttpServer,
    TestFailure,
    check,
    git,
    git_commit,
    init_git_repo,
    mgit,
    out,
    run,
    tempdir,
    text,
    write,
)


def test_basic() -> None:
    print("\n== basic ==")
    with tempdir("basic") as d:
        mgit(d, "init")
        check((d / ".git" / "objects").is_dir(), "init creates object database")

        write(d / "hello.txt", b"hello world")
        mh = out(mgit(d, "hash-object", "hello.txt")).strip()
        gh = out(git(d, "hash-object", "hello.txt")).strip()
        check(mh == gh, "hash-object matches real Git")

        wh = out(mgit(d, "hash-object", "-w", "hello.txt")).strip()
        check(wh == mh, "hash-object -w returns same object id")
        check((d / ".git" / "objects" / wh[:2] / wh[2:]).is_file(),
              "hash-object -w stores loose object")

        check(out(mgit(d, "cat-file", "-t", wh)).strip() == "blob",
              "cat-file -t reports blob")
        check(out(mgit(d, "cat-file", "-s", wh)).strip() == "11",
              "cat-file -s reports exact size")
        check(out(mgit(d, "cat-file", wh)) == "hello world",
              "cat-file reads stored content")

        # Real Git writes; mgit reads.
        write(d / "from-git.txt", b"git written content")
        gh2 = out(git(d, "hash-object", "-w", "from-git.txt")).strip()
        check(out(mgit(d, "cat-file", gh2)) == "git written content",
              "mgit reads loose object written by real Git")


def test_compat() -> None:
    print("\n== compat ==")
    with tempdir("compat") as d:
        # Same bytes => same hash, including binary.
        a = d / "mgit-hash"
        b = d / "git-hash"
        a.mkdir(); b.mkdir()
        mgit(a, "init")
        init_git_repo(b)

        data = b"hello world\n"
        write(a / "t.txt", data)
        write(b / "t.txt", data)
        check(out(mgit(a, "hash-object", "t.txt")).strip() ==
              out(git(b, "hash-object", "t.txt")).strip(),
              "text blob hash matches real Git")

        binary = bytes([0, 1, 2, 3, 128, 255, 254, 253, 100, 200])
        write(a / "binary.bin", binary)
        write(b / "binary.bin", binary)
        check(out(mgit(a, "hash-object", "binary.bin")).strip() ==
              out(git(b, "hash-object", "binary.bin")).strip(),
              "binary blob hash matches real Git")

        # mgit reads a Git-authored repository.
        g = d / "git-authored"
        init_git_repo(g)
        write(g / "file1.txt", b"content from git\n")
        git(g, "add", "file1.txt")
        git_commit(g, "git commit")
        commit = out(git(g, "rev-parse", "HEAD")).strip()
        tree = out(git(g, "rev-parse", "HEAD^{tree}")).strip()
        blob = out(git(g, "hash-object", "file1.txt")).strip()

        lg = text(mgit(g, "log"))
        check(commit[:7] in lg and "Tester" in lg,
              "mgit parses Git-authored commit")
        check("file1.txt" in text(mgit(g, "ls-tree", tree)),
              "mgit parses Git-authored tree")
        check("content from git" in text(mgit(g, "cat-file", "-p", blob)),
              "mgit reads Git-authored blob")

        # Real Git reads an mgit-authored repository.
        m = d / "mgit-authored"
        m.mkdir()
        mgit(m, "init")
        write(m / "mgit_file.txt", b"content from mgit\n")
        mgit(m, "add", "mgit_file.txt")
        mgit(m, "commit", "-m", "mgit commit")
        mcommit = out(git(m, "rev-parse", "HEAD")).strip()

        cat = out(git(m, "cat-file", "-p", mcommit))
        check("tree " in cat and "mgit commit" in cat,
              "real Git parses mgit commit")
        check("mgit_file.txt" in out(git(m, "ls-tree", "HEAD^{tree}")),
              "real Git parses mgit tree")
        git(m, "fsck", "--full")
        check(True, "real Git fsck accepts mgit repository")

        # Index interop both directions.
        idx = d / "index"
        init_git_repo(idx)
        write(idx / "a.txt", b"aaa\n")
        git(idx, "add", "a.txt")
        git_commit(idx, "init")

        write(idx / "b.txt", b"bbb\n")
        git(idx, "add", "b.txt")
        check("b.txt" in text(mgit(idx, "status")),
              "mgit reads Index written by real Git")

        write(idx / "c.txt", b"ccc\n")
        mgit(idx, "add", "c.txt")
        check("c.txt" in out(git(idx, "status", "--porcelain")),
              "real Git reads Index updated by mgit")

        # Ref interop both directions.
        refs = d / "refs"
        refs.mkdir()
        mgit(refs, "init")
        write(refs / "f.txt", b"fff\n")
        mgit(refs, "add", "f.txt")
        mgit(refs, "commit", "-m", "mgit init")
        mgit(refs, "tag", "mytag")
        mgit(refs, "branch", "feature")
        check("mytag" in out(git(refs, "tag", "-l")),
              "real Git reads mgit tag")
        check("feature" in out(git(refs, "branch")),
              "real Git reads mgit branch")

        git(refs, "tag", "gittag")
        git(refs, "branch", "gitbranch")
        check("gittag" in text(mgit(refs, "tag")),
              "mgit reads real Git tag")
        check("gitbranch" in text(mgit(refs, "branch")),
              "mgit reads real Git branch")


def test_dogfood() -> None:
    print("\n== dogfood ==")
    with tempdir("dogfood") as d:
        repo = d / "self"
        shutil.copytree(ROOT, repo, ignore=shutil.ignore_patterns(".git", "build"))
        mgit(repo, "init")
        mgit(repo, "add", ".")
        mgit(repo, "commit", "-m", "test: mgit self-host snapshot")

        check("working tree clean" in text(mgit(repo, "status")),
              "mgit sees self-hosted tree as clean")
        git(repo, "fsck", "--full")
        check(out(git(repo, "log", "-1", "--pretty=%s")).strip() ==
              "test: mgit self-host snapshot",
              "real Git reads self-hosted mgit commit")
        check(out(git(repo, "status", "--porcelain")).strip() == "",
              "real Git sees self-hosted tree as clean")


def test_network() -> None:
    print("\n== network ==")
    with tempdir("network") as d:
        src = d / "src"
        repos = d / "repos"
        init_git_repo(src)
        repos.mkdir()

        write(src / "a.txt", b"served over smart http\n")
        git(src, "add", "a.txt")
        git_commit(src, "server c1")
        git(d, "clone", "-q", "--bare", str(src), str(repos / "srv.git"))
        git(repos / "srv.git", "config", "http.receivepack", "true")

        with GitHttpServer(repos) as server:
            clone = d / "clone"
            mgit(d, "clone", f"{server.base_url}/srv.git", "clone")
            check((clone / "a.txt").read_bytes() == b"served over smart http\n",
                  "mgit Smart HTTP clone checks out expected content")
            git(clone, "fsck", "--full")
            check(True, "real Git accepts repository cloned by mgit")

            write(clone / "pushed.txt", b"pushed by mgit\n")
            mgit(clone, "add", "pushed.txt")
            mgit(clone, "commit", "-m", "network push")
            mgit(clone, "push")

            verify = d / "verify"
            git(d, "clone", "-q", f"{server.base_url}/srv.git", str(verify))
            check((verify / "pushed.txt").read_bytes() == b"pushed by mgit\n",
                  "real Git reads Smart HTTP push from mgit")
            git(repos / "srv.git", "fsck", "--full")
            check(True, "server object database stays valid after mgit push")


TESTS = {
    "basic": test_basic,
    "compat": test_compat,
    "dogfood": test_dogfood,
    "network": test_network,
}


def main() -> int:
    if not MGIT.exists():
        print(f"ERROR: mgit binary not found: {MGIT}", file=sys.stderr)
        return 2

    requested = sys.argv[1:] or list(TESTS)
    if requested == ["all"]:
        requested = list(TESTS)

    unknown = [name for name in requested if name not in TESTS]
    if unknown:
        print("unknown test group(s): " + ", ".join(unknown), file=sys.stderr)
        print("available: " + ", ".join(TESTS), file=sys.stderr)
        return 2

    passed = 0
    try:
        for name in requested:
            TESTS[name]()
            passed += 1
    except TestFailure as exc:
        print(f"\n[FAIL] {exc}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"\n[ERROR] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    print(f"\nPYTHON TESTS PASSED: {passed}/{len(requested)} groups")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
