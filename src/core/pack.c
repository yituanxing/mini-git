#include "pack.h"
#include "../base/zlib_util.h"
#include "../base/file.h"
#include "../base/error.h"

#include <zlib.h>       /* crc32 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/*
 * packfile 读写实现
 *
 * 写入：全量对象逐个 zlib 压缩（不做 delta，git 完全兼容）
 * 读取：支持完整对象 + OFS_DELTA + REF_DELTA（能解开 git gc 的 pack）
 */

/* pack on-disk object type numbers (they DIFFER from the ObjectType enum) */
#define PACK_OBJ_COMMIT   1
#define PACK_OBJ_TREE     2
#define PACK_OBJ_BLOB     3
#define PACK_OBJ_TAG      4
#define PACK_OBJ_OFS_DELTA 6
#define PACK_OBJ_REF_DELTA 7

/* Map ObjectType enum <-> pack on-disk type number.
 * They differ: enum blob=1 tree=2 commit=3 tag=4,
 * but pack uses commit=1 tree=2 blob=3 tag=4. */
static int pack_type_of(ObjectType t) {
    switch (t) {
    case OBJ_COMMIT: return PACK_OBJ_COMMIT;
    case OBJ_TREE:   return PACK_OBJ_TREE;
    case OBJ_BLOB:   return PACK_OBJ_BLOB;
    case OBJ_TAG:    return PACK_OBJ_TAG;
    default:         return -1;
    }
}

static ObjectType object_type_of_pack(int pt) {
    switch (pt) {
    case PACK_OBJ_COMMIT: return OBJ_COMMIT;
    case PACK_OBJ_TREE:   return OBJ_TREE;
    case PACK_OBJ_BLOB:   return OBJ_BLOB;
    case PACK_OBJ_TAG:    return OBJ_TAG;
    default:              return OBJ_NONE;
    }
}

#define MAX_DELTA_DEPTH 64   /* delta 链深度上限，防止环 */

/* ---------- 大端读写 ---------- */

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---------- 动态缓冲区 ---------- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

