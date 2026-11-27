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
    unsigned char staged[ClassicHfsNodeSize];
    if (writing) {
        memcpy(staged, *bytes, sizeof(staged));
        if (!ClassicHfsEncodeNode(staged, descriptor, offsets))
            return ClassicHfsNodeBadOffsets;
    }
    if (transfer(context, number, &staged) < 0)
        return ClassicHfsNodeTransferFailed;
    if (!writing) {
        ClassicHfsNodeDescriptor decoded;
        uint16_t positions[ClassicHfsMaxRecords + 1];
        if (!ClassicHfsDecodeNode(staged, &decoded, positions))
            return ClassicHfsNodeBadOffsets;
        *descriptor = decoded;
        memcpy(offsets, positions, (decoded.ndNRecs + 1) * sizeof(*offsets));
    }
    memcpy(*bytes, staged, sizeof(staged));
    return ClassicHfsNodeOk;
}
