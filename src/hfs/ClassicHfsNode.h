// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"

enum { ClassicHfsNodeSize = 512, ClassicHfsNodeHeaderSize = 14, ClassicHfsMaxRecords = 35 };

typedef struct {
    uint32_t ndFLink, ndBLink;
    int8_t ndType, ndNHeight;
    uint16_t ndNRecs;
    int16_t ndResv2;
} ClassicHfsNodeDescriptor;

// The last offset marks the start of free space. The offset table itself
// grows backward from the end of the sector.
static inline int ClassicHfsValidOffsets(unsigned count, const uint16_t *offsets)
{
    if (count > ClassicHfsMaxRecords || offsets[0] < ClassicHfsNodeHeaderSize)
        return 0;
    unsigned limit = ClassicHfsNodeSize - 2 * (count + 1);
    for (unsigned i = 0; i <= count; ++i) {
        if ((offsets[i] & 1) || offsets[i] > limit || (i && offsets[i] <= offsets[i - 1]))
            return 0;
    }
    return 1;
}

static inline int ClassicHfsDecodeNode(const unsigned char *bytes, ClassicHfsNodeDescriptor *node,
                                      uint16_t *offsets)
{
    unsigned count = ClassicHfsRead16(bytes + 10);
    if (count > ClassicHfsMaxRecords)
        return 0;
    for (unsigned i = 0; i <= count; ++i)
        offsets[i] = ClassicHfsRead16(bytes + ClassicHfsNodeSize - 2 * (i + 1));
    if (!ClassicHfsValidOffsets(count, offsets))
        return 0;
    node->ndFLink = ClassicHfsRead32(bytes);
    node->ndBLink = ClassicHfsRead32(bytes + 4);
    node->ndType = (int8_t)bytes[8];
    node->ndNHeight = (int8_t)bytes[9];
    node->ndNRecs = (uint16_t)count;
    node->ndResv2 = (int16_t)ClassicHfsRead16(bytes + 12);
    return 1;
}

static inline int ClassicHfsEncodeNode(unsigned char *bytes, const ClassicHfsNodeDescriptor *node,
                                      const uint16_t *offsets)
{
    unsigned count = node->ndNRecs;
    if (!ClassicHfsValidOffsets(count, offsets))
        return 0;
    ClassicHfsWrite32(bytes, node->ndFLink);
    ClassicHfsWrite32(bytes + 4, node->ndBLink);
    bytes[8] = (unsigned char)node->ndType;
    bytes[9] = (unsigned char)node->ndNHeight;
    ClassicHfsWrite16(bytes + 10, node->ndNRecs);
    ClassicHfsWrite16(bytes + 12, (uint16_t)node->ndResv2);
    for (unsigned i = 0; i <= count; ++i)
        ClassicHfsWrite16(bytes + ClassicHfsNodeSize - 2 * (i + 1), offsets[i]);
    return 1;
}
