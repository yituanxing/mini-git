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

import hashlib
import os
import shutil
import sys
import zlib
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



def test_checkout() -> None:
    print("\n== checkout ==")
    with tempdir("checkout") as d:
        mgit(d, "init")
        write(d / "a.txt", b"v1\n")
        mgit(d, "add", ".")
        mgit(d, "commit", "-m", "c1")
        c1 = out(git(d, "rev-parse", "HEAD")).strip()

        write(d / "a.txt", b"v2\n")
        mgit(d, "add", ".")
        mgit(d, "commit", "-m", "c2")
        c2 = out(git(d, "rev-parse", "HEAD")).strip()

        # A commit-ish that is not a local branch detaches HEAD.
        mgit(d, "checkout", c1[:7])
        check(out(git(d, "rev-parse", "HEAD")).strip() == c1,
              "checkout commit moves HEAD to that commit")
        check(run([GIT, "symbolic-ref", "-q", "HEAD"], cwd=d).returncode != 0,
              "checkout commit makes HEAD detached")
        check(out(git(d, "rev-parse", "refs/heads/master")).strip() == c2,
              "detached checkout does not move master")
        check("detached HEAD" in text(mgit(d, "status")),
              "mgit status explains detached HEAD")
        check((d / "a.txt").read_text(encoding="utf-8") == "v1\n",
              "detached checkout restores target worktree")

        # Commits made while detached advance HEAD directly, not master.
        write(d / "detached.txt", b"detached work\n")
        mgit(d, "add", "detached.txt")
        mgit(d, "commit", "-m", "detached commit")
        detached_tip = out(git(d, "rev-parse", "HEAD")).strip()
        check(detached_tip != c1 and
              out(git(d, "rev-parse", "HEAD^")).strip() == c1,
              "detached commit advances direct HEAD with the old commit as parent")
        check(out(git(d, "rev-parse", "refs/heads/master")).strip() == c2,
              "detached commit still leaves master unchanged")

        mgit(d, "checkout", "master")
        check(run([GIT, "symbolic-ref", "-q", "HEAD"], cwd=d).returncode == 0,
              "checkout branch reattaches HEAD")
        check(out(git(d, "rev-parse", "HEAD")).strip() == c2,
              "reattaching returns to branch tip")
        check(detached_tip[:7] in text(mgit(d, "reflog")),
              "reflog keeps detached commit recoverable after leaving it")

        # This teaching implementation restores the whole target tree, so it
        # deliberately requires tracked state to be clean before checkout.
        write(d / "staged.txt", b"staged\n")
        mgit(d, "add", "staged.txt")
        before = out(git(d, "rev-parse", "HEAD")).strip()
        refused = run([MGIT, "checkout", c1[:7]], cwd=d)
        check(refused.returncode != 0 and "staged" in text(refused).lower(),
              "checkout refuses staged changes instead of overwriting Index")
        check(out(git(d, "rev-parse", "HEAD")).strip() == before,
              "refused staged checkout leaves HEAD unchanged")
        git(d, "reset", "--hard", "HEAD")

        (d / "a.txt").unlink()
        refused = run([MGIT, "checkout", c1[:7]], cwd=d)
        check(refused.returncode != 0 and "deletion" in text(refused).lower(),
              "checkout refuses tracked Working Tree deletion")
        check(not (d / "a.txt").exists(),
              "refused checkout preserves local deletion")
        git(d, "reset", "--hard", "HEAD")


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




