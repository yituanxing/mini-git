#include "graph.h"
#include "commit.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    Hash *items;
    size_t count;
    size_t cap;
} HashVec;

static void vec_free(HashVec *v) {
    free(v->items);
    v->items = NULL;
    v->count = v->cap = 0;
}

static int vec_contains(const HashVec *v, const Hash *h) {
    for (size_t i = 0; i < v->count; i++) {
        if (hash_equal(&v->items[i], h)) return 1;
    }
    return 0;
}

static int vec_push(HashVec *v, const Hash *h) {
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        Hash *p = (Hash *)realloc(v->items, ncap * sizeof(Hash));
        if (!p) return -1;
        v->items = p;
        v->cap = ncap;
    }
    v->items[v->count++] = *h;
    return 0;
}

int graph_is_ancestor(ObjectStore *store, const Hash *ancestor,
                      const Hash *start) {
    if (hash_equal(ancestor, start)) return 1;

    HashVec queue = {0};
    HashVec seen = {0};
    size_t head = 0;

    if (vec_push(&queue, start) != 0) return 0;

    while (head < queue.count) {
        Hash cur = queue.items[head++];
        if (vec_contains(&seen, &cur)) continue;
        if (vec_push(&seen, &cur) != 0) break;

        Commit commit;
        memset(&commit, 0, sizeof(commit));
        if (commit_read(store, &cur, &commit) != 0) continue;

        for (int i = 0; i < commit.parent_count; i++) {
            if (hash_equal(ancestor, &commit.parents[i])) {
                commit_free(&commit);
                vec_free(&queue);
                vec_free(&seen);
                return 1;
            }
            if (!vec_contains(&seen, &commit.parents[i])) {
                if (vec_push(&queue, &commit.parents[i]) != 0) {
                    commit_free(&commit);
                    vec_free(&queue);
                    vec_free(&seen);
                    return 0;
                }
            }
        }
        commit_free(&commit);
    }

    vec_free(&queue);
    vec_free(&seen);
    return 0;
}
