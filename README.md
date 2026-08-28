# mgit — 从零用 C 语言实现的教学 Git

**mgit (mini-git)** 是一个可运行、可调试、可与真实 Git 互操作的 Git 教学实现。

它的目标不是覆盖 Git 的所有功能，而是把常见 Git 知识真正写成代码：
Working Tree / Index / HEAD、对象与引用、branch、merge/rebase、reset/revert、
stash/reflog，以及 clone/fetch/pull/push 背后的 Smart HTTP 与 pack。

保留下来的能力尽量让真实 Git 来验收：mgit 生成的仓库可以用
`git fsck` / `git cat-file` 检查，网络测试直接对接真实
`git http-backend`。

## 为什么造这个轮子 / Why

很多 Git 概念只背结论很容易混淆。例如：

- `git diff` 与 `git diff --cached` 到底在比较哪两层？
- `reset --soft / --mixed / --hard` 分别移动 HEAD、Index、Working Tree 中的什么？
- branch 与 HEAD 是什么关系？detached HEAD 为什么不会移动分支？
- fast-forward merge 为什么没有新 commit，而普通 merge 为什么有两个 parent？
- rebase 为什么会产生新的 commit hash？
- fetch 与 pull 到底差在哪一步？

mgit 希望这些问题能直接通过源码和实验看懂，而不是只靠背答案。

## 功能一览 / Features

- **28 个命令**：init / add / commit / status / log / diff / branch / checkout /
  merge / rebase / cherry-pick / stash / reflog / tag / reset / revert / gc …
- **真实 Git 数据模型**：blob / tree / commit、Index、refs、loose objects、packfile
- **网络协作**：clone / fetch / pull / push，使用 Git Smart HTTP
- **双平台**：
  - Windows：MinGW + WinHTTP
  - Linux：GCC + libcurl
- **互操作测试**：同一份 Python 测试在 Windows/Linux 运行，并用真实 Git 作为 oracle
- **Dogfood**：mgit 可以管理一份自己的源码快照，真实 Git 能继续读取和校验

## 构建 / Build

### Windows / MinGW

```text
mingw32-make
mingw32-make test
```

输出：`build/mgit.exe`

### Linux

```text
make
make test
```

输出：`build/mgit`

`make test` 在两个平台都运行 `tests/run_all.py`。迁移期间 Windows
仍保留旧 PowerShell 回归套件作为额外保险。

## 快速体验 / Quick Start

```text
mgit init
echo hello > a.txt
mgit add a.txt
mgit commit -m "first commit"
mgit status
mgit log
```

想理解底层对象，可以继续：

```text
mgit hash-object -w a.txt
mgit cat-file -t <hash>
mgit write-tree
mgit commit-tree <tree> -m "manual commit"
```

## 文档 / Documentation

- [DESIGN.md](DESIGN.md)：Git 核心模型、代码架构、命令语义与 mgit 的刻意简化

---

*本仓库持续用 mgit 自己做 dogfood；真实 Git 负责交叉验收。*
