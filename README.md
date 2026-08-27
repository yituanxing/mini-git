# mgit — 从零用 C 语言实现的迷你 Git

**mgit (mini-git)**：一个用 C 语言从零实现的 Git，目标是与真实 Git **双向兼容**——它生成的仓库能通过 `git fsck` 校验，它也能克隆、拉取、推送真实 Git 服务器（含 Gitee）上的仓库。

> mgit: a Git reimplementation in plain C, bidirectionally compatible with real Git.

## 为什么造这个轮子 / Why

为了真正弄懂 Git 的底层原理：内容寻址的对象库、四种对象、暂存区、三路合并、packfile、Smart HTTP 协议……最好的学习方式就是亲手写一个。完整的设计思路、架构图和扩展指南见 **[DESIGN.md](DESIGN.md)**。

## 功能一览 / Features

- **28 个命令**：init / add / commit / status / log / diff / branch / checkout / merge / rebase / cherry-pick / stash / reflog / tag / reset / revert / gc …
- **网络协作**：clone / fetch / pull / push，支持本地路径与 Smart HTTP（http/https，Basic Auth）
- **兼容性**：松散对象、packfile、index、ref 格式与真实 Git 互通，`git fsck` 全绿
- **测试**：17 个测试套件、400+ 断言，含对抗性用例

## 构建 / Build（Windows + MinGW）

```
mingw32-make        # 编译，产出 build/mgit.exe
mingw32-make test   # 跑全部测试
```

## 快速体验 / Quick Start

```
mgit init
echo hello > a.txt
mgit add a.txt
mgit commit -m "first commit / 首次提交"
mgit log
```

## 文档 / Documentation

- [DESIGN.md](DESIGN.md)：核心思想、代码架构、命令清单（含与真实 git 的参数差异）、扩展指南

---

*本仓库由 mgit 自己管理（自举）——提交历史就是它最好的广告。*