static int buf_append(Buf *b, const void *data, size_t len) {
    if (buf_reserve(b, len) != 0) return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

/* ---------- pack 对象头变长编码 ----------
 * 首字节: (type << 4) | size 低 4 位
 * 后续: 每字节 7 位，最高位表示还有后续字节
 */
static size_t encode_obj_header(uint8_t *out, int type, size_t size) {
    uint8_t c = (uint8_t)((type << 4) | (size & 15));
    size >>= 4;
    size_t n = 0;
    while (size) {
        out[n++] = (uint8_t)(c | 0x80);
        c = (uint8_t)(size & 0x7f);
        size >>= 7;
    }
    out[n++] = c;
    return n;
}

static int hash_cmp(const void *a, const void *b) {
    return memcmp(a, b, HASH_SIZE);
}

/* ================================================================
 * 写入 packfile
 * ================================================================ */

int pack_write(ObjectStore *store, Hash *hashes, size_t count,
               size_t *packed_out, char *name_out, size_t name_size) {
    if (count == 0) {
        *packed_out = 0;
        return 0;
    }

    /* 排序 + 去重（idx 要求按哈希有序） */
    qsort(hashes, count, sizeof(Hash), hash_cmp);
    size_t n = 1;
    for (size_t i = 1; i < count; i++) {
        if (memcmp(&hashes[i], &hashes[n - 1], HASH_SIZE) != 0) {
            hashes[n++] = hashes[i];
        }
    }

    uint32_t *crcs = (uint32_t *)malloc(sizeof(uint32_t) * n);
    uint64_t *offs = (uint64_t *)malloc(sizeof(uint64_t) * n);
    if (!crcs || !offs) {
        free(crcs);
        free(offs);
        return -1;
    }

    Buf pack = {0};
    SHA1_CTX sha;
    sha1_init(&sha);

    /* 头部: "PACK" + 版本 2 + 对象数 */
    uint8_t hdr[12];
    memcpy(hdr, "PACK", 4);
    put_be32(hdr + 4, 2);
    put_be32(hdr + 8, (uint32_t)n);
    if (buf_append(&pack, hdr, 12) != 0) goto oom;
    sha1_update(&sha, hdr, 12);

    for (size_t i = 0; i < n; i++) {
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &hashes[i], &obj) != 0) {
            free(crcs); free(offs); free(pack.data);
            return -1;
        }

        offs[i] = pack.len;

        /* 变长头（类型 + 未压缩大小） */
        uint8_t oh[16];
        size_t oh_len = encode_obj_header(oh, pack_type_of(obj.type), obj.size);
        if (buf_append(&pack, oh, oh_len) != 0) {
            object_free(&obj);
            goto oom;
        }
        sha1_update(&sha, oh, oh_len);

        /* zlib 压缩对象内容 */
        size_t comp_cap = obj.size + obj.size / 100 + 32;
        uint8_t *comp = (uint8_t *)malloc(comp_cap);
        if (!comp) {
            object_free(&obj);
            goto oom;
        }
        size_t comp_len = comp_cap;
        if (zlib_compress(obj.data, obj.size, comp, &comp_len) != 0) {
            free(comp);
            object_free(&obj);
            goto oom;
        }
        object_free(&obj);

        if (buf_append(&pack, comp, comp_len) != 0) {
            free(comp);
            goto oom;
        }
        sha1_update(&sha, comp, comp_len);

        /* CRC32 覆盖该对象的原始 pack 字节（头 + 压缩数据） */
        size_t raw_off = (size_t)offs[i];
        crcs[i] = (uint32_t)crc32(0L, Z_NULL, 0);
        crcs[i] = (uint32_t)crc32(crcs[i], pack.data + raw_off,
                                  pack.len - raw_off);
        free(comp);
    }

    /* 尾部: pack 内容的 SHA-1 */
    Hash pack_sha;
    sha1_final(&sha, &pack_sha);
    if (buf_append(&pack, pack_sha.bytes, HASH_SIZE) != 0) goto oom;

    /* ---------- 写 .pack ---------- */
    char pack_dir[1024];
    path_join(pack_dir, sizeof(pack_dir), store->objects_dir, "pack");
    if (file_mkdir_p(pack_dir) != 0) {
        free(crcs); free(offs); free(pack.data);
        return -1;
    }

    char sha_hex[HASH_HEX_SIZE];
    hash_to_hex(&pack_sha, sha_hex);

    char pack_path[1200];
    char idx_path[1200];
    snprintf(pack_path, sizeof(pack_path), "%s/pack-%s.pack", pack_dir, sha_hex);
    snprintf(idx_path, sizeof(idx_path), "%s/pack-%s.idx", pack_dir, sha_hex);

    if (file_write_all(pack_path, pack.data, pack.len) != 0) {
        free(crcs); free(offs); free(pack.data);
        return -1;
    }

    /* ---------- 构建 .idx v2 ---------- */
    Buf idx = {0};
    SHA1_CTX isha;
    sha1_init(&isha);

    uint8_t m[8];
    m[0] = 0xff; m[1] = 0x74; m[2] = 0x4f; m[3] = 0x63;  /* \377tOc */
    put_be32(m + 4, 2);
    if (buf_append(&idx, m, 8) != 0) goto oom2;
    sha1_update(&isha, m, 8);

    /* fanout: 首字节 <= i 的累计对象数 */
    uint32_t fanout[256];
    memset(fanout, 0, sizeof(fanout));
    for (size_t i = 0; i < n; i++) fanout[hashes[i].bytes[0]]++;
    for (int i = 1; i < 256; i++) fanout[i] += fanout[i - 1];
    uint8_t f4[4];
    for (int i = 0; i < 256; i++) {
        put_be32(f4, fanout[i]);
        if (buf_append(&idx, f4, 4) != 0) goto oom2;
        sha1_update(&isha, f4, 4);
    }

    /* 哈希表（已有序） */
    for (size_t i = 0; i < n; i++) {
        if (buf_append(&idx, hashes[i].bytes, HASH_SIZE) != 0) goto oom2;
        sha1_update(&isha, hashes[i].bytes, HASH_SIZE);
    }

    /* CRC32 表 */
    for (size_t i = 0; i < n; i++) {
        put_be32(f4, crcs[i]);
        if (buf_append(&idx, f4, 4) != 0) goto oom2;
        sha1_update(&isha, f4, 4);
    }

    /* 偏移表（教学规模不会超 2GB，大偏移路径仍按规范预留） */
    uint32_t large_pos = 0;
    Buf large = {0};
    for (size_t i = 0; i < n; i++) {
        uint32_t v;
        if (offs[i] >= 0x80000000ULL) {
            uint8_t b8[8];
            put_be32(b8, (uint32_t)(offs[i] >> 32));
            put_be32(b8 + 4, (uint32_t)(offs[i] & 0xffffffff));
            if (buf_append(&large, b8, 8) != 0) { free(large.data); goto oom2; }
            v = 0x80000000u | large_pos++;
        } else {
            v = (uint32_t)offs[i];
        }
        put_be32(f4, v);
        if (buf_append(&idx, f4, 4) != 0) { free(large.data); goto oom2; }
        sha1_update(&isha, f4, 4);
    }
    if (large.len > 0) {
        if (buf_append(&idx, large.data, large.len) != 0) { free(large.data); goto oom2; }
        sha1_update(&isha, large.data, large.len);
    }
    free(large.data);

    /* pack 校验和 + idx 自身校验和 */
    if (buf_append(&idx, pack_sha.bytes, HASH_SIZE) != 0) goto oom2;
    sha1_update(&isha, pack_sha.bytes, HASH_SIZE);
    Hash idx_sha;
    sha1_final(&isha, &idx_sha);
    if (buf_append(&idx, idx_sha.bytes, HASH_SIZE) != 0) goto oom2;

    int rc = file_write_all(idx_path, idx.data, idx.len);

    free(idx.data);
    free(pack.data);
    free(crcs);
    free(offs);

    if (rc != 0) return -1;

    *packed_out = n;
    snprintf(name_out, name_size, "pack-%s", sha_hex);
    return 0;