def test_rebase() -> None:
    print("\n== rebase ==")
    with tempdir("rebase") as root:
        # Linear rebase: replay local commit on a new base and therefore
        # produce a new commit id with the upstream tip as parent.
        repo = root / "linear"
        repo.mkdir()
        mgit(repo, "init")
        write(repo / "base.txt", b"base\n")
        mgit(repo, "add", ".")
        mgit(repo, "commit", "-m", "base")
        mgit(repo, "branch", "feature")

        mgit(repo, "checkout", "feature")
        write(repo / "feature.txt", b"feature\n")
        mgit(repo, "add", ".")
        mgit(repo, "commit", "-m", "feature work")
        old_feature = out(git(repo, "rev-parse", "HEAD")).strip()

        mgit(repo, "checkout", "master")
        write(repo / "master.txt", b"master\n")
        mgit(repo, "add", ".")
        mgit(repo, "commit", "-m", "master work")
        new_base = out(git(repo, "rev-parse", "HEAD")).strip()

        mgit(repo, "checkout", "feature")
        mgit(repo, "rebase", "master")
        new_feature = out(git(repo, "rev-parse", "HEAD")).strip()
        check(new_feature != old_feature,
              "rebase rewrites local commit identity")
        check(out(git(repo, "rev-parse", "HEAD^")).strip() == new_base,
              "rebased commit is replayed on top of new base")
        check((repo / "feature.txt").read_text(encoding="utf-8") == "feature\n" and
              (repo / "master.txt").read_text(encoding="utf-8") == "master\n",
              "rebased tree contains upstream and replayed changes")
        git(repo, "fsck", "--full")
        check(True, "real Git accepts mgit rebase result")

        # Rebase is a history rewrite, so this teaching implementation
        # refuses dirty tracked state before moving the branch.
        write(repo / "staged.txt", b"staged\n")
        mgit(repo, "add", "staged.txt")
        before = out(git(repo, "rev-parse", "HEAD")).strip()
        refused = run([MGIT, "rebase", "master"], cwd=repo)
        check(refused.returncode != 0 and "staged" in text(refused).lower(),
              "rebase refuses staged local changes")
        check(out(git(repo, "rev-parse", "HEAD")).strip() == before,
              "refused dirty rebase leaves branch tip unchanged")
        git(repo, "reset", "--hard", "HEAD")

        # Detached HEAD is not a branch to rewrite.
        mgit(repo, "checkout", new_base[:7])
        refused = run([MGIT, "rebase", "master"], cwd=repo)
        check(refused.returncode != 0 and "current branch" in text(refused).lower(),
              "mgit rebase explicitly rejects detached HEAD")

        # A local merge commit needs --rebase-merges semantics. mgit does
        # not implement that complexity and must reject before changing refs.
        merged = root / "merge-history"
        init_git_repo(merged)
        write(merged / "base.txt", b"base\n")
        git(merged, "add", ".")
        git_commit(merged, "base")
        git(merged, "branch", "feature")
        git(merged, "branch", "side")

        write(merged / "master.txt", b"master\n")
        git(merged, "add", ".")
        git_commit(merged, "master")
        master_tip = out(git(merged, "rev-parse", "HEAD")).strip()

        git(merged, "checkout", "-q", "side")
        write(merged / "side.txt", b"side\n")
        git(merged, "add", ".")
        git_commit(merged, "side")

        git(merged, "checkout", "-q", "feature")
        write(merged / "feature.txt", b"feature\n")
        git(merged, "add", ".")
        git_commit(merged, "feature")
        git(merged, "-c", "user.name=Tester", "-c", "user.email=t@t.com",
            "merge", "--no-ff", "side", "-m", "feature merge")
        merge_tip = out(git(merged, "rev-parse", "HEAD")).strip()

        refused = run([MGIT, "rebase", "master"], cwd=merged)
        check(refused.returncode != 0 and "merge commits" in text(refused).lower(),
              "rebase rejects local merge history instead of flattening it silently")
        check(out(git(merged, "rev-parse", "HEAD")).strip() == merge_tip,
              "rejected merge-history rebase leaves feature unchanged")
        check(out(git(merged, "rev-parse", "master")).strip() == master_tip,
              "rejected rebase leaves upstream unchanged")


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




