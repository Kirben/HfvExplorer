// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Keep the last visited node and predecessor slot available to callers.
static inline int ClassicHfsFindTreeKey(btree *tree, unsigned char *key, node *cursor)
{
    cursor->bt = tree;
    cursor->nnum = tree->hdr.bthRoot;
    cursor->rnum = -1;
    int root = ClassicHfsCheckSearchRoot(&tree->hdr);
    if (root < 0) {
        ERROR(EIO, "invalid classic HFS search root");
        return -1;
    }
    for (unsigned height = tree->hdr.bthDepth; height; --height) {
        if (ClassicHfsReadTreeNode(cursor) < 0)
            return -1;
        uint32_t child;
        int result = ClassicHfsLookupLevel(cursor->data, &cursor->nd, cursor->roff, height,
                                           tree == &tree->f.vol->cat, key, tree->compare,
                                           &cursor->rnum, tree->hdr.bthNNodes, &child, ClassicHfsLookup);
        if (result == ClassicHfsFoundKey)
            return 1;
        if (result == ClassicHfsMissingKey)
            break;
        if (result != ClassicHfsVisitChild || child == cursor->nnum) {
            ERROR(EIO, "invalid classic HFS search path");
            return -1;
        }
        cursor->nnum = child;
    }
    ERROR(ENOENT, NULL);
    return 0;
}
