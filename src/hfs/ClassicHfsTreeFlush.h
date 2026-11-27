// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdlib.h>
#include <string.h>

// Validate and stage the entire map chain before submitting any node write.
// The surrounding volume mutation handles rollback on a storage error.
static inline int ClassicHfsFlushTreeHeader(btree *tree)
{
    node header = tree->hdrnd;
    if (header.bt != tree || header.nnum || header.nd.ndType != ndHdrNode ||
        header.nd.ndNRecs != 3 || header.roff[0] != 14 || header.roff[1] != 120 ||
        header.roff[2] != 248 || header.roff[3] != 504 || !tree->map ||
        tree->mapsz < HFS_MAP1SZ || (tree->mapsz - HFS_MAP1SZ) % HFS_MAPXSZ ||
        (uint64_t)tree->mapsz * 8 < tree->hdr.bthNNodes || !ClassicHfsBitIsSet(tree->map, 0) ||
        !tree->hdr.bthNNodes || tree->hdr.bthNodeSize != ClassicHfsNodeSize ||
        tree->hdr.bthNNodes > tree->f.cat.u.fil.filPyLen / ClassicHfsNodeSize) {
        ERROR(EIO, "invalid classic HFS tree header or allocation map");
        return -1;
    }
    unsigned count = (tree->mapsz - HFS_MAP1SZ) / HFS_MAPXSZ;
    uint32_t next = header.nd.ndFLink;
    if (count >= tree->hdr.bthNNodes || !!next != !!count) {
        ERROR(EIO, "classic HFS map chain length mismatch");
        return -1;
    }
    node *parts = count ? (node *)calloc(count, sizeof(node)) : NULL;
    if (count && !parts) {
        ERROR(ENOMEM, "cannot stage classic HFS tree map");
        return -1;
    }
    for (unsigned i = 0; i < count; ++i) {
        node *part = &parts[i];
        part->bt = tree;
        part->nnum = next;
        if (ClassicHfsReadTreeNode(part) < 0)
            goto failed;
        if (part->nd.ndType != ndMapNode || part->nd.ndNRecs != 1 ||
            part->roff[0] != 14 || part->roff[1] != 506 ||
            !!part->nd.ndFLink != (i + 1 < count)) {
            ERROR(EIO, "invalid classic HFS tree map continuation");
            goto failed;
        }
        memcpy(part->data + 14, tree->map + HFS_MAP1SZ + i * HFS_MAPXSZ, HFS_MAPXSZ);
        next = part->nd.ndFLink;
    }
    ClassicHfsEncodeTreeHeader(header.data + 14, ClassicHfsTreeHeaderBytes, &tree->hdr);
    memcpy(header.data + 248, tree->map, HFS_MAP1SZ);
    for (unsigned i = 0; i < count; ++i)
        if (ClassicHfsWriteTreeNode(&parts[i]) < 0)
            goto failed;
    if (ClassicHfsWriteTreeNode(&header) < 0)
        goto failed;
    tree->hdrnd = header;
    tree->flags &= ~HFS_UPDATE_BTHDR;
    free(parts);
    return 0;
failed:
    free(parts);
    return -1;
}
