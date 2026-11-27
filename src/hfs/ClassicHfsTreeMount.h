// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdlib.h>
#include <string.h>

// The overflow file can be consulted while reading its own map nodes, so
// install the decoded tree header first and publish the bitmap only at the end.
static inline int ClassicHfsLoadTreeHeader(btree *tree)
{
    node *header = &tree->hdrnd;
    header->bt = tree;
    header->nnum = 0;
    memset(&tree->hdr, 0, sizeof(tree->hdr));
    if (ClassicHfsReadTreeNode(header) < 0)
        return -1;
    if (header->nd.ndType != ndHdrNode || header->nd.ndNRecs != 3 ||
        header->roff[0] != 14 || header->roff[1] != 120 || header->roff[2] != 248 ||
        header->roff[3] != 504 ||
        !ClassicHfsDecodeTreeHeader(header->data + 14, ClassicHfsTreeHeaderBytes, &tree->hdr)) {
        ERROR(EIO, "invalid classic HFS tree header node");
        return -1;
    }
    uint32_t nodes = tree->hdr.bthNNodes;
    if (tree->hdr.bthNodeSize != ClassicHfsNodeSize || !nodes ||
        nodes > tree->f.cat.u.fil.filPyLen / ClassicHfsNodeSize) {
        ERROR(EIO, "classic HFS tree size exceeds its file or has unsupported geometry");
        return -1;
    }
    unsigned size = HFS_MAP1SZ;
    char *map = (char *)malloc(size);
    uint32_t next = header->nd.ndFLink;
    unsigned char *visited = next ? (unsigned char *)calloc((nodes + 7) / 8, 1) : NULL;
    if (!map || (next && !visited)) {
        ERROR(ENOMEM, "cannot allocate classic HFS tree map");
        goto failed;
    }
    memcpy(map, header->data + header->roff[2], size);
    while (next) {
        if (next >= nodes || ClassicHfsBitIsSet(visited, next)) {
            ERROR(EIO, "invalid or repeated classic HFS tree map node");
            goto failed;
        }
        ClassicHfsSetBit(visited, next);
        node part;
        part.bt = tree;
        part.nnum = next;
        if (ClassicHfsReadTreeNode(&part) < 0)
            goto failed;
        if (part.nd.ndType != ndMapNode || part.nd.ndNRecs != 1 ||
            part.roff[0] != 14 || part.roff[1] != 506 || size > UINT32_MAX - HFS_MAPXSZ) {
            ERROR(EIO, "invalid classic HFS tree map record");
            goto failed;
        }
        char *expanded = (char *)realloc(map, size + HFS_MAPXSZ);
        if (!expanded) {
            ERROR(ENOMEM, "cannot extend classic HFS tree map");
            goto failed;
        }
        map = expanded;
        memcpy(map + size, part.data + part.roff[0], HFS_MAPXSZ);
        size += HFS_MAPXSZ;
        next = part.nd.ndFLink;
    }
    if (size < (nodes + 7) / 8 || !ClassicHfsBitIsSet(map, 0)) {
        ERROR(EIO, "incomplete classic HFS tree allocation map");
        goto failed;
    }
    free(visited);
    tree->map = map;
    tree->mapsz = size;
    return 0;
failed:
    free(visited);
    free(map);
    return -1;
}
