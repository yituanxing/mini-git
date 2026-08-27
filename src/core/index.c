#include "index.h"
#include "object.h"
#include "tree.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Index 文件签名 */
#define INDEX_SIGNATURE "DIRC"
#define INDEX_VERSION 2

/* 大端序读写辅助函数 */
static uint32_t read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint16_t read_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

/* 计算条目填充后的大小 (Git 规范: 条目大小 = 62 + name_len + 1 个 NUL,
 * 填充到 8 字节对齐。注意必须把结尾 NUL 计入长度，否则与真实 git 的
 * 边界差 1 字节，逐条累积后真实 git 会报 "unknown index entry format") */
static size_t entry_padded_size(size_t name_len) {
    size_t entry_len = 62 + name_len + 1;
    return (entry_len + 7) & ~7;
}

Index *index_open(const char *git_dir) {
    Index *idx = (Index *)calloc(1, sizeof(Index));
    if (!idx) return NULL;

    idx->path = (char *)malloc(strlen(git_dir) + 10);
    if (!idx->path) {
        free(idx);
        return NULL;
    }
    path_join(idx->path, strlen(git_dir) + 10, git_dir, "index");

    idx->entries = NULL;
    idx->count = 0;
    idx->capacity = 0;
    idx->dirty = 0;

    /* 尝试读取已有 index，不存在则创建空的 */
    if (file_exists(idx->path)) {
        if (index_read(idx) != 0) {
            /* 读取失败，使用空 index */
            idx->count = 0;
        }
    }

    return idx;
}

void index_close(Index *idx) {
    if (idx) {
        /* 如果有未保存的修改，自动保存 */
        if (idx->dirty) {
            index_write(idx);
        }
        for (size_t i = 0; i < idx->count; i++) {
            free(idx->entries[i].name);
        }
        free(idx->entries);
        free(idx->path);
        free(idx);
    }
}

/*
 * 读取 Index 文件
 * 
 * 格式：
 * - 12 字节头部: "DIRC" + version(4) + count(4)
 * - 每个条目: 32字节stat + 20字节hash + 2字节flags + name(填充到8字节对齐)
 * - 20 字节尾部校验和
 */
int index_read(Index *idx) {
    uint8_t *data;
    size_t size;

    if (file_read_all(idx->path, &data, &size) != 0) {
        return -1;
    }

    /* 检查最小长度 */
    if (size < 12) {
        free(data);
        mgit_error("index file too short");
        return -1;
    }

    /* 验证签名 */
    if (memcmp(data, INDEX_SIGNATURE, 4) != 0) {
        free(data);
        mgit_error("invalid index signature");
        return -1;
    }

    /* 读取版本和条目数 */
    uint32_t version = read_u32(data + 4);
    uint32_t count = read_u32(data + 8);

    if (version != 2) {
        mgit_error("unsupported index version: %u", version);
        free(data);
        return -1;
    }

    /* 清除旧条目 */
    for (size_t i = 0; i < idx->count; i++) {
        free(idx->entries[i].name);
    }
    free(idx->entries);

    /* 分配新条目 */
    idx->capacity = count > 0 ? count : 8;
    idx->entries = (IndexEntry *)calloc(idx->capacity, sizeof(IndexEntry));
    idx->count = 0;

    if (count > 0 && !idx->entries) {
        free(data);
        return -1;
    }

    /* 解析条目 */
    size_t offset = 12;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry_start = offset;
        /* 每个条目最小长度: 32 + 20 + 2 + 1 = 55 字节 */
        if (offset + 55 > size) {
            mgit_error("index file truncated at entry %u", i);
            break;
        }

        IndexEntry *entry = &idx->entries[idx->count];

        /* 32 字节 stat 信息 */
        entry->ctime_sec = read_u32(data + offset);      offset += 4;
        offset += 4;  /* ctime nanoseconds */
        entry->mtime_sec = read_u32(data + offset);      offset += 4;
        offset += 4;  /* mtime nanoseconds */
        entry->dev = read_u32(data + offset);            offset += 4;
        entry->ino = read_u32(data + offset);            offset += 4;
        entry->mode = read_u32(data + offset);           offset += 4;
        entry->uid = read_u32(data + offset);            offset += 4;
        entry->gid = read_u32(data + offset);            offset += 4;
        entry->size = read_u32(data + offset);           offset += 4;

        /* 20 字节 hash */
        memcpy(entry->hash.bytes, data + offset, HASH_SIZE);
        offset += HASH_SIZE;

        /* 2 字节 flags */
        uint16_t flags = read_u16(data + offset);        offset += 2;
        uint16_t name_len = flags & 0x0FFF;

        /* 读取文件名 */
        if (offset + name_len > size) {
            mgit_error("index entry name truncated");
            break;
        }

        entry->name = (char *)malloc(name_len + 1);
        if (!entry->name) {
            free(data);
            return -1;
        }
        memcpy(entry->name, data + offset, name_len);
        entry->name[name_len] = 0;

        /* 跳到下一个条目（从条目起始 + 填充后大小） */
        offset = entry_start + entry_padded_size(name_len);

        idx->count++;
    }

    free(data);
    idx->dirty = 0;
    return 0;
}

