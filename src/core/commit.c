#include "commit.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

Commit *commit_new(void) {
    Commit *commit = (Commit *)calloc(1, sizeof(Commit));
    return commit;
}

void commit_free(Commit *commit) {
    if (commit) {
        free(commit->author.name);
        free(commit->author.email);
        free(commit->committer.name);
        free(commit->committer.email);
        free(commit->message);
        commit->author.name = NULL;
        commit->author.email = NULL;
        commit->committer.name = NULL;
        commit->committer.email = NULL;
        commit->message = NULL;
        /* 注意：不 free(commit) 本身，因为调用者可能用栈分配 */
    }
}

/* 解析签名行: "Name <email> timestamp timezone" */
static int parse_signature(const char *line, Signature *sig) {
    /* 找到 < 和 > */
    const char *lt = strchr(line, '<');
    const char *gt = strchr(line, '>');
    if (!lt || !gt || gt <= lt) return -1;

    /* 提取 name */
    size_t name_len = lt - line;
    while (name_len > 0 && line[name_len - 1] == ' ') name_len--;
    sig->name = (char *)malloc(name_len + 1);
    if (!sig->name) return -1;
    memcpy(sig->name, line, name_len);
    sig->name[name_len] = 0;

    /* 提取 email */
    size_t email_len = gt - lt - 1;
    sig->email = (char *)malloc(email_len + 1);
    if (!sig->email) return -1;
    memcpy(sig->email, lt + 1, email_len);
    sig->email[email_len] = 0;

    /* 提取 timestamp 和 timezone */
    const char *rest = gt + 2;
    sig->timestamp = (time_t)strtoul(rest, NULL, 10);
    
    /* 找到 timezone */
    const char *tz_start = strchr(rest, ' ');
    if (tz_start) {
        tz_start++;
        strncpy(sig->tz, tz_start, 5);
        sig->tz[5] = 0;
        /* 去掉换行 */
        char *nl = strchr(sig->tz, '\n');
        if (nl) *nl = 0;
    } else {
        strcpy(sig->tz, "+0000");
    }

    return 0;
}

int commit_parse(const uint8_t *data, size_t size, Commit *commit) {
    char *str = (char *)malloc(size + 1);
    if (!str) return -1;
    memcpy(str, data, size);
    str[size] = 0;

    char *line = str;
    char *next;
    int in_message = 0;
    char *message_start = NULL;

    while (line && *line) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;

        if (in_message) {
            /* 已经在消息部分 */
            if (!message_start) message_start = line;
        } else if (strncmp(line, "tree ", 5) == 0) {
            hex_to_hash(line + 5, &commit->tree);
        } else if (strncmp(line, "parent ", 7) == 0) {
            if (commit->parent_count < MAX_PARENTS) {
                hex_to_hash(line + 7, &commit->parents[commit->parent_count]);
                commit->parent_count++;
            }
        } else if (strncmp(line, "author ", 7) == 0) {
            parse_signature(line + 7, &commit->author);
        } else if (strncmp(line, "committer ", 10) == 0) {
            parse_signature(line + 10, &commit->committer);
        } else if (strlen(line) == 0) {
            /* 空行，开始消息 */
            in_message = 1;
            if (next) message_start = next;
            break;
        }

        line = next;
    }

    /* 提取消息 */
    if (message_start) {
        size_t msg_len = size - (message_start - str);
        commit->message = (char *)malloc(msg_len + 1);
        if (commit->message) {
            memcpy(commit->message, message_start, msg_len);
            commit->message[msg_len] = 0;
        }
    }

    free(str);
    return 0;
}

int commit_serialize(const Commit *commit, uint8_t **data, size_t *size) {
    /* 动态计算所需大小 */
    size_t msg_len = commit->message ? strlen(commit->message) : 0;
    size_t buf_size = 4096 + msg_len;  /* 头部预留 4096（足以容纳极端长的作者信息）+ 消息 */
    char *buf = (char *)malloc(buf_size);
    if (!buf) return -1;

    int pos = 0;
    char tree_hex[HASH_HEX_SIZE];
    hash_to_hex(&commit->tree, tree_hex);

    pos += snprintf(buf + pos, buf_size - pos, "tree %s\n", tree_hex);

    for (int i = 0; i < commit->parent_count; i++) {
        char parent_hex[HASH_HEX_SIZE];
        hash_to_hex(&commit->parents[i], parent_hex);
        pos += snprintf(buf + pos, buf_size - pos, "parent %s\n", parent_hex);
    }

    pos += snprintf(buf + pos, buf_size - pos, 
                    "author %s <%s> %lu %s\n",
                    commit->author.name ? commit->author.name : "Unknown",
                    commit->author.email ? commit->author.email : "unknown@example.com",
                    (unsigned long)commit->author.timestamp,
                    commit->author.tz[0] ? commit->author.tz : "+0000");

    pos += snprintf(buf + pos, buf_size - pos, 
                    "committer %s <%s> %lu %s\n",
                    commit->committer.name ? commit->committer.name : "Unknown",
                    commit->committer.email ? commit->committer.email : "unknown@example.com",
                    (unsigned long)commit->committer.timestamp,
                    commit->committer.tz[0] ? commit->committer.tz : "+0000");

    /* 头部截断保护：snprintf 返回值超过剩余空间说明溢出 */
    if (pos < 0 || (size_t)pos + msg_len + 1 > buf_size) {
        free(buf);
        return -1;
    }

    buf[pos++] = '\n';  /* 空行分隔头部和消息 */

    if (commit->message) {
        memcpy(buf + pos, commit->message, msg_len);
        pos += (int)msg_len;
    }

    *data = (uint8_t *)malloc(pos);
    if (!*data) {
        free(buf);
        return -1;
    }
    memcpy(*data, buf, pos);
    *size = pos;
    free(buf);
    return 0;
}

