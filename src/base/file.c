#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(path) _mkdir(path)
#else
#include <unistd.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

int file_mkdir_p(const char *path) {
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    
    if (len == 0) return -1;
    
    /* 在 Windows 上，将 / 转换为 \ */
#ifdef _WIN32
    for (char *c = tmp; *c; c++) {
        if (*c == '/') *c = '\\';
    }
#endif
    
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') tmp[len - 1] = 0;
    
    /* 如果目录已存在，直接返回成功 */
    if (file_is_dir(tmp)) {
        return 0;
    }
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            if (!file_is_dir(tmp)) {
                if (MKDIR(tmp) != 0 && errno != EEXIST) {
                    return -1;
                }
            }
            *p = '\\';
        }
    }
    
    if (!file_is_dir(tmp)) {
        if (MKDIR(tmp) != 0 && errno != EEXIST) {
            return -1;
        }
    }
    
    return 0;
}

int file_exists(const char *path) {
    struct stat st;
#ifdef _WIN32
    /* Windows 上 stat 可能不支持 / 分隔符，转换为 \ */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *c = tmp; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    return stat(tmp, &st) == 0;
#else
    return stat(path, &st) == 0;
#endif
}

int file_is_dir(const char *path) {
    struct stat st;
#ifdef _WIN32
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *c = tmp; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    int ret = stat(tmp, &st);
#else
    int ret = stat(path, &st);
#endif
    if (ret != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

int file_read_all(const char *path, uint8_t **data, size_t *size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(fp);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(fsize + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    size_t read = fread(buf, 1, fsize, fp);
    fclose(fp);

    if ((long)read != fsize) {
        free(buf);
        return -1;
    }

    buf[fsize] = 0;  /* 方便当文本处理 */
    *data = buf;
    *size = fsize;
    return 0;
}

int file_write_all(const char *path, const uint8_t *data, size_t size) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);

    return written == size ? 0 : -1;
}

int file_read_line(const char *path, char *buf, size_t size) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    if (!fgets(buf, (int)size, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* 去掉末尾换行 */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
        buf[--len] = 0;
    }

    return 0;
}

int file_write_line(const char *path, const char *text) {
    /* binary mode: keep LF only (git refs must not have CR) */
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    fprintf(fp, "%s\n", text);
    fclose(fp);
    return 0;
}

void path_join(char *buf, size_t size, const char *base, const char *name) {
    size_t base_len = strlen(base);
    
    /* 检查 base 末尾是否有分隔符 */
    if (base_len > 0 && (base[base_len-1] == '/' || base[base_len-1] == '\\')) {
        snprintf(buf, size, "%s%s", base, name);
    } else {
        snprintf(buf, size, "%s/%s", base, name);
    }
}

int file_delete(const char *path) {
#ifdef _WIN32
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *c = tmp; *c; c++) {
        if (*c == '/') *c = '\\';
    }
    return DeleteFileA(tmp) ? 0 : -1;
#else
    return remove(path);
#endif
}
