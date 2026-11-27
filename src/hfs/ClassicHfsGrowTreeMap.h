// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdlib.h>
#include <string.h>

// Locate the existing tail once. Every continuation represents exactly
// HFS_MAPXSZ bytes in memory, which bounds traversal even for a corrupt cycle.
static inline int ClassicHfsGrowTreeMap(btree *tree)
{
    if (tree->hdr.bthNNodes <= (uint64_t)tree->mapsz * 8)
        return 0;
    if (!tree->map || tree->mapsz < HFS_MAP1SZ ||
        (tree->mapsz - HFS_MAP1SZ) % HFS_MAPXSZ) {
        ERROR(EIO, "invalid classic HFS allocation map size");
        return -1;
    }
    node tail;
    tail.bt = tree;
    unsigned count = (tree->mapsz - HFS_MAP1SZ) / HFS_MAPXSZ;
    uint32_t next = tree->hdrnd.nd.ndFLink;
    for (unsigned i = 0; i < count; ++i) {
        if (!next) {
            ERROR(EIO, "truncated classic HFS allocation map chain");
            return -1;
        }
        tail.nnum = next;
        if (bt_getnode(&tail) < 0)
            return -1;
        if (tail.nd.ndType != ndMapNode || tail.nd.ndNRecs != 1 ||
            tail.roff[0] != 14 || tail.roff[1] != 506) {
            ERROR(EIO, "invalid classic HFS allocation map continuation");
            return -1;
        }
        next = tail.nd.ndFLink;
    }
    if (next) {
        ERROR(EIO, "classic HFS allocation map chain exceeds its bitmap");
        return -1;
    }
    while (tree->hdr.bthNNodes > (uint64_t)tree->mapsz * 8) {
        char *expanded = (char *)realloc(tree->map, tree->mapsz + HFS_MAPXSZ);
        if (!expanded) {
            ERROR(ENOMEM, "cannot grow classic HFS allocation map");
            return -1;
        }
        memset(expanded + tree->mapsz, 0, HFS_MAPXSZ);
        tree->map = expanded;
        tree->mapsz += HFS_MAPXSZ;
        node added;
        ClassicHfsInitTreeNode(&added, tree, ndMapNode, 0);
        if (ClassicHfsAllocateTreeNode(&added) < 0)
            return -1;
        added.nd.ndNRecs = 1;
        added.roff[1] = 14 + HFS_MAPXSZ;
        added.nd.ndBLink = count ? tail.nnum : 0;
        if (bt_putnode(&added) < 0)
            return -1;
        if (count) {
            tail.nd.ndFLink = added.nnum;
            if (bt_putnode(&tail) < 0)
                return -1;
        } else {
            tree->hdrnd.nd.ndFLink = added.nnum;
        }
        tail = added;
        ++count;
    }
    return 0;
}