def test_stash() -> None:
    print("\n== stash ==")
    with tempdir("stash") as d:
        mgit(d, "init")
        write(d / "a.txt", b"a1\n")
        write(d / "b.txt", b"b1\n")
        mgit(d, "add", ".")
        mgit(d, "commit", "-m", "base")

        # A clean tree must not create a stash entry.
        clean = mgit(d, "stash")
        check("No local changes to save" in text(clean),
              "stash on a clean repository is a no-op")
        check("(no stashes)" in text(mgit(d, "stash", "list")),
              "clean stash does not create an entry")

        # Default stash saves tracked modifications/deletions, but not untracked.
        write(d / "a.txt", b"a2\n")
        (d / "b.txt").unlink()
        write(d / "untracked.txt", b"keep me\n")

        mgit(d, "stash")
        check((d / "a.txt").read_text(encoding="utf-8") == "a1\n",
              "stash restores modified tracked file to HEAD")
        check((d / "b.txt").read_text(encoding="utf-8") == "b1\n",
              "stash restores deleted tracked file to HEAD")
        check((d / "untracked.txt").read_text(encoding="utf-8") == "keep me\n",
              "default stash leaves untracked files alone")
        check(run([GIT, "diff", "--quiet"], cwd=d).returncode == 0 and
              run([GIT, "diff", "--cached", "--quiet"], cwd=d).returncode == 0,
              "tracked Working Tree and Index are clean after stash")
        check("stash@{0}" in text(mgit(d, "stash", "list")),
              "stash entry is listed")

        mgit(d, "stash", "pop")
        check((d / "a.txt").read_text(encoding="utf-8") == "a2\n",
              "stash pop restores tracked modification")
        check(not (d / "b.txt").exists(),
              "stash pop restores tracked deletion")
        check((d / "untracked.txt").exists(),
              "stash pop preserves unrelated untracked file")
        check(run([GIT, "diff", "--quiet"], cwd=d).returncode != 0,
              "restored stash appears as unstaged tracked changes")
        check(run([GIT, "diff", "--cached", "--quiet"], cwd=d).returncode == 0,
              "stash pop leaves Index aligned with HEAD")
        check("(no stashes)" in text(mgit(d, "stash", "list")),
              "stash pop removes the restored entry")



