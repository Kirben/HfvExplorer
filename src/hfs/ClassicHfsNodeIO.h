// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBitmap.h"
#include "ClassicHfsNode.h"

typedef int (*ClassicHfsNodeTransfer)(void *, uint32_t, unsigned char (*)[ClassicHfsNodeSize]);
typedef enum {
    ClassicHfsNodeOk, ClassicHfsNodeUnavailable, ClassicHfsNodeBadOffsets, ClassicHfsNodeTransferFailed
} ClassicHfsNodeStatus;

// A reader can bootstrap the header and its map chain before installing the
// allocation bitmap. Writes always require an allocated, addressable node.
static inline ClassicHfsNodeStatus ClassicHfsTransferNode(
    uint32_t number, uint32_t total, const void *map, uint32_t mapBytes,
    unsigned char (*bytes)[ClassicHfsNodeSize], ClassicHfsNodeDescriptor *descriptor,
    uint16_t *offsets, int writing, ClassicHfsNodeTransfer transfer, void *context)
{
    if ((number >= total && (number || writing || map)) || (writing && !map) ||
        (map && (number / 8 >= mapBytes || !ClassicHfsBitIsSet(map, number))))
        return ClassicHfsNodeUnavailable;
    if (writing && !ClassicHfsEncodeNode(*bytes, descriptor, offsets))
        return ClassicHfsNodeBadOffsets;
    if (transfer(context, number, bytes) < 0)
        return ClassicHfsNodeTransferFailed;
    if (!writing && !ClassicHfsDecodeNode(*bytes, descriptor, offsets))
        return ClassicHfsNodeBadOffsets;
    return ClassicHfsNodeOk;
}
