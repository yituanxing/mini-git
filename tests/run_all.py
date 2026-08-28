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





def test_branch() -> None:
    print("\n== branch ==")
    with tempdir("branch") as d:
        mgit(d, "init")
        write(d / "base.txt", b"base\n")
        mgit(d, "add", ".")
        mgit(d, "commit", "-m", "base")

        # A branch whose tip is already an ancestor of HEAD is safe to delete.
        mgit(d, "branch", "done")
        write(d / "master.txt", b"master\n")
        mgit(d, "add", ".")
        mgit(d, "commit", "-m", "master advances")
        check(run([GIT, "merge-base", "--is-ancestor", "done", "HEAD"], cwd=d).returncode == 0,
              "real Git confirms done is merged into HEAD")
        mgit(d, "branch", "-d", "done")
        check(run([GIT, "show-ref", "--verify", "--quiet", "refs/heads/done"], cwd=d).returncode != 0,
              "branch -d deletes a fully merged branch")

        # An unmerged branch must survive safe deletion.
        mgit(d, "branch", "feature")
        mgit(d, "checkout", "feature")
        write(d / "feature.txt", b"feature\n")
        mgit(d, "add", ".")
        mgit(d, "commit", "-m", "feature work")
        feature_tip = out(git(d, "rev-parse", "HEAD")).strip()
        mgit(d, "checkout", "master")

        check(run([GIT, "merge-base", "--is-ancestor", feature_tip, "HEAD"], cwd=d).returncode != 0,
              "real Git confirms feature is not merged into HEAD")
        refused = run([MGIT, "branch", "-d", "feature"], cwd=d)
        check(refused.returncode != 0,
              "branch -d refuses an unmerged branch")
        check(run([GIT, "show-ref", "--verify", "--quiet", "refs/heads/feature"], cwd=d).returncode == 0,
              "refused branch -d leaves the branch intact")


def test_commit() -> None:
    print("\n== commit ==")
    with tempdir("commit") as d:
        mgit(d, "init")
        write(d / "a.txt", b"a1\n")
        write(d / "b.txt", b"b1\n")
        mgit(d, "add", ".")
        mgit(d, "commit", "-m", "base")

        # -a operates on already tracked paths: modified + deleted.
        write(d / "a.txt", b"a2\n")
        (d / "b.txt").unlink()
        write(d / "untracked.txt", b"do not include me\n")
        mgit(d, "commit", "-a", "-m", "commit-a")

        check(out(git(d, "show", "HEAD:a.txt")).replace("\r\n", "\n") == "a2\n",
              "commit -a includes modified tracked file")
        check(run([GIT, "cat-file", "-e", "HEAD:b.txt"], cwd=d).returncode != 0,
              "commit -a includes tracked deletion")
        tree_names = out(git(d, "ls-tree", "-r", "--name-only", "HEAD"))
        check("untracked.txt" not in tree_names,
              "commit -a does not add untracked files")
        check("untracked.txt" in out(git(d, "status", "--porcelain")),
              "untracked file remains untracked after commit -a")
        git(d, "fsck", "--full")
        check(True, "real Git accepts commit -a result")


def test_diff() -> None:
    print("\n== diff ==")
    with tempdir("diff") as d:
        mgit(d, "init")
        write(d / "a.txt", b"v1\n")
        mgit(d, "add", "a.txt")
        mgit(d, "commit", "-m", "base")

        # Working Tree differs from Index, while Index still equals HEAD.
        write(d / "a.txt", b"v2\n")
        unstaged = text(mgit(d, "diff"))
        staged = text(mgit(d, "diff", "--cached"))
        check("a.txt" in unstaged and "modified" in unstaged,
              "diff shows Working Tree vs Index changes")
        check("a.txt" not in staged,
              "diff --cached stays clean when Index still equals HEAD")
        check(run([GIT, "diff", "--quiet"], cwd=d).returncode != 0,
              "real Git agrees there is an unstaged change")
        check(run([GIT, "diff", "--cached", "--quiet"], cwd=d).returncode == 0,
              "real Git agrees there is no staged change")

        # After add, Index moves to v2: default diff becomes clean while
        # --cached now sees the staged difference from HEAD.
        mgit(d, "add", "a.txt")
        unstaged = text(mgit(d, "diff"))
        staged = text(mgit(d, "diff", "--cached"))
        check("a.txt" not in unstaged,
              "add moves the diff boundary so Working Tree equals Index")
        check("a.txt" in staged and "modified" in staged,
              "diff --cached shows Index vs HEAD changes")
        check(run([GIT, "diff", "--quiet"], cwd=d).returncode == 0,
              "real Git agrees Working Tree now equals Index")
        check(run([GIT, "diff", "--cached", "--quiet"], cwd=d).returncode != 0,
              "real Git agrees Index now differs from HEAD")

        # The three areas can all hold different snapshots at once:
        # HEAD=v1, Index=v2, Working Tree=v3.
        write(d / "a.txt", b"v3\n")
        check("a.txt" in text(mgit(d, "diff")),
              "default diff still observes Working Tree vs Index")
        check("a.txt" in text(mgit(d, "diff", "--cached")),
              "cached diff independently observes Index vs HEAD")

        # Untracked files are outside the Index and are not part of git diff.
        write(d / "untracked.txt", b"new\n")
        check("untracked.txt" not in text(mgit(d, "diff")),
              "default diff ignores untracked files like real Git")

        # Deletion of a tracked file is an unstaged Working Tree change.
        (d / "a.txt").unlink()
        deleted = text(mgit(d, "diff"))
        check("a.txt" in deleted and "deleted" in deleted,
              "default diff reports tracked Working Tree deletion")



