#include "tree.h"
#include "index.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Tree *tree_new(void) {
    Tree *tree = (Tree *)malloc(sizeof(Tree));
    if (!tree) return NULL;

    tree->entries = NULL;
    tree->count = 0;
    tree->capacity = 0;
    return tree;
}

void tree_free(Tree *tree) {
    if (tree) {
        for (size_t i = 0; i < tree->count; i++) {
            free(tree->entries[i].name);
        }
        free(tree->entries);
        tree->entries = NULL;
        tree->count = 0;
        tree->capacity = 0;
        /* 注意：不 free(tree) 本身，因为调用者可能用栈分配 */
    }
}

/*
 * 解析 Tree 对象
 * 
 * 格式: "<mode> <name>\0<20-byte-sha1>"
 * 例如: "100644 README.md\0<20 bytes>"
 */
int tree_parse(const uint8_t *data, size_t size, Tree *tree) {
    size_t pos = 0;

    while (pos < size) {
        /* 找到空格，分离 mode */
        size_t space_pos = pos;
        while (space_pos < size && data[space_pos] != ' ') {
            space_pos++;
        }
        if (space_pos >= size) {
            mgit_error("invalid tree format: no space found");
            return -1;
        }

        /* 提取 mode */
        size_t mode_len = space_pos - pos;
        if (mode_len >= 8) {
            mgit_error("invalid tree mode length");
            return -1;
        }

        /* 找到 NUL，分离 name */
        size_t null_pos = space_pos + 1;
        while (null_pos < size && data[null_pos] != 0) {
            null_pos++;
        }
        if (null_pos >= size) {
            mgit_error("invalid tree format: no NUL found");
            return -1;
        }

        /* 提取 name */
        size_t name_len = null_pos - space_pos - 1;
        char *name = (char *)malloc(name_len + 1);
        if (!name) return -1;
        memcpy(name, data + space_pos + 1, name_len);
        name[name_len] = 0;

        /* 提取 hash (20 bytes) */
        if (null_pos + 1 + HASH_SIZE > size) {
            mgit_error("invalid tree format: not enough data for hash");
            free(name);
            return -1;
        }

        /* 添加条目 */
        char mode[8];
        memcpy(mode, data + pos, mode_len);
        mode[mode_len] = 0;

        Hash hash;
        memcpy(hash.bytes, data + null_pos + 1, HASH_SIZE);

        if (tree_add_entry(tree, mode, name, &hash) != 0) {
            free(name);
            return -1;
        }
        free(name);

        /* 移动到下一个条目 */
        pos = null_pos + 1 + HASH_SIZE;
    }

    return 0;
}

int tree_serialize(const Tree *tree, uint8_t **data, size_t *size) {
    /* 计算总大小 */
    size_t total = 0;
    for (size_t i = 0; i < tree->count; i++) {
        /* "<mode> <name>\0<20-byte-hash>" */
        total += strlen(tree->entries[i].mode) + 1;  /* mode + space */
        total += strlen(tree->entries[i].name) + 1;  /* name + NUL */
        total += HASH_SIZE;                          /* hash */
    }

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    size_t pos = 0;
    for (size_t i = 0; i < tree->count; i++) {
        /* mode + space */
        size_t mode_len = strlen(tree->entries[i].mode);
        memcpy(buf + pos, tree->entries[i].mode, mode_len);
        pos += mode_len;
        buf[pos++] = ' ';

        /* name + NUL */
        size_t name_len = strlen(tree->entries[i].name);
        memcpy(buf + pos, tree->entries[i].name, name_len);
        pos += name_len;
        buf[pos++] = 0;

        /* hash */
        memcpy(buf + pos, tree->entries[i].hash.bytes, HASH_SIZE);
        pos += HASH_SIZE;
    }

    *data = buf;
    *size = total;
    return 0;
}

int tree_add_entry(Tree *tree, const char *mode, const char *name, const Hash *hash) {
    /* 扩展容量 */
    if (tree->count >= tree->capacity) {
        size_t new_cap = tree->capacity == 0 ? 8 : tree->capacity * 2;
        TreeEntry *new_entries = (TreeEntry *)realloc(tree->entries, new_cap * sizeof(TreeEntry));
        if (!new_entries) return -1;
        tree->entries = new_entries;
        tree->capacity = new_cap;
    }

    TreeEntry *entry = &tree->entries[tree->count];
    strncpy(entry->mode, mode, 7);
    entry->mode[7] = 0;
    
    entry->name = (char *)malloc(strlen(name) + 1);
    if (!entry->name) return -1;
    strcpy(entry->name, name);
    
    entry->hash = *hash;
    entry->type = (strcmp(mode, "40000") == 0 || strcmp(mode, "040000") == 0) 
                  ? TREE_ENTRY_TREE : TREE_ENTRY_BLOB;

    tree->count++;
    return 0;
}

