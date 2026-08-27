#include "../command.h"
#include "../core/object.h"
#include "../core/commit.h"
#include "../core/tree.h"
#include "../core/pack.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/*
 * mgit gc - Garbage Collection
 *
 * Git objects have two storage forms: loose objects (one object per file)
 * and packfiles (.pack + .idx). gc walks all objects reachable from
 * refs and reflog, packs them into a packfile, then deletes the
 * redundant loose objects.
 */

/* Hash list (dynamic array with dedup) */
typedef struct {
    Hash *items;
    size_t count;
    size_t capacity;
} HashList;

static void hashlist_free(HashList *list) {
    free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

/* Add a hash (skip if already present) */
static int hashlist_add(HashList *list, const Hash *hash) {
    for (size_t i = 0; i < list->count; i++) {
        if (hash_equal(&list->items[i], hash)) return 0;
    }
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 64;
        Hash *p = realloc(list->items, new_cap * sizeof(Hash));
        if (!p) return -1;
        list->items = p;
        list->capacity = new_cap;
    }
    list->items[list->count++] = *hash;
    return 0;
}

/* Queue helper for BFS over reachable objects */
static int queue_push(HashList *queue, const Hash *hash) {
    if (hash_is_zero(hash)) return 0;
    return hashlist_add(queue, hash);
}

/* Expand an object, pushing the objects it references */
static int expand_object(ObjectStore *store, const Object *obj, HashList *queue) {
    (void)store;
    if (obj->type == OBJ_COMMIT) {
        Commit commit;
        memset(&commit, 0, sizeof(commit));
        if (commit_parse(obj->data, obj->size, &commit) != 0) return -1;
        queue_push(queue, &commit.tree);
        for (int i = 0; i < commit.parent_count; i++) {
            queue_push(queue, &commit.parents[i]);
        }
        commit_free(&commit);
    } else if (obj->type == OBJ_TREE) {
        Tree tree;
        memset(&tree, 0, sizeof(tree));
        if (tree_parse(obj->data, obj->size, &tree) != 0) return -1;
        for (size_t i = 0; i < tree.count; i++) {
            queue_push(queue, &tree.entries[i].hash);
        }
        tree_free(&tree);
    } else if (obj->type == OBJ_TAG) {
        /* annotated tag: parse the "object <hex>" line（对象数据无 NUL 保证，用边界安全的前缀匹配） */
        if (obj->size >= 7 + 40 && memcmp(obj->data, "object ", 7) == 0) {
            char hex[HASH_HEX_SIZE];
            memcpy(hex, obj->data + 7, 40);
            hex[40] = 0;
            Hash target;
            if (hex_to_hash(hex, &target) == 0) {
                queue_push(queue, &target);
            }
        }
    }
    return 0;
}

/* BFS: collect every object reachable from the roots in queue */
static int walk_reachable(ObjectStore *store, HashList *seen, HashList *queue) {
    size_t head = 0;
    while (head < queue->count) {
        Hash h = queue->items[head++];
        if (hashlist_add(seen, &h) != 0) return -1;

        Object obj;
        if (object_store_read(store, &h, &obj) != 0) {
            char hex[HASH_HEX_SIZE];
            hash_to_hex(&h, hex);
            mgit_warning("gc: object %s missing, skipped", hex);
            continue;
        }
        if (expand_object(store, &obj, queue) != 0) {
            object_free(&obj);
            return -1;
        }
        object_free(&obj);
    }
    return 0;
}

/* Recursively collect hashes pointed to by refs under .git/refs */
static int collect_refs(const char *dir, HashList *queue) {
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[512];
        path_join(path, sizeof(path), dir, ent->d_name);

        if (file_is_dir(path)) {
            collect_refs(path, queue);
        } else {
            uint8_t *data = NULL;
            size_t size = 0;
            if (file_read_all(path, &data, &size) == 0) {
                /* content may be a hash or a symref ("ref: refs/...") */
                char hex[HASH_HEX_SIZE];
                size_t n = 0;
                while (n < 40 && n < size && data[n] != '\n' && data[n] != '\r') {
                    hex[n] = (char)data[n];
                    n++;
                }
                hex[n] = 0;
                free(data);

                if (n == 40) {
                    Hash h;
                    if (hex_to_hash(hex, &h) == 0 && !hash_is_zero(&h)) {
                        queue_push(queue, &h);
                    }
                }
            }
        }
    }
    closedir(d);
    return 0;
}

