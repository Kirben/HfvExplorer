// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdlib.h>
#include <string.h>

// Begin with the header node reserved. Tree growth allocates its physical
// storage before the formatter inserts the root folder.
static inline int ClassicHfsInitFormatTree(btree *tree, unsigned keyLength,
                                           int (*compare)(unsigned char *, unsigned char *))
{
    tree->map = (char *)calloc(HFS_MAP1SZ, 1);
    if (!tree->map) {
        ERROR(ENOMEM, "cannot allocate classic HFS tree bitmap");
        return -1;
    }
    tree->mapsz = HFS_MAP1SZ;
    ClassicHfsSetBit(tree->map, 0);
    tree->flags = HFS_UPDATE_BTHDR;
    tree->compare = compare;

    memset(&tree->hdr, 0, sizeof(tree->hdr));
    tree->hdr.bthNodeSize = ClassicHfsNodeSize;
    tree->hdr.bthKeyLen = keyLength;

    node *header = &tree->hdrnd;
    memset(header, 0, sizeof(*header));
    header->bt = tree;
    header->rnum = -1;
    header->nd.ndType = ndHdrNode;
    header->nd.ndNRecs = 3;
    header->roff[0] = ClassicHfsNodeHeaderSize;
    header->roff[1] = header->roff[0] + ClassicHfsTreeHeaderBytes;
    header->roff[2] = header->roff[1] + 128; // user data record
    header->roff[3] = header->roff[2] + HFS_MAP1SZ;
    return 0;
}
