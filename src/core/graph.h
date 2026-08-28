#ifndef MGIT_GRAPH_H
#define MGIT_GRAPH_H

#include "object.h"
#include "../base/hash.h"

/*
 * Shared commit-graph primitives.
 *
 * Keep this layer deliberately small: commands still show their Git-level
 * orchestration, while ancestry itself has one correct implementation.
 */

/* Return 1 if ancestor is reachable from start (including start), 0 if not. */
int graph_is_ancestor(ObjectStore *store, const Hash *ancestor,
                      const Hash *start);

/*
 * Find the nearest common ancestor using mgit's deliberately simple
 * breadth-first merge-base rule. Return 0 on success, -1 if none is found.
 */
int graph_find_merge_base(ObjectStore *store, const Hash *ours,
                          const Hash *theirs, Hash *base_out);

#endif /* MGIT_GRAPH_H */
