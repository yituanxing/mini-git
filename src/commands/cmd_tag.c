#include "../command.h"
#include "../core/ref.h"
#include "../core/object.h"
#include "../base/hash.h"
#include "../base/file.h"
#include "../base/error.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/*
 * mgit tag [<name> | -l | -d <name>]
 * 
 * 标签管理
 * 
 * 标签 vs 分支：
 * - 分支是可移动的指针，随 commit 前进
 * - 标签是固定的指针，永远指向同一个 commit
 * - 标签存储在 refs/tags/ 而非 refs/heads/
 */

static void tag_help(void) {
    printf("usage: mgit tag [<name> | -l | -d <name>]\n\n");
    printf("Create, list, or delete tags.\n\n");
    printf("Options:\n");
    printf("    (no args)    List all tags\n");
    printf("    -l           List all tags\n");
    printf("    <name>       Create a tag at HEAD\n");
    printf("    -d <name>    Delete a tag\n");
}

static int tag_run(int argc, char **argv) {
    RefManager *refs = ref_manager_open(".git");
    if (!refs) {
        mgit_error("not a git repository");
        return -1;
    }

    /* 无参数或 -l：列出标签 */
    if (argc < 2 || strcmp(argv[1], "-l") == 0) {
        char path[1024];
        path_join(path, sizeof(path), ".git", "refs/tags");

        if (!file_is_dir(path)) {
            printf("(no tags)\n");
            ref_manager_close(refs);
            return 0;
        }

        /* 遍历 refs/tags/ 目录 */
#ifdef _WIN32
        WIN32_FIND_DATAA ffd;
        char search_path[1024];
        snprintf(search_path, sizeof(search_path), "%s/*", path);
        HANDLE hFind = FindFirstFileA(search_path, &ffd);
        if (hFind == INVALID_HANDLE_VALUE) {
            printf("(no tags)\n");
            ref_manager_close(refs);
            return 0;
        }
        int count = 0;
        do {
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char ref_path[1024];
            snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", ffd.cFileName);
            Hash hash;
            if (ref_resolve(refs, ref_path, &hash) == 0) {
                char hex[HASH_HEX_SIZE];
                hash_to_hex(&hash, hex);
                printf("%-20s %s\n", ffd.cFileName, hex);
                count++;
            }
        } while (FindNextFileA(hFind, &ffd) != 0);
        FindClose(hFind);
        if (count == 0) printf("(no tags)\n");
#else
        DIR *dir = opendir(path);
        if (!dir) {
            printf("(no tags)\n");
            ref_manager_close(refs);
            return 0;
        }
        int count = 0;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char ref_path[1024];
            snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", entry->d_name);
            Hash hash;
            if (ref_resolve(refs, ref_path, &hash) == 0) {
                char hex[HASH_HEX_SIZE];
                hash_to_hex(&hash, hex);
                printf("%-20s %s\n", entry->d_name, hex);
                count++;
            }
        }
        closedir(dir);
        if (count == 0) printf("(no tags)\n");
#endif
        ref_manager_close(refs);
        return 0;
    }

    /* 删除标签 */
    if (strcmp(argv[1], "-d") == 0) {
        if (argc < 3) {
            mgit_error("tag name required");
            ref_manager_close(refs);
            return -1;
        }
        char ref_path[1024];
        snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", argv[2]);
        
        char full_path[1024];
        path_join(full_path, sizeof(full_path), ".git", ref_path);
        if (!file_exists(full_path)) {
            mgit_error("tag '%s' not found", argv[2]);
            ref_manager_close(refs);
            return -1;
        }

        if (file_delete(full_path) != 0) {
            mgit_error("failed to delete tag '%s'", argv[2]);
            ref_manager_close(refs);
            return -1;
        }
        printf("Deleted tag '%s'\n", argv[2]);
        ref_manager_close(refs);
        return 0;
    }

    /* 创建标签 */
    const char *tag_name = argv[1];

    /* 拒绝覆盖已存在的标签（与真实 git 一致） */
    {
        char check_path[1024];
        snprintf(check_path, sizeof(check_path), "refs/tags/%s", tag_name);
        Hash existing;
        if (ref_resolve_quiet(refs, check_path, &existing) == 0) {
            mgit_error("tag '%s' already exists", tag_name);
            ref_manager_close(refs);
            return -1;
        }
    }

    /* 获取 HEAD commit */
    Hash head_hash;
    if (ref_resolve_head(refs, &head_hash) != 0) {
        mgit_error("cannot create tag: no commits yet");
        ref_manager_close(refs);
        return -1;
    }

    /* 写入 refs/tags/<name> */
    char ref_path[1024];
    snprintf(ref_path, sizeof(ref_path), "refs/tags/%s", tag_name);
    if (ref_update(refs, ref_path, &head_hash) != 0) {
        mgit_error("failed to create tag '%s'", tag_name);
        ref_manager_close(refs);
        return -1;
    }

    char hex[HASH_HEX_SIZE];
    hash_to_hex(&head_hash, hex);
    printf("Created tag '%s' at %s\n", tag_name, hex);

    ref_manager_close(refs);
    return 0;
}

Command cmd_tag = {
    .name = "tag",
    .description = "Create, list, or delete tags",
    .run = tag_run,
    .help = tag_help
};
