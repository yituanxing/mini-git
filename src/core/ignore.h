#ifndef MGIT_IGNORE_H
#define MGIT_IGNORE_H

/*
 * .gitignore 支持
 *
 * 支持的模式语法（真实 Git 的子集）：
 * - 空行与 # 开头的注释
 * - ! 前缀：取反（重新包含）
 * - 末尾 / ：只匹配目录
 * - 开头 / 或含中间 / ：锚定到仓库根
 * - * 与 ? 通配符（不跨 /）
 * - 不含 / 的模式匹配任意层级的文件名/目录名
 *
 * 规则：按行顺序匹配，后面的规则覆盖前面的；
 * 父目录被忽略后，其下文件不可被 ! 重新包含。
 */

#define IGNORE_MAX_PATTERNS 128
#define IGNORE_PATTERN_LEN  256

typedef struct {
    int count;
    char patterns[IGNORE_MAX_PATTERNS][IGNORE_PATTERN_LEN];
} IgnoreList;

/* 读取仓库根目录的 .gitignore（不存在则为空列表） */
int ignore_load(IgnoreList *list);

/*
 * 判断路径是否被忽略
 * @param rel_path  相对仓库根的路径（正斜杠）
 * @param is_dir    是否为目录
 * @return 1 忽略，0 不忽略
 */
int ignore_is_ignored(const IgnoreList *list, const char *rel_path, int is_dir);

#endif /* MGIT_IGNORE_H */