def test_merge() -> None:
    print("\n== merge ==")
    with tempdir("merge") as root:
        # Fast-forward: move the branch ref; do not invent a merge commit.
        ff = root / "ff"
        ff.mkdir()
        mgit(ff, "init")
        write(ff / "base.txt", b"base\n")
        mgit(ff, "add", ".")
        mgit(ff, "commit", "-m", "base")
        mgit(ff, "branch", "feature")
        mgit(ff, "checkout", "feature")
        write(ff / "feature.txt", b"feature\n")
        mgit(ff, "add", ".")
        mgit(ff, "commit", "-m", "feature")
        feature_tip = out(git(ff, "rev-parse", "HEAD")).strip()
        mgit(ff, "checkout", "master")
        mgit(ff, "merge", "feature")
        check(out(git(ff, "rev-parse", "HEAD")).strip() == feature_tip,
              "fast-forward merge moves master to existing commit")
        parents = out(git(ff, "rev-list", "--parents", "-n", "1", "HEAD")).split()
        check(len(parents) == 2,
              "fast-forward merge does not create a two-parent commit")

        # Diverged histories: create a real two-parent merge commit.
        mgit(ff, "branch", "side")
        write(ff / "master.txt", b"master\n")
        mgit(ff, "add", ".")
        mgit(ff, "commit", "-m", "master work")
        master_before = out(git(ff, "rev-parse", "HEAD")).strip()
        mgit(ff, "checkout", "side")
        write(ff / "side.txt", b"side\n")
        mgit(ff, "add", ".")
        mgit(ff, "commit", "-m", "side work")
        side_tip = out(git(ff, "rev-parse", "HEAD")).strip()
        mgit(ff, "checkout", "master")
        mgit(ff, "merge", "side")
        merge_line = out(git(ff, "rev-list", "--parents", "-n", "1", "HEAD")).split()
        check(len(merge_line) == 3,
              "true merge creates a commit with two parents")
        check(merge_line[1] == master_before and merge_line[2] == side_tip,
              "merge parents are old HEAD then merged branch tip")

        # Dirty tracked state is rejected before merge mutates history.
        dirty = root / "dirty"
        dirty.mkdir()
        mgit(dirty, "init")
        write(dirty / "a.txt", b"base\n")
        mgit(dirty, "add", ".")
        mgit(dirty, "commit", "-m", "base")
        mgit(dirty, "branch", "feature")
        mgit(dirty, "checkout", "feature")
        write(dirty / "feature.txt", b"feature\n")
        mgit(dirty, "add", ".")
        mgit(dirty, "commit", "-m", "feature")
        mgit(dirty, "checkout", "master")
        before = out(git(dirty, "rev-parse", "HEAD")).strip()

        write(dirty / "staged.txt", b"staged\n")
        mgit(dirty, "add", "staged.txt")
        refused = run([MGIT, "merge", "feature"], cwd=dirty)
        check(refused.returncode != 0 and "staged" in text(refused).lower(),
              "merge refuses staged local changes")
        check(out(git(dirty, "rev-parse", "HEAD")).strip() == before,
              "refused dirty merge leaves branch tip unchanged")
        git(dirty, "reset", "--hard", "HEAD")

        (dirty / "a.txt").unlink()
        refused = run([MGIT, "merge", "feature"], cwd=dirty)
        check(refused.returncode != 0 and "deletion" in text(refused).lower(),
              "merge refuses unstaged tracked deletion")
        check(not (dirty / "a.txt").exists(),
              "refused merge preserves local deletion")
        git(dirty, "reset", "--hard", "HEAD")

        mgit(dirty, "checkout", before[:7])
        refused = run([MGIT, "merge", "feature"], cwd=dirty)
        check(refused.returncode != 0 and "current branch" in text(refused).lower(),
              "mgit merge explicitly rejects detached HEAD")


        # MERGE_HEAD is durable state, not a temporary file to discard.
        state = root / "state"
        state.mkdir()
        mgit(state, "init")
        write(state / "f.txt", b"base\n")
        mgit(state, "add", ".")
        mgit(state, "commit", "-m", "base")
        mgit(state, "branch", "side")

        mgit(state, "checkout", "side")
        write(state / "f.txt", b"side\n")
        mgit(state, "add", ".")
        mgit(state, "commit", "-m", "side")

        mgit(state, "checkout", "master")
        write(state / "f.txt", b"master\n")
        mgit(state, "add", ".")
        mgit(state, "commit", "-m", "master")
        master_tip = out(git(state, "rev-parse", "HEAD")).strip()

        conflict = run([MGIT, "merge", "side"], cwd=state)
        check(conflict.returncode != 0 and (state / ".git" / "MERGE_HEAD").is_file(),
              "conflicting merge records MERGE_HEAD")

        saved_merge_head = (state / ".git" / "MERGE_HEAD").read_text().strip()
        again = run([MGIT, "merge", "side"], cwd=state)
        check(again.returncode != 0 and "previous merge" in text(again).lower(),
              "second merge is rejected while previous merge is unresolved")
        check((state / ".git" / "MERGE_HEAD").read_text().strip() == saved_merge_head,
              "rejected second merge preserves original MERGE_HEAD")

        soft = run([MGIT, "reset", "--soft", "HEAD"], cwd=state)
        check(soft.returncode != 0 and "middle of a merge" in text(soft).lower(),
              "reset --soft is rejected during an unresolved merge")
        check((state / ".git" / "MERGE_HEAD").is_file(),
              "failed soft reset preserves merge state")

        mgit(state, "reset", "--mixed", "HEAD")
        check(not (state / ".git" / "MERGE_HEAD").exists(),
              "reset --mixed aborts merge state")
        check(out(git(state, "rev-parse", "HEAD")).strip() == master_tip,
              "mixed merge abort keeps branch at pre-merge HEAD")

        # Clean the conflict-marked Working Tree, then prove --hard also aborts.
        git(state, "reset", "--hard", "HEAD")
        check(run([MGIT, "merge", "side"], cwd=state).returncode != 0,
              "fixture can enter merge conflict again")
        mgit(state, "reset", "--hard", "HEAD")
        check(not (state / ".git" / "MERGE_HEAD").exists() and
              (state / "f.txt").read_text(encoding="utf-8") == "master\n",
              "reset --hard clears merge state and restores Working Tree")

        # Finally resolve a conflict and commit it: MERGE_HEAD must survive
        # until the branch update succeeds, then disappear.
        check(run([MGIT, "merge", "side"], cwd=state).returncode != 0,
              "fixture can enter merge conflict for resolution")
        write(state / "f.txt", b"resolved\n")
        mgit(state, "add", "f.txt")
        mgit(state, "commit", "-m", "resolve merge")
        check(not (state / ".git" / "MERGE_HEAD").exists(),
              "successful merge commit clears MERGE_HEAD")
        merge_parents = out(git(state, "rev-list", "--parents", "-n", "1", "HEAD")).split()
        check(len(merge_parents) == 3,
              "resolved merge commit keeps both parents")