/* 条目按路径名字节序比较（真实 git 要求 index 条目有序，
 * 否则报 "unordered stage entries"） */
static int entry_name_cmp(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->name, eb->name);
}

/*
 * 保存 Index 文件
 */
int index_write(Index *idx) {
    /* 写出前统一排序，保证与真实 git 兼容（任何命令写入都安全） */
    if (idx->count > 1) {
        qsort(idx->entries, idx->count, sizeof(IndexEntry), entry_name_cmp);
    }

    /* 计算总大小 */
    size_t total = 12;  /* header */
    for (size_t i = 0; i < idx->count; i++) {
        size_t name_len = strlen(idx->entries[i].name);
        total += entry_padded_size(name_len);
    }
    total += 20;  /* trailing SHA-1 */

    uint8_t *data = (uint8_t *)calloc(1, total);
    if (!data) return -1;

    /* 写入头部 */
    memcpy(data, INDEX_SIGNATURE, 4);
    write_u32(data + 4, INDEX_VERSION);
    write_u32(data + 8, (uint32_t)idx->count);

    /* 写入条目 */
    size_t offset = 12;
    for (size_t i = 0; i < idx->count; i++) {
        size_t entry_start = offset;
        IndexEntry *entry = &idx->entries[i];
        size_t name_len = strlen(entry->name);

        /* 32 字节 stat */
        write_u32(data + offset, entry->ctime_sec);   offset += 4;
        write_u32(data + offset, 0);                   offset += 4;  /* nsec */
        write_u32(data + offset, entry->mtime_sec);   offset += 4;
        write_u32(data + offset, 0);                   offset += 4;  /* nsec */
        write_u32(data + offset, entry->dev);          offset += 4;
        write_u32(data + offset, entry->ino);          offset += 4;
        write_u32(data + offset, entry->mode);         offset += 4;
        write_u32(data + offset, entry->uid);          offset += 4;
        write_u32(data + offset, entry->gid);          offset += 4;
        write_u32(data + offset, entry->size);         offset += 4;

        /* 20 字节 hash */
        memcpy(data + offset, entry->hash.bytes, HASH_SIZE);
        offset += HASH_SIZE;

        /* 2 字节 flags: name_len 在低 12 位 */
        uint16_t flags = (uint16_t)(name_len & 0x0FFF);
        write_u16(data + offset, flags);
        offset += 2;

        /* name + NUL + padding 到 8 字节对齐 */
        memcpy(data + offset, entry->name, name_len);
        /* 跳到下一个条目起始位置 */
        offset = entry_start + entry_padded_size(name_len);
    }

    /* 计算 SHA-1 校验和（不含最后 20 字节） */
    Hash checksum;
    hash_data(data, total - 20, &checksum);
    memcpy(data + total - 20, checksum.bytes, HASH_SIZE);

    /* 写入文件 */
    int ret = file_write_all(idx->path, data, total);
    free(data);

    if (ret == 0) {
        idx->dirty = 0;
    }
    return ret;
}

int index_add(Index *idx, const char *path, const Hash *hash, uint32_t mode) {
    /* 查找是否已存在 */
    for (size_t i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].name, path) == 0) {
            /* 更新已有条目 */
            idx->entries[i].hash = *hash;
            idx->entries[i].mode = mode;
            idx->dirty = 1;
            return 0;
        }
    }

    /* 添加新条目 */
    if (idx->count >= idx->capacity) {
        size_t new_cap = idx->capacity == 0 ? 8 : idx->capacity * 2;
        IndexEntry *new_entries = (IndexEntry *)realloc(idx->entries, 
                                                        new_cap * sizeof(IndexEntry));
        if (!new_entries) return -1;
        idx->entries = new_entries;
        idx->capacity = new_cap;
    }

    IndexEntry *entry = &idx->entries[idx->count];
    memset(entry, 0, sizeof(IndexEntry));

    entry->name = (char *)malloc(strlen(path) + 1);
    if (!entry->name) return -1;
    strcpy(entry->name, path);

    entry->hash = *hash;
    entry->mode = mode;

    /* 获取文件 stat 信息 */
    struct stat st;
    if (stat(path, &st) == 0) {
        entry->ctime_sec = (uint32_t)st.st_ctime;
        entry->mtime_sec = (uint32_t)st.st_mtime;
        entry->dev = (uint32_t)st.st_dev;
        entry->ino = (uint32_t)st.st_ino;
        entry->uid = (uint32_t)st.st_uid;
        entry->gid = (uint32_t)st.st_gid;
        entry->size = (uint32_t)st.st_size;
    }

    idx->count++;
    idx->dirty = 1;
    return 0;
}

