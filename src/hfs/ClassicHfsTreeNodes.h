// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

int ClassicHfsBuildSeparator(node *, unsigned char *, unsigned, int *);

int ClassicHfsInsertAtCursor(node *, unsigned char *, int *);

int ClassicHfsDeleteNodeRecord(node *, unsigned char *, unsigned, int *);
#include <string.h>

static inline void ClassicHfsInitTreeNode(node *cursor, btree *tree, int type, unsigned height)
{
    memset(cursor, 0, sizeof(*cursor));
    cursor->bt = tree;
    cursor->nnum = UINT32_MAX;
    cursor->rnum = -1;
    cursor->nd.ndType = (int8_t)type;
    cursor->nd.ndNHeight = (int8_t)height;
    cursor->roff[0] = ClassicHfsNodeHeaderSize;
}

static inline int ClassicHfsAllocateTreeNode(node *cursor)
{
    btree *tree = cursor->bt;
    if (cursor->nnum != UINT32_MAX) {
        ERROR(EIO, "classic HFS node already has an allocation");
        return -1;
    }
    uint32_t number = ClassicHfsClaimTreeNode(tree->map, tree->mapsz, tree->hdr.bthNNodes,
                                              tree->hdr.bthFree);
    if (!number) {
        ERROR(EIO, "no usable free node in classic HFS B-tree bitmap");
        return -1;
    }
    cursor->nnum = number;
    --tree->hdr.bthFree;
    tree->flags |= HFS_UPDATE_BTHDR;
    return 0;
}

static inline int ClassicHfsFreeTreeNode(node *released)
{
    btree *tree = released->bt;
    if (!ClassicHfsReleaseTreeNode(tree->map, tree->mapsz, tree->hdr.bthNNodes,
                                   tree->hdr.bthFree, released->nnum)) {
        ERROR(EIO, "invalid classic HFS B-tree node release");
        return -1;
    }
    ++tree->hdr.bthFree;
    tree->flags |= HFS_UPDATE_BTHDR;
    return 0;
}
