#ifndef MGIT_FILE_H
#define MGIT_FILE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 文件操作工具
 */

/*
 * 创建目录（递归）
 * @param path  目录路径
 * @return      0 成功，-1 失败
 */
int file_mkdir_p(const char *path);

/*
 * 检查文件是否存在
 * @param path  文件路径
 * @return      1 存在，0 不存在
 */
int file_exists(const char *path);

/*
 * 检查是否为目录
 * @param path  路径
 * @return      1 是目录，0 不是
 */
int file_is_dir(const char *path);

/*
 * 读取整个文件内容
 * @param path      文件路径
 * @param data      输出：文件内容（调用者释放）
 * @param size      输出：文件大小
 * @return          0 成功，-1 失败
 */
int file_read_all(const char *path, uint8_t **data, size_t *size);

/*
 * 写入文件（覆盖）
 * @param path  文件路径
 * @param data  数据
 * @param size  数据大小
 * @return      0 成功，-1 失败
 */
int file_write_all(const char *path, const uint8_t *data, size_t size);

/*
 * 读取文本文件的一行
 * @param path  文件路径
 * @param buf   缓冲区
 * @param size  缓冲区大小
 * @return      0 成功，-1 失败
 */
int file_read_line(const char *path, char *buf, size_t size);

/*
 * 写入文本文件（覆盖，自动添加换行）
 * @param path  文件路径
 * @param text  文本内容
 * @return      0 成功，-1 失败
 */
int file_write_line(const char *path, const char *text);

/*
 * 拼接路径
 * @param buf   输出缓冲区
 * @param size  缓冲区大小
 * @param base  基础路径
 * @param name  要追加的路径
 */
void path_join(char *buf, size_t size, const char *base, const char *name);

/*
 * 删除文件
 * @param path  文件路径
 * @return      0 成功，-1 失败
 */
int file_delete(const char *path);

#endif /* MGIT_FILE_H */
