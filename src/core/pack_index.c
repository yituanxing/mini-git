#include "pack_index.h"
#include "pack.h"
#include "../base/zlib_util.h"
#include "../base/hash.h"
#include "../base/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * pack 解包器（unpack-objects）
 *
 * 顺序扫描 pack：每解出一个完整对象就记录（偏移 + 类型 + 内容），
 * 供后面的 OFS_DELTA / REF_DELTA 作为基对象使用。
 * 全部解完后逐个计算哈希写入松散对象存储。
 *
 * 内存策略：所有解出的对象内容保留到结束（delta 基对象随时可能被引用）。
 * 教学规模的仓库完全够用；超大仓库应换用流式 index-pack。
 */

#define PACK_OBJ_COMMIT   1
#define PACK_OBJ_TREE     2
#define PACK_OBJ_BLOB     3
#define PACK_OBJ_TAG      4
#define PACK_OBJ_OFS_DELTA 6
#define PACK_OBJ_REF_DELTA 7

/* 已解出的对象条目 */
typedef struct {
    uint64_t offset;      /* 在 pack 中的起始偏移（OFS_DELTA 定位用） */
    ObjectType type;
    uint8_t *data;
    size_t size;
} Entry;

static ObjectType pack_to_object_type(int pt) {
    switch (pt) {
    case PACK_OBJ_COMMIT: return OBJ_COMMIT;
    case PACK_OBJ_TREE:   return OBJ_TREE;
    case PACK_OBJ_BLOB:   return OBJ_BLOB;
    case PACK_OBJ_TAG:    return OBJ_TAG;
    default:              return OBJ_NONE;
    }
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 在已解出条目中按偏移查找基对象（OFS_DELTA） */
static Entry *find_by_offset(Entry *entries, size_t count, uint64_t off) {
    /* 条目按偏移递增写入，从后往前找更快 */
    for (size_t i = count; i > 0; i--) {
        if (entries[i - 1].offset == off) return &entries[i - 1];
    }
    return NULL;
}

/* 在已解出条目中按哈希查找基对象（REF_DELTA） */
static Entry *find_by_hash(Entry *entries, size_t count, const Hash *hash) {
    for (size_t i = 0; i < count; i++) {
        Hash h;
        object_hash(entries[i].type, entries[i].data, entries[i].size, &h);
        if (memcmp(h.bytes, hash->bytes, HASH_SIZE) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

int pack_unpack(ObjectStore *store, const uint8_t *pack, size_t pack_size,
                size_t *unpacked) {
    if (unpacked) *unpacked = 0;

    /* ---------- 头部校验 ---------- */
    if (pack_size < 12 + HASH_SIZE || memcmp(pack, "PACK", 4) != 0) {
        mgit_error("invalid pack header");
        return -1;
    }
    uint32_t version = get_be32(pack + 4);
    uint32_t count = get_be32(pack + 8);
    if (version != 2 && version != 3) {
        mgit_error("unsupported pack version %u", (unsigned)version);
        return -1;
    }

    /* ---------- 尾部校验和 ---------- */
    SHA1_CTX sha;
    sha1_init(&sha);
    sha1_update(&sha, pack, pack_size - HASH_SIZE);
    Hash expect;
    sha1_final(&sha, &expect);
    if (memcmp(expect.bytes, pack + pack_size - HASH_SIZE, HASH_SIZE) != 0) {
        mgit_error("pack checksum mismatch (data corrupted in transit?)");
        return -1;
    }

    Entry *entries = NULL;
    if (count > 0) {
        entries = (Entry *)calloc(count, sizeof(Entry));
        if (!entries) {
            mgit_error("out of memory");
            return -1;
        }
    }

    size_t pos = 12;
    size_t end = pack_size - HASH_SIZE;
    int rc = -1;
    uint32_t i;

    for (i = 0; i < count; i++) {
        if (pos >= end) {
            mgit_error("pack truncated at object %u", (unsigned)i);
            goto cleanup;
        }
        uint64_t entry_start = pos;

        /* 变长头: 类型(3bit) + 未压缩大小 */
        uint8_t c = pack[pos++];
        int pt = (c >> 4) & 7;
        size_t size = c & 15;
        int shift = 4;
        while (c & 0x80) {
            if (pos >= end) goto truncated;
            c = pack[pos++];
            size |= (size_t)(c & 0x7f) << shift;
            shift += 7;
        }

        ObjectType type = OBJ_NONE;
        uint8_t *data = NULL;
        size_t dlen = 0;

        if (pt == PACK_OBJ_OFS_DELTA) {
            /* 负偏移变长编码：基对象 = entry_start - rel */
            if (pos >= end) goto truncated;
            uint8_t b = pack[pos++];
            uint64_t rel = b & 0x7f;
            while (b & 0x80) {
                if (pos >= end) goto truncated;
                b = pack[pos++];
                rel = ((rel + 1) << 7) | (b & 0x7f);
            }
            if (rel > entry_start) goto truncated;

            Entry *base = find_by_offset(entries, i, entry_start - rel);
            if (!base) {
                mgit_error("ofs-delta base not found at object %u",
                           (unsigned)i);
                goto cleanup;
            }

            uint8_t *delta;
            size_t delta_len, consumed;
            if (zlib_inflate_stream(pack + pos, end - pos,
                                    &delta, &delta_len, &consumed) != 0) {
                goto truncated;
            }
            pos += consumed;

            if (pack_apply_delta(base->data, base->size,
                                 delta, delta_len, &data, &dlen) != 0) {
                free(delta);
                mgit_error("bad ofs-delta at object %u", (unsigned)i);
                goto cleanup;
            }
            free(delta);
            type = base->type;

        } else if (pt == PACK_OBJ_REF_DELTA) {
            if (pos + HASH_SIZE > end) goto truncated;
            Hash base_hash;
            memcpy(base_hash.bytes, pack + pos, HASH_SIZE);
            pos += HASH_SIZE;

            /* 基对象优先在本 pack 内找，找不到再查已有对象库 */
            uint8_t *base_data = NULL;
            size_t base_len = 0;
            ObjectType base_type = OBJ_NONE;
            Object loose;
            memset(&loose, 0, sizeof(loose));
            int have_loose = 0;

            Entry *base = find_by_hash(entries, i, &base_hash);
            if (base) {
                base_data = base->data;
                base_len = base->size;
                base_type = base->type;
            } else if (object_store_read(store, &base_hash, &loose) == 0) {
                have_loose = 1;
                base_data = loose.data;
                base_len = loose.size;
                base_type = loose.type;
            } else {
                mgit_error("ref-delta base missing at object %u", (unsigned)i);
                goto cleanup;
            }

            uint8_t *delta;
            size_t delta_len, consumed;
            int irc = zlib_inflate_stream(pack + pos, end - pos,
                                          &delta, &delta_len, &consumed);
            if (irc == 0) {
                pos += consumed;
                if (pack_apply_delta(base_data, base_len,
                                     delta, delta_len, &data, &dlen) != 0) {
                    irc = -1;
                }
                free(delta);
            }
            if (have_loose) object_free(&loose);
            if (irc != 0) {
                mgit_error("bad ref-delta at object %u", (unsigned)i);
                goto cleanup;
            }
            type = base_type;

        } else {
            /* 完整对象 */
            type = pack_to_object_type(pt);
            if (type == OBJ_NONE) {
                mgit_error("unknown pack object type %d", pt);
                goto cleanup;
            }
            size_t consumed;
            if (zlib_inflate_stream(pack + pos, end - pos,
                                    &data, &dlen, &consumed) != 0) {
                goto truncated;
            }
            pos += consumed;
            if (dlen != size) {
                mgit_error("object size mismatch at object %u", (unsigned)i);
                free(data);
                goto cleanup;
            }
        }

        entries[i].offset = entry_start;
        entries[i].type = type;
        entries[i].data = data;
        entries[i].size = dlen;
    }

    /* ---------- 全部解开：逐个写入松散对象 ---------- */
    for (i = 0; i < count; i++) {
        Hash h;
        if (object_store_write(store, entries[i].type,
                               entries[i].data, entries[i].size, &h) != 0) {
            mgit_error("failed to write unpacked object");
            goto cleanup;
        }
    }

    if (unpacked) *unpacked = count;
    rc = 0;
    goto cleanup;

truncated:
    mgit_error("corrupted zlib stream in pack");

cleanup:
    if (entries) {
        for (uint32_t i = 0; i < count; i++) free(entries[i].data);
        free(entries);
    }
    return rc;
}