def test_abbrev() -> None:
    print("\n== abbreviated object ids ==")
    with tempdir("abbrev") as d:
        mgit(d, "init")

        # Find a deterministic 4-hex collision in memory, then write only the
        # two colliding blobs. This makes the ambiguity test fast on both CI OSes.
        seen: dict[str, tuple[str, bytes]] = {}
        pair = None
        for i in range(20000):
            data = f"abbrev-collision-{i}\n".encode()
            raw = f"blob {len(data)}\0".encode() + data
            oid = hashlib.sha1(raw).hexdigest()
            prefix = oid[:4]
            if prefix in seen and seen[prefix][0] != oid:
                pair = (seen[prefix], (oid, data))
                break
            seen[prefix] = (oid, data)
        check(pair is not None, "test fixture finds a 4-hex SHA-1 prefix collision")

        (oid1, data1), (oid2, data2) = pair
        write(d / "one.bin", data1)
        write(d / "two.bin", data2)
        got1 = out(mgit(d, "hash-object", "-w", "one.bin")).strip()
        got2 = out(mgit(d, "hash-object", "-w", "two.bin")).strip()
        check(got1 == oid1 and got2 == oid2 and got1[:4] == got2[:4],
              "fixture stores two distinct objects sharing a 4-hex prefix")

        ambiguous = run([MGIT, "cat-file", "-t", got1[:4]], cwd=d)
        check(ambiguous.returncode != 0 and "ambiguous" in text(ambiguous).lower(),
              "short object ID must be unique, not first-match")

        short = run([MGIT, "cat-file", "-t", got1[:3]], cwd=d)
        check(short.returncode != 0,
              "object abbreviation shorter than four hex digits is rejected")

        unique = out(mgit(d, "cat-file", "-t", got1[:8].upper())).strip()
        check(unique == "blob",
              "unique abbreviated object IDs accept uppercase hex too")



def test_integrity() -> None:
    print("\n== integrity ==")
    with tempdir("integrity") as d:
        mgit(d, "init")

        # A loose object's pathname OID must match its canonical content hash.
        write(d / "good.txt", b"good\n")
        good = out(mgit(d, "hash-object", "-w", "good.txt")).strip()
        write(d / "other.txt", b"other\n")
        wrong = out(git(d, "hash-object", "other.txt")).strip()

        src = d / ".git" / "objects" / good[:2] / good[2:]
        dst = d / ".git" / "objects" / wrong[:2] / wrong[2:]
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)

        bad = run([MGIT, "cat-file", "-t", wrong], cwd=d)
        check(bad.returncode != 0 and "hash mismatch" in text(bad).lower(),
              "object reader rejects content stored under the wrong OID")

        # The Index checksum protects the complete serialized staging area.
        write(d / "tracked.txt", b"tracked\n")
        mgit(d, "add", "tracked.txt")
        index_path = d / ".git" / "index"
        original = bytearray(index_path.read_bytes())
        check(len(original) > 32, "test Index has header entries and checksum")

        tampered = bytearray(original)
        tampered[12] ^= 0x01
        index_path.write_bytes(tampered)
        bad_index = run([MGIT, "status"], cwd=d)
        check(bad_index.returncode != 0 and "checksum" in text(bad_index).lower(),
              "Index reader rejects checksum mismatch")
        check(index_path.read_bytes() == tampered,
              "failed Index read does not silently rewrite it as empty")

        index_path.write_bytes(original[:-7])
        truncated = run([MGIT, "status"], cwd=d)
        check(truncated.returncode != 0,
              "Index reader rejects truncated file")



