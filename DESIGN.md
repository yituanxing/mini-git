# mgit 设计文档

> mgit（mini-git）：一个用 C 语言从零实现、可与真实 Git 互操作的教学 Git。
> 定位：把常见 Git 概念和面试知识变成可运行、可调试、可读源码的实现；
> 在不牺牲可读性的前提下保留足够的实际使用能力。

本文档分四部分：
1. [Git 核心思想](#一git-核心思想) —— 理解本项目前必须理解的东西
2. [代码架构](#二代码架构) —— 目录、模块、分层、关键数据结构
3. [命令清单](#三命令清单与参数支持) —— 每个命令支持什么、与真实 git 的差异
4. [维护原则](#四扩展指南想加新功能看这里) —— 如何在不教错 Git 的前提下改代码

---

## 一、Git 核心思想

写代码之前先吃透这五点，本项目每一处设计都是它们的直接推论。

### 1.1 内容寻址：一切皆对象，哈希即地址

Git 的本质是一个**以 SHA-1 哈希为键的对象数据库**。任何内容
（文件、目录、提交）写入前，先算出它的内容哈希，哈希就是它的"地址"。

- 内容相同 ⇒ 哈希相同 ⇒ 只存一份（天然去重）
- 内容变一个字节 ⇒ 对象 ID 随之改变（内容寻址能暴露内容不一致，但 SHA-1 不是现代安全防篡改机制）
- 对象之间**只通过哈希互相引用**，没有文件名、没有路径硬编码

对象存储布局（本项目与真实 git 完全一致）：

```
.git/objects/
├── 3f/                  # 哈希前 2 位作目录名
│   └── 28c218c44b...    # 剩余 38 位作文件名（内容是 zlib 压缩的）
└── pack/                # 打包后的对象（.pack + .idx）
```

松散对象的磁盘格式：`"<类型> <长度>\0" + zlib(原始内容)`。

### 1.2 四种对象类型

| 类型 | 内容 | 大白话 |
|---|---|---|
| `blob` | 文件内容（不含文件名！） | 一张"文件内容快照" |
| `tree` | 若干 `<模式> <名字>\0<哈希>` 条目 | 目录清单：谁叫什么名、指向哪个 blob/子 tree |
| `commit` | tree 哈希 + 父提交哈希 + 作者/时间 + 消息 | 一次"存档"，指着一个目录快照，串着历史 |
| `tag` | 指向某对象的哈希 + 标签信息 | 这是 **annotated tag object**；mgit 当前只创建 lightweight tag，因此不会创建这种对象 |

关键推论：**提交记录的是整个项目的目录快照（通过 tree），不是差异**。
"两个版本的差别"是运行时对比两棵 tree 算出来的，存储里没有"修改记录"这种东西。

### 1.3 四层结构：工作区 → Index → 对象库 → 引用

```
工作区 (你看到的文件)
   │  add
   ▼
Index (.git/index，暂存区：下次提交要包含的文件清单)
   │  commit  (把 index 固化成 tree + commit 对象)
   ▼
对象库 (.git/objects/，保存不可变对象；存储可被 gc 打包/清理)
   ▲
引用 (.git/refs/ + HEAD：可移动的"标签"，指向某个对象哈希)
```

- **分支** = `refs/heads/<名字>` 文件里的一行哈希。移动分支 = 改一个文件。
- **HEAD** 通常是指向当前分支的符号引用；detached HEAD 时直接保存 commit 哈希。
- **reset** 的核心是移动 HEAD/分支，并按模式决定是否同步 Index 和工作区。
- **rebase** 会把一串提交的改动重放到新基底上，生成新的 commit，再移动分支。
- **merge** 若能 fast-forward 只移动分支；否则会创建一个新的多父 commit。

### 1.4 不可变与可达性

对象内容一旦写入就不原地修改；内容变化会产生新的对象 ID。所谓"删除提交"
通常只是让引用不再指向它，使它变成不可达对象。真实 Git 还会考虑 reflog、
过期时间等保留策略；mgit 的 gc 为教学简化，按自己的可达性规则处理对象。

### 1.5 分布式 = 对象库的差集同步

两个仓库互通，本质就是：
1. 比较双方的引用，算出"你有什么我没有"（对象哈希差集）；
2. 把差集对象打包传过去；
3. 对方更新引用。

clone/fetch/push 全部是这三步的不同姿势。网络协议只是"怎么把差集
传过去"的信封。理解这一点，[transport.c](src/core/transport.c) 的
几百行就没有任何魔法了。

---

## 二、代码架构

### 2.1 目录与分层

```
src/
├── main.c          # 命令注册表 + 分发（加命令只需动这里一行）
├── command.h       # Command 接口：{name, description, run, help}
├── base/           # 最底层：与 git 语义无关的工具
│   ├── hash.*      # SHA-1 实现（自写，不依赖 OpenSSL）
│   ├── zlib_util.* # zlib 压缩/解压封装
│   ├── file.*      # 文件读写、路径拼接、目录创建
│   ├── http.*      # Windows WinHTTP 后端
│   ├── http_curl.c # Linux libcurl 后端；二者共用 http.h 接口
│   └── error.h     # mgit_error 统一错误输出
├── core/           # git 领域核心：对象模型 + 协议
│   ├── object.*    # 松散对象读写（类型校验、按哈希前缀查找）
│   ├── ref.*       # refs/HEAD 管理 + packed-refs 读取
│   ├── tree.*      # tree 解析/序列化/递归展开
│   ├── commit.*    # commit 解析/生成
│   ├── graph.*     # commit DAG 的共享祖先判断（动态遍历）
│   ├── index.*     # index v2 读写 + write-tree（含 checksum / 路径排序）
│   ├── linemerge.* # 行级三路合并（merge 冲突处理的核心算法）
│   ├── ignore.*    # .gitignore 规则匹配
│   ├── remote.*    # .git/config 中 remote 配置读写
│   ├── pack.*      # pack 写出（gc/push）+ 解包（clone/fetch，含 ofs-delta）
│   ├── pack_index.*# .idx v2 解析
│   └── transport.* # Git Smart HTTP 客户端（广告解析/协商/推送回执）
└── commands/       # 28 个命令，每个一个文件；保留可读的命令流程
```

**分层纪律**：整体保持 `commands → core → base` 的单向依赖。
底层格式、对象模型、协议 primitive 放在 core/base；命令层允许保留有教学价值的
流程编排（例如 rebase 如何逐个重放提交），避免为了抽象而把 Git 原理藏起来。

### 2.2 核心数据结构

| 结构 | 位置 | 说明 |
|---|---|---|
| `Hash` | base/hash.h | 20 字节 SHA-1 |
| `Object` | core/object.h | `{type, size, data}`，读完必须 `object_free` |
| `ObjectStore` | core/object.h | 对象库句柄，`object_store_open(".git")` |
| `Index/IndexEntry` | core/index.h | 暂存区，条目含路径/哈希/模式 |
| `Tree/TreeEntry` | core/tree.h | 目录快照，条目含模式/名字/哈希/类型 |
| `Commit` | core/commit.h | 双亲数组 + tree 哈希 + 消息 |
| `RefManager` | core/ref.h | 引用的读写门面 |
| `RefAd` | core/transport.h | 服务器引用广告（哈希+名字的数组） |
| `PushUpdate` | core/transport.h | 一条推送指令（引用名 + 新旧哈希） |

### 2.3 一个提交的完整生命周期（读代码的参考路线）

```
mgit add a.txt      cmd_add.c    → ignore 过滤 → 内容算哈希写松散对象 → 记入 Index
mgit commit -m x    cmd_commit.c → index_write_tree (index.c: 递归建树)
                                 → commit 对象生成 → 移动分支引用 → 写 reflog
mgit push           cmd_push.c   → 广告对比 → 收集差集对象 → pack_build_memory
                                 → transport_push_refs (transport.c)
```

读懂这三条链路，其余 25 个命令都是同一批积木的不同搭法。

### 2.4 网络协议实现要点（Smart HTTP, protocol v0）

三种操作共用同一个积木：`transport_get_refs_service`（GET 广告）+
pkt-line 帧构造/解析 + 平台 HTTP 后端 POST（Windows=WinHTTP，Linux=libcurl）。

| 操作 | 端点 | 请求体 | 响应 |
|---|---|---|---|
| clone | `POST /git-upload-pack` | want 全部广告哈希 + done | side-band 包裹的完整 pack |
| fetch | `POST /git-upload-pack` | want 差集尖端 + have 本地已有 + done | 差集 pack（服务器算） |
| push | `POST /git-receive-pack` | `<old> <new> <ref>\0<能力串>` 指令 + 裸 pack | report-status 回执 |

**踩坑记录（每条都真实咬过人，改协议前必读）**：

1. **push 首条指令的能力串分隔符是 `\0`，不是空格**（`transport.c`
   `build_push_request`）。upload-pack 的 want 行用空格，两者不一样；
   用空格会被服务端当引用名一部分拒收，且返回空响应体。
2. **不要把结构体内嵌字段当连续数组传**。`&ad->refs[0].hash` 这种写法
   在 `RemoteRef`（280 字节）里指针步进是 20 字节，第二个元素会读进
   引用名字节，服务器报 "not our ref"（哈希解码出来是 ASCII）。
   需要连续就先拷成紧凑 `Hash` 数组。
3. **index 写出前必须按路径排序**（`index_write`），否则真实 git
   报 "unordered stage entries" 拒读。
4. **tree 里目录的 mode 存 `40000`（无前导 0），显示时补成 `040000`**。
   存储带前导 0 会导致哈希与真实 git 不一致。
5. **遍历队列别用固定长度静默截断**。push 收集提交曾固定 1024，
   超过会无声丢祖先、推坏服务器仓库；已改动态扩容。
6. **对象为空 ≠ 无事可做**：推送新分支时对象可能全在服务器，
   但引用缺失，仍要发空 pack 更新引用。
7. **排障方法**：Python `urllib` 原样重放请求 + 绕过封装直连
   `git http-backend` 看 stderr，是定位协议问题最快的两招。

### 2.5 构建与测试

Windows / MinGW：
```
mingw32-make
mingw32-make test
```

Linux：
```
make
make test
```

`make test` / `mingw32-make test` 都运行同一份 Python 标准库测试
`tests/run_all.py`。迁移期间 Windows 还保留旧 PowerShell 套件作为回归保险。
测试原则：
- 本地和 CI 调用同一套 Python 测试合同；
- `tests/git_http_server.py` 包装真实 `git http-backend`，网络测试不是 mock；
- 涉及对象、Index、refs、网络等兼容语义时，让真实 Git 作为 oracle，
  用 `git fsck` / `git cat-file` / `git diff` 等交叉验证；
- Windows 和 Linux CI 分别验证 MinGW+WinHTTP 与 GCC+libcurl。

---

## 三、命令清单与参数支持

共 **28 个命令**。"差异"栏只列与真实 git **不一样**或**不支持**的点；
没列的行为即为一致。

### 3.1 日常命令

| 命令 | 支持的形式 | 与真实 git 的差异 |
|---|---|---|
| `init` | `mgit init [目录]` | 无 `--bare`、`-b` 选项；默认分支名 `master` |
| `add` | `mgit add <文件>... \| . \| -A` | 无 `-u`（只更新已跟踪）、无 `-p` |
| `commit` | `mgit commit -m <消息> [-a] [--amend]` | **`-m` 必填**，没有编辑器交互；无 `--author` 等 |
| `status` | `mgit status` | 输出格式简化；无 `-s` 等选项 |
| `log` | `mgit log [--oneline] [-n N] [分支]` | 遍历所有可达 parent，但显示顺序是简化 BFS，不等同于真实 Git 完整的 revision ordering；无 `--graph`/范围语法/`-p` |
| `diff` | `mgit diff [--cached] [<tree> [<tree>]]` | 两个哈希时比较两棵 tree；无文件过滤参数 |
| `branch` | `mgit branch [<名字> \| -d <名字>]` | 无 `-a`（远端分支列表）、无 `-m` 改名 |
| `checkout` | `mgit checkout <分支或提交>`、`mgit checkout -b <新分支>` | 支持 detached HEAD；为避免整棵恢复覆盖数据，mgit 要求 tracked Index/Working Tree 干净（比真实 Git 更保守）；无单文件恢复 |
| `reset` | `mgit reset [--soft|--mixed|--hard] <哈希>` | 默认 `--mixed`，三模式与真实 Git 的三棵树语义一致；无路径级 reset |
| `revert` | `mgit revert <提交>` | 无 `--no-commit`；merge commit 因未实现 `-m` mainline 而明确拒绝 |
| `tag` | `mgit tag [<名字> \| -l \| -d <名字>]` | **只有 lightweight tag**：`refs/tags/name → commit`，不会创建 tag object；不支持 annotated tag |
| `stash` | `mgit stash [push\|pop\|list\|drop]` | 默认保存 tracked 修改/删除且忽略 untracked；内部 stash 存储是教学简化，不复刻真实 refs/stash reflog/commit 图 |
| `reflog` | `mgit reflog [show]` | 无过期清理、无按引用查看 |
| `gc` | `mgit gc` | 打包为全量对象（无发送端风格的 delta 压缩） |
| `count-objects` | `mgit count-objects [-v]` | — |

### 3.2 协作命令

| 命令 | 支持的形式 | 与真实 git 的差异 |
|---|---|---|
| `merge` | `mgit merge <分支>` | FF 与双亲 merge 语义正确；为保护数据要求 tracked 状态干净（比真实 Git 更保守）；冲突策略为简化行级三路合并 |
| `cherry-pick` | `mgit cherry-pick <提交>` | 一次一个提交；无 sequencer/`--continue`；merge commit 因未实现 `-m` mainline 而明确拒绝 |
| `rebase` | `mgit rebase <分支>` / `--continue` / `--abort` | 教学版只重放**线性本地历史**；detached HEAD 和含 merge commit 的本地历史明确拒绝；无 `-i`/`--onto`/`--rebase-merges` |
| `pull` | `mgit pull [<remote>] [<分支>]` | 真实 Git 是 fetch + integrate；mgit 固定选择 merge 作为 integrate 策略 |
| `fetch` | `mgit fetch [<remote>]` | 单轮协商（一次 have 往返），不做多轮收敛 |
| `push` | `mgit push [-f|--force] [<remote>] [<分支>]` | **一次一个分支**；无 `-u`、无标签推送；默认拒绝非快进 |
| `clone` | `mgit clone <路径或URL> [目录]` | 无 `--depth`/`--branch`/`--bare` |
| `remote` | `mgit remote [-v]` / `add <名> <地址>` / `remove <名>` | **没有 `set-url`**；地址可以是本地路径或 http(s) URL |

### 3.3 底层命令（理解原理/调试用）

| 命令 | 支持的形式 | 说明 |
|---|---|---|
| `hash-object` | `mgit hash-object [-w] [-t <类型>] <文件>` | `-w` 写入对象库 |
| `cat-file` | `mgit cat-file [-p \| -t \| -s] <对象>` | 支持短哈希；`-p` 对 tree 输出与真实 git 逐字符一致的条目列表 |
| `write-tree` | `mgit write-tree` | 把当前 Index 固化成 tree |
| `commit-tree` | `mgit commit-tree <tree> [-p <父>] [-m <消息>]` | 手工造提交 |
| `ls-tree` | `mgit ls-tree <tree\|commit\|分支\|HEAD>` | 列目录快照条目 |

### 3.4 刻意不实现的（及原因）

| 功能 | 不做的原因 |
|---|---|
| 交互式命令（编辑器、进度条交互） | 教学项目聚焦数据结构与协议 |
| 更完整的 push 策略（多 ref、upstream 等） | 会增加工程复杂度，不帮助当前教学主线 |
| 协议 v2 / SSH | v0 已能覆盖全部互通需求 |
| 子模块 / 稀疏检出 / LFS | 与核心原理无关的上层功能 |
| 发送端更复杂的 delta 压缩 | 当前教学目标不需要，保持现实现即可 |

---

## 四、维护原则

### 4.1 当前维护方向

当前阶段**冻结横向功能扩展**。维护优先级只有三类：

1. 会把 Git 核心概念教错的行为；
2. 会破坏 Git 格式/互操作的不变量；
3. 能用更少、更清晰代码消除同一概念的重复实现。

常用基础积木：

| 想做什么 | 用什么 |
|---|---|
| 解析一个引用/分支名到哈希 | `ref_resolve*`（ref.h） |
| 读对象 | `object_store_read` → 按 `obj.type` 分派 |
| 解析提交 | `commit_parse` / `commit_free` |
| 遍历目录 | `tree_parse` + `tree_flatten` |
| 判断 commit 祖先关系 | `graph_is_ancestor`（graph.h） |
| 读写暂存区 | `index_open` / `index_write` |

### 4.2 改现有命令时的原则

优先保证 Git 概念正确，而不是继续扩参数数量。路线是：
**先用真实 Git 明确语义 → 写最小回归 → 修改实现 → Windows/Linux 同测**。
例如 `diff` 要守住 Working Tree↔Index / Index↔HEAD 两个边界，
`reset` 要守住 soft/mixed/hard 三棵树，而不是只追求“命令能跑”。

### 4.3 改协议 / 网络层

- 所有帧构造与解析集中在 [transport.c](src/core/transport.c)，
  命令层（clone/fetch/push）不碰字节流。改协议只动这一个文件。
- 调试协议问题的标准流程：
  1. `Invoke-WebRequest` 或 Python 直接打广告接口，确认服务器行为；
  2. Python 原样构造请求体，确认"协议本身的理解"对不对；
  3. 转储 mgit 实际发出的字节（临时 `fwrite` 到文件），与期望逐字节比；
  4. 绕过封装直连 `git http-backend` 看服务端 stderr。
- protocol v2 不属于当前教学目标；这里不作为待办项。

### 4.4 当前刻意保持的教学简化

当前阶段不继续横向扩功能。以下差异应理解为“明确的教学边界”，而不是
等待补齐的产品 backlog：

- stash 保留高层 push/pop/list/drop 语义，但不复制真实 Git 的 stash
  merge-commit/reflog 内部结构；
- merge/cherry-pick 冲突不实现真实 Index 的 stage 1/2/3 全套状态机；
- cherry-pick/revert 不实现 merge commit 的 `-m/--mainline`，因此明确拒绝，
  而不是猜一个 parent；
- pull 固定采用 fetch + merge，不实现配置驱动的 rebase/ff-only 策略；
- protocol v2、SSH、LFS、submodule 等不属于当前教学主线。

如果以后要扩展，也必须先回答：**它是否能显著帮助理解 Git？**
不能的话，不因为“真实 Git 有”就实现。

### 4.5 改动任何核心逻辑后的验证清单

1. 当前平台 clean build 通过；
2. 共享 Python 测试全绿，Windows/Linux CI 都通过；
3. 涉及存储格式的：把产物丢给真实 `git fsck` / `git log` 验证；
4. 涉及网络的：本地 `git_http_server.py` 起真服务器端到端跑一遍；
5. 边界场景重点看：**超过固定容量、空输入、重复执行**。
   commit DAG 的共享祖先遍历已经改为动态结构，新增代码不要重新引入静默固定上限。