oom:
    free(pack.data);
    free(crcs);
    free(offs);
    mgit_error("out of memory while writing pack");
    return -1;

oom2:
    free(idx.data);
    free(pack.data);
    free(crcs);
    free(offs);
    mgit_error("out of memory while writing idx");
    return -1;
}

/*
 * 构建 pack 到内存（push 发送用；与 pack_write 的 pack 部分格式一致：
 * 全量对象逐个 zlib 压缩，不做 delta，git 完全兼容）
 *
 * hashes 会被排序去重（与 pack_write 相同约定）。
 */
int pack_build_memory(ObjectStore *store, Hash *hashes, size_t count,
                      size_t *packed_out, uint8_t **pack_out,
                      size_t *pack_size) {
    *pack_out = NULL;
    *pack_size = 0;
    if (count == 0) {
        *packed_out = 0;
        return 0;
    }

    /* 排序 + 去重 */
    qsort(hashes, count, sizeof(Hash), hash_cmp);
    size_t n = 1;
    for (size_t i = 1; i < count; i++) {
        if (memcmp(&hashes[i], &hashes[n - 1], HASH_SIZE) != 0) {
            hashes[n++] = hashes[i];
        }
    }

    Buf pack = {0};
    SHA1_CTX sha;
    sha1_init(&sha);

    /* 头部: "PACK" + 版本 2 + 对象数 */
    uint8_t hdr[12];
    memcpy(hdr, "PACK", 4);
    put_be32(hdr + 4, 2);
    put_be32(hdr + 8, (uint32_t)n);
    if (buf_append(&pack, hdr, 12) != 0) goto oom;
    sha1_update(&sha, hdr, 12);

    for (size_t i = 0; i < n; i++) {
        Object obj;
        memset(&obj, 0, sizeof(obj));
        if (object_store_read(store, &hashes[i], &obj) != 0) {
            free(pack.data);
            return -1;
        }

        /* 变长头（类型 + 未压缩大小） */
        uint8_t oh[16];
        size_t oh_len = encode_obj_header(oh, pack_type_of(obj.type), obj.size);
        if (buf_append(&pack, oh, oh_len) != 0) {
            object_free(&obj);
            goto oom;
        }
        sha1_update(&sha, oh, oh_len);

        /* zlib 压缩对象内容 */
        size_t comp_cap = obj.size + obj.size / 100 + 32;
        uint8_t *comp = (uint8_t *)malloc(comp_cap);
        if (!comp) {
            object_free(&obj);
            goto oom;
        }
        size_t comp_len = comp_cap;
        if (zlib_compress(obj.data, obj.size, comp, &comp_len) != 0) {
            free(comp);
            object_free(&obj);
            goto oom;
        }
        object_free(&obj);

        if (buf_append(&pack, comp, comp_len) != 0) {
            free(comp);
            goto oom;
        }
        sha1_update(&sha, comp, comp_len);
        free(comp);
    }

    /* 尾部: pack 内容的 SHA-1 */
    Hash pack_sha;
    sha1_final(&sha, &pack_sha);
    if (buf_append(&pack, pack_sha.bytes, HASH_SIZE) != 0) goto oom;

    *packed_out = n;
    *pack_out = pack.data;
    *pack_size = pack.len;
    return 0;

oom:
    free(pack.data);
    mgit_error("out of memory while building pack");
    return -1;
}

