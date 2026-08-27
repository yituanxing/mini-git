#include "../command.h"
#include "../core/object.h"
#include "../core/pack.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/*
 * mgit count-objects [-v]
 *
 * Count loose objects and report pack statistics.
 * Without -v: print "count size-in-KiB".
 * With -v: count/size/in-pack/packs/size-pack (sizes in KiB).
 */

/* Count loose objects and their total bytes on disk */
static int count_loose(ObjectStore *store, size_t *count, size_t *bytes) {
    *count = 0;
    *bytes = 0;
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
            if (strlen(f->d_name) != 38) continue;
            char path[512];
            path_join(path, sizeof(path), dirpath, f->d_name);
            uint8_t *data = NULL;
            size_t size = 0;
            if (file_read_all(path, &data, &size) == 0) {
                (*count)++;
                *bytes += size;
                free(data);
            }
        }
        closedir(sub);
    }
    closedir(d);
    return 0;
}

static void count_objects_help(void) {
    printf("usage: mgit count-objects [-v]\n\n");
    printf("Count unpacked objects and display pack statistics.\n\n");
    printf("  -v    verbose: show in-pack/packs/size-pack details\n");
}

static int count_objects_run(int argc, char **argv) {
    if (!file_exists(".git")) {
        mgit_error("not a git repository (run 'mgit init' first)");
        return -1;
    }

    ObjectStore *store = object_store_open(".git");
    if (!store) {
        mgit_error("failed to open object store");
        return -1;
    }

    /* Parse -v flag */
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
    }

    size_t loose_count = 0, loose_bytes = 0;
    count_loose(store, &loose_count, &loose_bytes);

    size_t num_packs = 0, pack_objects = 0, pack_bytes = 0;
    pack_stats(store, &num_packs, &pack_objects, &pack_bytes);

    if (!verbose) {
        /* Default: count and size in KiB */
        printf("%lu objects, %lu kilobytes\n",
               (unsigned long)loose_count,
               (unsigned long)((loose_bytes + 1023) / 1024));
    } else {
        printf("count: %lu\n", (unsigned long)loose_count);
        printf("size: %lu\n", (unsigned long)((loose_bytes + 1023) / 1024));
        printf("in-pack: %lu\n", (unsigned long)pack_objects);
        printf("packs: %lu\n", (unsigned long)num_packs);
        printf("size-pack: %lu\n", (unsigned long)((pack_bytes + 1023) / 1024));
        printf("prune-packable: 0\n");
        printf("garbage: 0\n");
    }

    object_store_close(store);
    return 0;
}

Command cmd_count_objects = {
    .name = "count-objects",
    .description = "Count unpacked objects and show pack statistics",
    .run = count_objects_run,
    .help = count_objects_help
};
