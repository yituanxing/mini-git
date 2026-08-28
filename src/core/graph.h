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

#endif /* MGIT_GRAPH_H */