/* 获取当前时区字符串 */
static void get_timezone(char *buf, size_t size) {
#ifdef _WIN32
    TIME_ZONE_INFORMATION tzi;
    GetTimeZoneInformation(&tzi);
    int bias = tzi.Bias;
    char sign = bias > 0 ? '-' : '+';
    bias = abs(bias);
    snprintf(buf, size, "%c%02d%02d", sign, bias / 60, bias % 60);
#else
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, size, "%z", tm);
#endif
}

int commit_create(ObjectStore *store, const Hash *tree, const Hash *parent,
                  const char *message, Hash *out) {
    Commit commit;
    memset(&commit, 0, sizeof(commit));

    commit.tree = *tree;
    
    if (parent) {
        commit.parents[0] = *parent;
        commit.parent_count = 1;
    }

    /* 设置作者/提交者信息 */
    commit.author.name = "mgit user";
    commit.author.email = "mgit@example.com";
    commit.author.timestamp = time(NULL);
    get_timezone(commit.author.tz, sizeof(commit.author.tz));

    commit.committer = commit.author;
    commit.message = (char *)message;

    /* 序列化 */
    uint8_t *data;
    size_t size;
    if (commit_serialize(&commit, &data, &size) != 0) {
        return -1;
    }

    /* 写入对象存储 */
    int ret = object_store_write(store, OBJ_COMMIT, data, size, out);
    free(data);
    return ret;
}

int commit_create_merge(ObjectStore *store, const Hash *tree,
                        const Hash *parent1, const Hash *parent2,
                        const char *message, Hash *out) {
    Commit commit;
    memset(&commit, 0, sizeof(commit));

    commit.tree = *tree;
    commit.parents[0] = *parent1;
    commit.parents[1] = *parent2;
    commit.parent_count = 2;

    commit.author.name = "mgit user";
    commit.author.email = "mgit@example.com";
    commit.author.timestamp = time(NULL);
    get_timezone(commit.author.tz, sizeof(commit.author.tz));
    commit.committer = commit.author;
    commit.message = (char *)message;

    uint8_t *data;
    size_t size;
    if (commit_serialize(&commit, &data, &size) != 0) {
        return -1;
    }

    int ret = object_store_write(store, OBJ_COMMIT, data, size, out);
    free(data);
    return ret;
}

int commit_read(ObjectStore *store, const Hash *hash, Commit *commit) {
    Object obj;
    if (object_store_read(store, hash, &obj) != 0) {
        return -1;
    }

    if (obj.type != OBJ_COMMIT) {
        mgit_error("object is not a commit");
        object_free(&obj);
        return -1;
    }

    int ret = commit_parse(obj.data, obj.size, commit);
    object_free(&obj);
    return ret;
}

int commit_read_tree(ObjectStore *store, const Hash *commit_hash, Tree *tree_out) {
    Commit commit;
    memset(&commit, 0, sizeof(commit));
    if (commit_read(store, commit_hash, &commit) != 0) {
        return -1;
    }

    Object tree_obj;
    memset(&tree_obj, 0, sizeof(tree_obj));
    if (object_store_read(store, &commit.tree, &tree_obj) != 0) {
        commit_free(&commit);
        return -1;
    }
    if (tree_obj.type != OBJ_TREE) {
        object_free(&tree_obj);
        commit_free(&commit);
        return -1;
    }
    int ret = tree_parse(tree_obj.data, tree_obj.size, tree_out);
    object_free(&tree_obj);
    commit_free(&commit);
    if (ret != 0) {
        mgit_error("cannot parse tree of commit");
        return -1;
    }
    return 0;
}
