// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"

enum { ClassicHfsExtentKeySize = 8 };

static inline int ClassicHfsWriteExtentKey(unsigned char *key, unsigned char fork,
                                          uint32_t fileId, uint16_t firstBlock)
{
    key[0] = ClassicHfsExtentKeySize - 1;
    key[1] = fork;
    ClassicHfsWrite32(key + 2, fileId);
    ClassicHfsWrite16(key + 6, firstBlock);
    return ClassicHfsExtentKeySize;
}

// The overflow B-tree orders by file ID first even though the fork is stored first.
static inline int ClassicHfsCompareExtentKeys(unsigned char *left, unsigned char *right)
{
    uint32_t leftId = ClassicHfsRead32(left + 2);
    uint32_t rightId = ClassicHfsRead32(right + 2);
    if (leftId != rightId)
        return leftId < rightId ? -1 : 1;
    if (left[1] != right[1])
        return (int)left[1] - right[1];
    return (int)ClassicHfsRead16(left + 6) - ClassicHfsRead16(right + 6);
}