def test_reset() -> None:
    print("\n== reset ==")
    with tempdir("reset") as d:
        def history(name: str):
            repo = d / name
            repo.mkdir()
            mgit(repo, "init")
            write(repo / "a.txt", b"v1\n")
            mgit(repo, "add", "a.txt")
            mgit(repo, "commit", "-m", "c1")
            c1 = out(git(repo, "rev-parse", "HEAD")).strip()

            write(repo / "a.txt", b"v2\n")
            mgit(repo, "add", "a.txt")
            mgit(repo, "commit", "-m", "c2")
            c2 = out(git(repo, "rev-parse", "HEAD")).strip()
            return repo, c1, c2

        # --soft: only HEAD moves. Index and Working Tree stay at c2.
        repo, c1, c2 = history("soft")
        mgit(repo, "reset", "--soft", c1)
        check(out(git(repo, "rev-parse", "HEAD")).strip() == c1,
              "reset --soft moves HEAD")
        check((repo / "a.txt").read_text(encoding="utf-8") == "v2\n",
              "reset --soft keeps Working Tree")
        check(run([GIT, "diff", "--quiet"], cwd=repo).returncode == 0,
              "reset --soft keeps Index equal to Working Tree")
        check(run([GIT, "diff", "--cached", "--quiet"], cwd=repo).returncode != 0,
              "reset --soft keeps old Index, now different from HEAD")

        # --mixed: HEAD + Index move, Working Tree stays. This is the default.
        repo, c1, _ = history("mixed")
        mgit(repo, "reset", "--mixed", c1)
        check(out(git(repo, "rev-parse", "HEAD")).strip() == c1,
              "reset --mixed moves HEAD")
        check((repo / "a.txt").read_text(encoding="utf-8") == "v2\n",
              "reset --mixed keeps Working Tree")
        check(run([GIT, "diff", "--cached", "--quiet"], cwd=repo).returncode == 0,
              "reset --mixed resets Index to HEAD")
        check(run([GIT, "diff", "--quiet"], cwd=repo).returncode != 0,
              "reset --mixed leaves Working Tree different from Index")

        repo, c1, _ = history("default")
        mgit(repo, "reset", c1)
        check(run([GIT, "diff", "--cached", "--quiet"], cwd=repo).returncode == 0 and
              run([GIT, "diff", "--quiet"], cwd=repo).returncode != 0,
              "plain reset defaults to --mixed")

        # --hard: all three snapshots move together.
        repo, c1, c2 = history("hard")
        mgit(repo, "reset", "--hard", c1)
        check(out(git(repo, "rev-parse", "HEAD")).strip() == c1,
              "reset --hard moves HEAD")
        check((repo / "a.txt").read_text(encoding="utf-8") == "v1\n",
              "reset --hard restores Working Tree")
        check(run([GIT, "diff", "--quiet"], cwd=repo).returncode == 0 and
              run([GIT, "diff", "--cached", "--quiet"], cwd=repo).returncode == 0,
              "reset --hard makes HEAD Index and Working Tree agree")
        check(c2[:7] in text(mgit(repo, "reflog")),
              "reflog keeps the commit that reset moved away from")


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
            check((verify / "pushed.txt").read_text(encoding="utf-8") == "pushed by mgit\n",
                  "real Git reads Smart HTTP push from mgit")
            git(repos / "srv.git", "fsck", "--full")
            check(True, "server object database stays valid after mgit push")


TESTS = {
    "basic": test_basic,
    "compat": test_compat,
    "branch": test_branch,
    "commit": test_commit,
    "diff": test_diff,
    "reset": test_reset,
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
