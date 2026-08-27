#ifndef MGIT_ERROR_H
#define MGIT_ERROR_H

#include <stdio.h>

/*
 * 错误处理
 * 简单的错误报告机制
 */

/* 错误输出到 stderr */
#define mgit_error(fmt, ...) \
    fprintf(stderr, "error: " fmt "\n", ##__VA_ARGS__)

#define mgit_warning(fmt, ...) \
    fprintf(stderr, "warning: " fmt "\n", ##__VA_ARGS__)

#define mgit_fatal(fmt, ...) \
    do { \
        fprintf(stderr, "fatal: " fmt "\n", ##__VA_ARGS__); \
        return -1; \
    } while (0)

/* 调试输出（可通过编译开关控制） */
#ifdef MGIT_DEBUG
#define mgit_debug(fmt, ...) \
    fprintf(stderr, "debug: " fmt "\n", ##__VA_ARGS__)
#else
#define mgit_debug(fmt, ...) ((void)0)
#endif

#endif /* MGIT_ERROR_H */