/* ================================================================
 * 读取 packfile
 * ================================================================ */

/* 载入一个 idx v2 文件（浅结构，指针指向 idx_data 内部） */
typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t count;
    const uint8_t *shas;      /* count * 20 */
    const uint8_t *offsets;   /* count * 4 */
    const uint8_t *large;     /* 大偏移表（可为 NULL） */
    size_t large_bytes;
} IdxInfo;

static int idx_load(const char *path, IdxInfo *out) {
    memset(out, 0, sizeof(*out));
    if (file_read_all(path, &out->data, &out->size) != 0) return -1;

    size_t sz = out->size;
    /* 最小尺寸: 8 + 1024 + 40 */
    if (sz < 8 + 1024 + 40) return -1;
    if (out->data[0] != 0xff || out->data[1] != 0x74 ||
        out->data[2] != 0x4f || out->data[3] != 0x63) return -1;
    if (get_be32(out->data + 4) != 2) return -1;  /* 仅支持 v2 */

    uint32_t total = get_be32(out->data + 8 + 255 * 4);
    size_t need = 8 + 1024 + (size_t)total * 28 + 40;
    if (sz < need) return -1;

    out->count = total;
    out->shas = out->data + 8 + 1024;
    out->offsets = out->shas + (size_t)total * 24;  /* skip sha(20) + crc(4) */
    out->large = out->offsets + (size_t)total * 4;
    out->large_bytes = sz - (out->large - out->data) - 40;
    return 0;
}

static void idx_free(IdxInfo *ix) {
    free(ix->data);
    ix->data = NULL;
}

/* 在 idx 中查哈希，返回条目下标，未找到返回 -1 */
static long idx_find(const IdxInfo *ix, const Hash *hash) {
    int first = hash->bytes[0];
    uint32_t lo = first ? get_be32(ix->data + 8 + (first - 1) * 4) : 0;
    uint32_t hi = get_be32(ix->data + 8 + first * 4);
    for (uint32_t i = lo; i < hi; i++) {
        if (memcmp(ix->shas + (size_t)i * 20, hash->bytes, HASH_SIZE) == 0) {
            return (long)i;
        }
    }
    return -1;
}