int index_remove(Index *idx, const char *path) {
    for (size_t i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].name, path) == 0) {
            free(idx->entries[i].name);
            /* 移动后面的条目 */
            for (size_t j = i; j < idx->count - 1; j++) {
                idx->entries[j] = idx->entries[j + 1];
            }
            idx->count--;
            idx->dirty = 1;
            return 0;
        }
    }
    return -1;
}

IndexEntry *index_find(Index *idx, const char *path) {
    for (size_t i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].name, path) == 0) {
            return &idx->entries[i];
        }
    }
    return NULL;
}

/*
 * 从 Index 构建（嵌套）tree
 *
 * Index 是扁平的路径列表（如 "src/main.c"），
 * Git 的 tree 是嵌套的：根 tree 里 src 是一个子 tree 条目（mode 40000）。
 * 按路径分层递归构建，并按 Git 规则排序（目录视为带 '/' 后缀参与比较）。
 */

/* Git tree 条目排序比较：目录名视为带尾部 '/' 参与比较 */
static int git_name_cmp(const char *na, int ta, const char *nb, int tb) {
    size_t i = 0;
    while (na[i] && nb[i]) {
        if (na[i] != nb[i]) return (unsigned char)na[i] - (unsigned char)nb[i];
        i++;
    }
    unsigned char ca = na[i] ? (unsigned char)na[i] : (ta ? '/' : 0);
    unsigned char cb = nb[i] ? (unsigned char)nb[i] : (tb ? '/' : 0);
    return ca - cb;
}

/* 当前层级的一个条目 */
typedef struct {
    char name[256];      /* 本层名字（不含路径） */
    Hash hash;
    uint32_t mode;
    int is_tree;
} BuildEntry;

static int build_tree_level(Index *idx, ObjectStore *store, const char *prefix, Hash *out) {
    size_t plen = strlen(prefix);

    BuildEntry *entries = (BuildEntry *)malloc(sizeof(BuildEntry) * (idx->count + 1));
    if (!entries) return -1;
    size_t count = 0;

    /* 1. 收集本层条目：直接子文件 + 子目录（去重） */
    for (size_t i = 0; i < idx->count; i++) {
        const char *path = idx->entries[i].name;
        if (plen > 0 && strncmp(path, prefix, plen) != 0) continue;

        const char *rest = path + plen;
        const char *slash = strchr(rest, '/');

        if (!slash) {
            /* 直接子文件 */
            BuildEntry *be = &entries[count++];
            snprintf(be->name, sizeof(be->name), "%s", rest);
            be->hash = idx->entries[i].hash;
            be->mode = idx->entries[i].mode;
            be->is_tree = 0;
        } else {
            /* 子目录：检查是否已记录 */
            size_t dlen = (size_t)(slash - rest);
            int found = 0;
            for (size_t j = 0; j < count; j++) {
                if (entries[j].is_tree &&
                    strlen(entries[j].name) == dlen &&
                    strncmp(entries[j].name, rest, dlen) == 0) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                BuildEntry *be = &entries[count++];
                snprintf(be->name, sizeof(be->name), "%.*s", (int)dlen, rest);
                memset(&be->hash, 0, sizeof(Hash));
                be->mode = 0040000;
                be->is_tree = 1;
            }
        }
    }

    /* 2. 递归构建子 tree */
    for (size_t i = 0; i < count; i++) {
        if (!entries[i].is_tree) continue;

        char sub_prefix[1024];
        snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, entries[i].name);

        Hash sub_hash;
        if (build_tree_level(idx, store, sub_prefix, &sub_hash) != 0) {
            free(entries);
            return -1;
        }
        entries[i].hash = sub_hash;
    }

    /* 3. 按 Git 规则排序（插入排序，条目数通常不多） */
    for (size_t i = 1; i < count; i++) {
        BuildEntry key = entries[i];
        size_t j = i;
        while (j > 0 &&
               git_name_cmp(entries[j-1].name, entries[j-1].is_tree,
                            key.name, key.is_tree) > 0) {
            entries[j] = entries[j-1];
            j--;
        }
        entries[j] = key;
    }

    /* 4. 写入 tree */
    Tree *tree = tree_new();
    if (!tree) { free(entries); return -1; }

    for (size_t i = 0; i < count; i++) {
        BuildEntry *be = &entries[i];
        char mode_str[8];
        if (be->is_tree) {
            snprintf(mode_str, sizeof(mode_str), "40000");
        } else {
            snprintf(mode_str, sizeof(mode_str), "%o", be->mode & 0xFFFF);
        }
        tree_add_entry(tree, mode_str, be->name, &be->hash);
    }

    int ret = tree_write(store, tree, out);
    tree_free(tree);
    free(tree);
    free(entries);
    return ret;
}

int index_write_tree(Index *idx, void *store_ptr, Hash *out) {
    ObjectStore *store = (ObjectStore *)store_ptr;
    return build_tree_level(idx, store, "", out);
}