/* Collect old/new hashes from reflog so lost commits stay recoverable */
static int collect_reflog(HashList *queue) {
    uint8_t *data = NULL;
    size_t size = 0;
    if (file_read_all(".git/logs/HEAD", &data, &size) != 0) {
        return 0;
    }

    size_t pos = 0;
    while (pos < size) {
        size_t line_start = pos;
        while (pos < size && data[pos] != '\n') pos++;

        size_t line_len = pos - line_start;
        if (line_len >= 81) {
            Hash h;
            char hex[HASH_HEX_SIZE];

            memcpy(hex, data + line_start, 40);
            hex[40] = 0;
            if (hex_to_hash(hex, &h) == 0 && !hash_is_zero(&h)) {
                queue_push(queue, &h);
            }

            memcpy(hex, data + line_start + 41, 40);
            hex[40] = 0;
            if (hex_to_hash(hex, &h) == 0 && !hash_is_zero(&h)) {
                queue_push(queue, &h);
            }
        }
        pos++;
    }
    free(data);
    return 0;
}

/* Count loose objects on disk */
static size_t count_loose_objects(ObjectStore *store) {
    size_t total = 0;
    DIR *d = opendir(store->objects_dir);
    if (!d) return 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strlen(ent->d_name) != 2) continue;

        char dirpath[512];
        path_join(dirpath, sizeof(dirpath), store->objects_dir, ent->d_name);
        DIR *sub = opendir(dirpath);
        if (!sub) continue;

        struct dirent *f;
        while ((f = readdir(sub)) != NULL) {
            if (strlen(f->d_name) == 38) total++;
        }
        closedir(sub);
    }
    closedir(d);
    return total;
}

static void gc_help(void) {
    printf("usage: mgit gc\n\n");
    printf("Pack reachable objects and remove loose object files.\n\n");
    printf("gc does three things:\n");
    printf("  1. Find all reachable objects (refs + reflog as roots)\n");
    printf("  2. Write them into a packfile (.pack + .idx)\n");
    printf("  3. Delete the now-redundant loose objects\n");
}

static int gc_run(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (!file_exists(".git")) {
        mgit_error("not a git repository (run 'mgit init' first)");
        return -1;
    }

    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("failed to open object store");
        return -1;
    }

    HashList seen = {0};
    HashList queue = {0};
    int ret = -1;

    /* 1. Collect roots: all refs + reflog */
    if (collect_refs(".git/refs", &queue) != 0) goto cleanup;
    if (collect_reflog(&queue) != 0) goto cleanup;

    if (queue.count == 0) {
        printf("Nothing to do: no commits yet.\n");
        ret = 0;
        goto cleanup;
    }

    /* 2. Walk reachable objects */
    if (walk_reachable(store, &seen, &queue) != 0) goto cleanup;

    printf("Counting objects: %lu reachable, done.\n", (unsigned long)seen.count);

    /* 3. Write the packfile */
    size_t packed = 0;
    char pack_name[128];
    if (pack_write(store, seen.items, seen.count, &packed,
                   pack_name, sizeof(pack_name)) != 0) {
        mgit_error("failed to write packfile");
        goto cleanup;
    }
    printf("Writing objects: %lu into %s.pack\n",
           (unsigned long)packed, pack_name);

    /* 4. Delete packed loose objects */
    size_t before = count_loose_objects(store);
    size_t removed = 0;
    for (size_t i = 0; i < seen.count; i++) {
        char path[512];
        object_path(store, &seen.items[i], path, sizeof(path));
        if (file_exists(path) && file_delete(path) == 0) {
            removed++;
        }
    }
    printf("Removed %lu loose objects (before: %lu, after: %lu)\n",
           (unsigned long)removed, (unsigned long)before,
           (unsigned long)(before - removed));

    ret = 0;

cleanup:
    hashlist_free(&seen);
    hashlist_free(&queue);
    object_store_close(store);
    return ret;
}

Command cmd_gc = {
    .name = "gc",
    .description = "Pack reachable objects and clean up loose ones",
    .run = gc_run,
    .help = gc_help
};