/* 取条目偏移（处理大偏移表） */
static uint64_t idx_offset(const IdxInfo *ix, uint32_t entry) {
    uint32_t v = get_be32(ix->offsets + (size_t)entry * 4);
    if (!(v & 0x80000000u)) return v;
    uint32_t pos = v & 0x7fffffffu;
    if ((size_t)pos * 8 + 8 > ix->large_bytes) return 0;
    const uint8_t *p = ix->large + (size_t)pos * 8;
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

/* delta 尺寸变长整数（小端 7 位分组） */
static size_t delta_read_size(const uint8_t **pp, const uint8_t *end) {
    size_t v = 0;
    int shift = 0;
    while (*pp < end) {
        uint8_t b = *(*pp)++;
        v |= (size_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return v;
}

/*
 * 应用 delta 指令
 * 指令流: 复制（高位 1）/ 插入（高位 0，长度 1..127）
 */
int pack_apply_delta(const uint8_t *base, size_t base_len,
                     const uint8_t *delta, size_t delta_len,
                     uint8_t **out, size_t *out_len) {
    const uint8_t *p = delta;
    const uint8_t *end = delta + delta_len;

    size_t bsize = delta_read_size(&p, end);
    size_t rsize = delta_read_size(&p, end);
    if (bsize != base_len) return -1;

    uint8_t *res = (uint8_t *)malloc(rsize ? rsize : 1);
    if (!res) return -1;
    size_t pos = 0;

    while (p < end) {
        uint8_t op = *p++;
        if (op & 0x80) {
            /* 复制: 低 4 位标记 offset 字节, 4-6 位标记 size 字节 */
            uint32_t cp_off = 0, cp_size = 0;
            for (int i = 0; i < 4; i++) {
                if (op & (1 << i)) {
                    if (p >= end) {
                        free(res);
                        return -1;
                    }
                    cp_off |= (uint32_t)*p++ << (8 * i);
                }
            }
            for (int i = 0; i < 3; i++) {
                if (op & (0x10 << i)) {
                    if (p >= end) {
                        free(res);
                        return -1;
                    }
                    cp_size |= (uint32_t)*p++ << (8 * i);
                }
            }
            if (cp_size == 0) cp_size = 0x10000;
            if ((size_t)cp_off + cp_size > base_len || pos + cp_size > rsize || p > end) {
                free(res);
                return -1;
            }
            memcpy(res + pos, base + cp_off, cp_size);
            pos += cp_size;
        } else if (op > 0) {
            /* 插入字面数据 */
            if (p + op > end || pos + op > rsize) {
                free(res);
                return -1;
            }
            memcpy(res + pos, p, op);
            p += op;
            pos += op;
        } else {
            free(res);
            return -1;  /* op == 0 非法 */
        }
    }

    if (pos != rsize) {
        free(res);
        return -1;
    }
    *out = res;
    *out_len = rsize;
    return 0;
}

/* 前向声明 */
static int pack_read_at(const uint8_t *pack, size_t pack_len,
                        uint64_t offset, int depth, ObjectStore *store,
                        Object *obj);

/* 在已载入的 pack 内读指定偏移处的对象 */
static int pack_read_at(const uint8_t *pack, size_t pack_len,
                        uint64_t offset, int depth, ObjectStore *store,
                        Object *obj) {
    if (depth > MAX_DELTA_DEPTH) return -1;
    if (offset >= pack_len) return -1;

    const uint8_t *p = pack + offset;
    const uint8_t *end = pack + pack_len;

    /* 变长头: 类型(3bit) + 大小 */
    uint8_t c = *p++;
    int type = (c >> 4) & 7;
    size_t size = c & 15;
    int shift = 4;
    while (c & 0x80) {
        if (p >= end) return -1;
        c = *p++;
        size |= (size_t)(c & 0x7f) << shift;
        shift += 7;
    }

    switch (type) {
    case PACK_OBJ_COMMIT:
    case PACK_OBJ_TREE:
    case PACK_OBJ_BLOB:
    case PACK_OBJ_TAG: {
        uint8_t *data;
        size_t dlen;
        if (zlib_decompress_alloc(p, (size_t)(end - p), &data, &dlen) != 0)
            return -1;
        if (dlen != size) {
            free(data);
            return -1;
        }
        obj->type = object_type_of_pack(type);
        obj->size = dlen;
        obj->data = data;
        return 0;
    }
    case PACK_OBJ_OFS_DELTA: {
        /* 负偏移变长编码 */
        if (p >= end) return -1;
        uint8_t b = *p++;
        uint64_t rel = b & 0x7f;
        while (b & 0x80) {
            if (p >= end) return -1;
            b = *p++;
            rel = ((rel + 1) << 7) | (b & 0x7f);
        }
        if (rel > offset) return -1;

        Object base;
        memset(&base, 0, sizeof(base));
        if (pack_read_at(pack, pack_len, offset - rel, depth + 1, store, &base) != 0)
            return -1;

        uint8_t *delta;
        size_t delta_len;
        if (zlib_decompress_alloc(p, (size_t)(end - p), &delta, &delta_len) != 0) {
            object_free(&base);
            return -1;
        }
        uint8_t *res;
        size_t res_len;
        int rc = pack_apply_delta(base.data, base.size, delta, delta_len, &res, &res_len);
        free(delta);
        if (rc != 0) {
            object_free(&base);
            return -1;
        }
        obj->type = base.type;
        obj->size = res_len;
        obj->data = res;
        object_free(&base);
        return 0;
    }
    case PACK_OBJ_REF_DELTA: {
        if (p + HASH_SIZE > end) return -1;
        Hash base_hash;
        memcpy(base_hash.bytes, p, HASH_SIZE);
        p += HASH_SIZE;

        /* 基对象可能在 pack 外（松散对象）：统一走对象存储 */
        Object base;
        memset(&base, 0, sizeof(base));
        if (object_store_read(store, &base_hash, &base) != 0)
            return -1;

        uint8_t *delta;
        size_t delta_len;
        if (zlib_decompress_alloc(p, (size_t)(end - p), &delta, &delta_len) != 0) {
            object_free(&base);
            return -1;
        }
        uint8_t *res;
        size_t res_len;
        int rc = pack_apply_delta(base.data, base.size, delta, delta_len, &res, &res_len);
        free(delta);
        if (rc != 0) {
            object_free(&base);
            return -1;
        }
        obj->type = base.type;
        obj->size = res_len;
        obj->data = res;
        object_free(&base);
        return 0;
    }
    default:
        return -1;
    }
}

/* 在一个 pack/idx 对中查找并读取对象；-1 表示不在这个 pack */
static int pack_try_read(const char *pack_dir, const char *idx_name,
                         const Hash *hash, ObjectStore *store, Object *obj) {
    char idx_path[1200];
    snprintf(idx_path, sizeof(idx_path), "%s/%s", pack_dir, idx_name);

    IdxInfo ix;
    if (idx_load(idx_path, &ix) != 0) return -1;

    long entry = idx_find(&ix, hash);
    if (entry < 0) {
        idx_free(&ix);
        return -1;
    }
    uint64_t off = idx_offset(&ix, (uint32_t)entry);
    idx_free(&ix);

    /* pack 文件名与 idx 同名（扩展名不同） */
    char pack_path[1200];
    snprintf(pack_path, sizeof(pack_path), "%s/%.*s.pack",
             pack_dir, (int)(strlen(idx_name) - 4), idx_name);

    uint8_t *pack;
    size_t pack_len;
    if (file_read_all(pack_path, &pack, &pack_len) != 0) return -1;

    int rc = pack_read_at(pack, pack_len, off, 0, store, obj);
    free(pack);
    if (rc == 0) {
        obj->hash = *hash;
        return 0;
    }
    return -1;
}

int pack_read_object(ObjectStore *store, const Hash *hash, Object *obj) {
    char pack_dir[1024];
    path_join(pack_dir, sizeof(pack_dir), store->objects_dir, "pack");

    DIR *dir = opendir(pack_dir);
    if (!dir) return -1;

    int rc = -1;
    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        const char *name = d->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".idx") != 0) continue;

        if (pack_try_read(pack_dir, name, hash, store, obj) == 0) {
            rc = 0;
            break;
        }
    }
    closedir(dir);
    return rc;
}