int tree_write(ObjectStore *store, Tree *tree, Hash *out) {
    uint8_t *data;
    size_t size;

    if (tree_serialize(tree, &data, &size) != 0) {
        return -1;
    }

    int ret = object_store_write(store, OBJ_TREE, data, size, out);
    free(data);
    return ret;
}

/* 递归展开 tree 为扁平文件列表（内部实现） */
static void flatten_rec(ObjectStore *store, const Tree *tree, const char *prefix,
                        TreeFlatEntry **arr, size_t *count, size_t *cap) {
    for (size_t i = 0; i < tree->count; i++) {
        const TreeEntry *e = &tree->entries[i];
        char full[1024];
        if (prefix[0]) {
            snprintf(full, sizeof(full), "%s/%s", prefix, e->name);
        } else {
            snprintf(full, sizeof(full), "%s", e->name);
        }

        if (e->type == TREE_ENTRY_TREE) {
            Object obj;
            if (object_store_read(store, &e->hash, &obj) != 0) continue;
            if (obj.type != OBJ_TREE) { object_free(&obj); continue; }
            Tree sub = {0};
            if (tree_parse(obj.data, obj.size, &sub) == 0) {
                flatten_rec(store, &sub, full, arr, count, cap);
            }
            tree_free(&sub);
            object_free(&obj);
        } else {
            /* 动态扩容 */
            if (*count >= *cap) {
                size_t new_cap = *cap == 0 ? 64 : *cap * 2;
                TreeFlatEntry *na = (TreeFlatEntry *)realloc(*arr, sizeof(TreeFlatEntry) * new_cap);
                if (!na) return;
                *arr = na;
                *cap = new_cap;
            }
            TreeFlatEntry *fe = &(*arr)[(*count)++];
            snprintf(fe->path, sizeof(fe->path), "%s", full);
            fe->hash = e->hash;
        }
    }
}

int tree_flatten(ObjectStore *store, const Tree *tree, TreeFlatEntry **out, size_t *count) {
    TreeFlatEntry *arr = NULL;
    size_t cap = 0;
    *count = 0;
    flatten_rec(store, tree, "", &arr, count, &cap);
    *out = arr;
    return 0;
}

TreeFlatEntry *tree_flat_find(TreeFlatEntry *entries, size_t count, const char *path) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].path, path) == 0) return &entries[i];
    }
    return NULL;
}

int tree_rebuild_index(ObjectStore *store, Index *idx, const Tree *tree) {
    TreeFlatEntry *entries;
    size_t count;
    if (tree_flatten(store, tree, &entries, &count) != 0) return -1;

    /* 清空 Index（释放 name） */
    for (size_t i = 0; i < idx->count; i++) {
        free(idx->entries[i].name);
    }
    idx->count = 0;
    idx->dirty = 1;

    /* 重建 Index（不碰工作区文件） */
    for (size_t i = 0; i < count; i++) {
        if (index_add(idx, entries[i].path, &entries[i].hash, 0100644) != 0) {
            free(entries);
            return -1;
        }
    }

    free(entries);
    return 0;
}

int tree_restore_worktree(ObjectStore *store, Index *idx, const Tree *tree) {
    TreeFlatEntry *entries;
    size_t count;
    if (tree_flatten(store, tree, &entries, &count) != 0) return -1;

    /* 1. 删除 tree 中不存在的已跟踪文件 */
    for (size_t i = 0; i < idx->count; i++) {
        if (!tree_flat_find(entries, count, idx->entries[i].name)) {
            file_delete(idx->entries[i].name);
        }
    }

    /* 2. 清空 Index（释放 name） */
    for (size_t i = 0; i < idx->count; i++) {
        free(idx->entries[i].name);
    }
    idx->count = 0;
    idx->dirty = 1;

    /* 3. 写出文件并重建 Index */
    for (size_t i = 0; i < count; i++) {
        /* 确保父目录存在（子目录支持） */
        char dir[1024];
        const char *slash = strrchr(entries[i].path, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - entries[i].path);
            if (dlen < sizeof(dir)) {
                memcpy(dir, entries[i].path, dlen);
                dir[dlen] = 0;
                file_mkdir_p(dir);
            }
        }

        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &entries[i].hash, &obj) == 0) {
            if (obj.type == OBJ_BLOB) {
                file_write_all(entries[i].path, obj.data, obj.size);
            }
            object_free(&obj);
        }
        if (index_add(idx, entries[i].path, &entries[i].hash, 0100644) != 0) {
            free(entries);
            return -1;
        }
    }

    free(entries);
    return 0;
}