def test_deep_graph() -> None:
    print("\n== deep graph ==")

    def store_object(repo: Path, kind: str, payload: bytes) -> str:
        raw = f"{kind} {len(payload)}\0".encode() + payload
        oid = hashlib.sha1(raw).hexdigest()
        path = repo / ".git" / "objects" / oid[:2] / oid[2:]
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(zlib.compress(raw))
        return oid

    def commit(repo: Path, tree: str, parents: list[str], message: str) -> str:
        lines = [f"tree {tree}\n"]
        lines.extend(f"parent {p}\n" for p in parents)
        lines += [
            "author Tester <t@t.com> 1 +0000\n",
            "committer Tester <t@t.com> 1 +0000\n",
            "\n",
            message + "\n",
        ]
        return store_object(repo, "commit", "".join(lines).encode())

    with tempdir("deep-graph") as root:
        repo = root / "merge"
        repo.mkdir()
        mgit(repo, "init")
        empty_tree = out(mgit(repo, "write-tree")).strip()

        base = commit(repo, empty_tree, [], "base")
        ours = base
        for i in range(1105):
            ours = commit(repo, empty_tree, [ours], f"ours-{i}")
        side = commit(repo, empty_tree, [base], "side")

        write(repo / ".git" / "refs" / "heads" / "master", ours + "\n")
        write(repo / ".git" / "refs" / "heads" / "side", side + "\n")
        mgit(repo, "merge", "side")
        parents = out(git(repo, "rev-list", "--parents", "-n", "1", "HEAD")).split()
        check(len(parents) == 3 and parents[1] == ours and parents[2] == side,
              "merge-base traversal remains correct beyond 1000 commits")

        # Commit parsing must preserve every parent of an octopus merge.
        roots = [commit(repo, empty_tree, [], f"root-{i}") for i in range(20)]
        octopus = commit(repo, empty_tree, roots, "octopus")
        write(repo / ".git" / "refs" / "heads" / "master", octopus + "\n")
        log = text(mgit(repo, "log", "-n", "1"))
        check(all(p[:7] in log for p in roots),
              "commit parser preserves more than 16 parents")

        # Local-path fetch used to silently stop walking after 1000 commits.
        src = root / "src"
        dst = root / "dst"
        src.mkdir()
        dst.mkdir()
        mgit(src, "init")
        src_tree = out(mgit(src, "write-tree")).strip()
        tip = commit(src, src_tree, [], "root")
        for i in range(1105):
            tip = commit(src, src_tree, [tip], f"c-{i}")
        write(src / ".git" / "refs" / "heads" / "master", tip + "\n")

        mgit(dst, "init")
        mgit(dst, "remote", "add", "origin", str(src))
        mgit(dst, "fetch", "origin")
        count = int(out(git(dst, "rev-list", "--count",
                            "refs/remotes/origin/master")).strip())
        check(count == 1106,
              "local fetch transfers complete history beyond 1000 commits")
        git(dst, "fsck", "--full")
        check(True, "real Git validates deep local-fetch object closure")



def test_mainline() -> None:
    print("\n== mainline ==")
    with tempdir("mainline") as d:
        init_git_repo(d)
        write(d / "base.txt", b"base\n")
        git(d, "add", ".")
        git_commit(d, "base")

        git(d, "branch", "feature")
        git(d, "checkout", "-q", "feature")
        write(d / "feature.txt", b"feature\n")
        git(d, "add", ".")
        git_commit(d, "feature")

        git(d, "checkout", "-q", "master")
        write(d / "master.txt", b"master\n")
        git(d, "add", ".")
        git_commit(d, "master")
        git(d, "-c", "user.name=Tester", "-c", "user.email=t@t.com",
            "merge", "--no-ff", "feature", "-m", "merge")
        merge_commit = out(git(d, "rev-parse", "HEAD")).strip()
        parent1 = out(git(d, "rev-parse", "HEAD^1")).strip()

        # Put HEAD before the merge so cherry-pick/revert have a meaningful target.
        git(d, "reset", "--hard", parent1)
        before = out(git(d, "rev-parse", "HEAD")).strip()

        real_cp = run([GIT, "cherry-pick", merge_commit], cwd=d)
        check(real_cp.returncode != 0,
              "real Git requires a mainline to cherry-pick a merge commit")
        cp = run([MGIT, "cherry-pick", merge_commit], cwd=d)
        check(cp.returncode != 0 and "mainline" in text(cp).lower(),
              "mgit refuses merge cherry-pick instead of guessing parent 1")
        check(out(git(d, "rev-parse", "HEAD")).strip() == before,
              "refused merge cherry-pick leaves HEAD unchanged")

        real_revert = run([GIT, "revert", "--no-edit", merge_commit], cwd=d)
        check(real_revert.returncode != 0,
              "real Git requires a mainline to revert a merge commit")
        rv = run([MGIT, "revert", merge_commit], cwd=d)
        check(rv.returncode != 0 and "mainline" in text(rv).lower(),
              "mgit refuses merge revert instead of guessing parent 1")
        check(out(git(d, "rev-parse", "HEAD")).strip() == before,
              "refused merge revert leaves HEAD unchanged")


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
    "checkout": test_checkout,
    "commit": test_commit,
    "diff": test_diff,
    "rebase": test_rebase,
    "reset": test_reset,
    "dogfood": test_dogfood,
    "abbrev": test_abbrev,
    "integrity": test_integrity,
    "deep-graph": test_deep_graph,
    "mainline": test_mainline,
    "merge": test_merge,
    "stash": test_stash,
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