int pack_contains(ObjectStore *store, const Hash *hash) {
    char pack_dir[1024];
    path_join(pack_dir, sizeof(pack_dir), store->objects_dir, "pack");

    DIR *dir = opendir(pack_dir);
    if (!dir) return 0;

    int found = 0;
    struct dirent *d;
    while ((d = readdir(dir)) != NULL && !found) {
        const char *name = d->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".idx") != 0) continue;

        char idx_path[1200];
        snprintf(idx_path, sizeof(idx_path), "%s/%s", pack_dir, name);

        IdxInfo ix;
        if (idx_load(idx_path, &ix) != 0) continue;
        if (idx_find(&ix, hash) >= 0) found = 1;
        idx_free(&ix);
    }
    closedir(dir);
    return found;
}

/*
 * 在所有 idx 中按十六进制前缀查找对象（短哈希支持）
 * 扫描每个 idx 的哈希表，返回第一个匹配项。
 *
 * @param prefix  十六进制前缀（1-40 字符，全小写）
 * @param plen    前缀长度
 * @param out     输出：完整哈希
 * @return        0 找到，-1 未找到
 */
int pack_find_by_prefix(ObjectStore *store, const char *prefix,
                        size_t plen, Hash *out) {
    char pack_dir[1024];
    path_join(pack_dir, sizeof(pack_dir), store->objects_dir, "pack");

    DIR *dir = opendir(pack_dir);
    if (!dir) return -1;

    int found = -1;
    struct dirent *d;
    while ((d = readdir(dir)) != NULL && found != 0) {
        const char *name = d->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".idx") != 0) continue;

        char idx_path[1200];
        snprintf(idx_path, sizeof(idx_path), "%s/%s", pack_dir, name);

        IdxInfo ix;
        if (idx_load(idx_path, &ix) != 0) continue;

        /* 哈希表按哈希排序，逐条比对十六进制前缀 */
        char hex[HASH_HEX_SIZE];
        for (uint32_t i = 0; i < ix.count; i++) {
            Hash cand;
            memcpy(cand.bytes, ix.shas + (size_t)i * 20, HASH_SIZE);
            hash_to_hex(&cand, hex);
            if (strncmp(hex, prefix, plen) == 0) {
                *out = cand;
                found = 0;
                break;
            }
        }
        idx_free(&ix);
    }
    closedir(dir);
    return found;
}

int pack_stats(ObjectStore *store, size_t *num_packs,
               size_t *num_objects, size_t *total_bytes) {
    *num_packs = 0;
    *num_objects = 0;
    *total_bytes = 0;

    char pack_dir[1024];
    path_join(pack_dir, sizeof(pack_dir), store->objects_dir, "pack");

    DIR *dir = opendir(pack_dir);
    if (!dir) return 0;  /* 没有 pack 目录不算错误 */

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        const char *name = d->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".pack") != 0) continue;

        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", pack_dir, name);

        uint8_t *data;
        size_t size;
        if (file_read_all(path, &data, &size) != 0) continue;

        (*num_packs)++;
        *total_bytes += size;
        if (size >= 12 && memcmp(data, "PACK", 4) == 0) {
            *num_objects += get_be32(data + 8);
        }
        free(data);
    }
    closedir(dir);
    return 0;
}
