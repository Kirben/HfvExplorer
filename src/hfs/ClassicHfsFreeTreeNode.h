// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

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
