// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsExtentKey.h"

typedef struct {
    uint16_t xdrStABN;
    uint16_t xdrNumABlks;
} ClassicHfsExtent;

enum { ClassicHfsExtentCount = 3, ClassicHfsExtentBytes = 12 };

static inline int ClassicHfsEncodeExtents(unsigned char *bytes, const ClassicHfsExtent *extents)
{
    for (unsigned i = 0; i < ClassicHfsExtentCount; ++i) {
        ClassicHfsWrite16(bytes + 4*i, extents[i].xdrStABN);
        ClassicHfsWrite16(bytes + 4*i + 2, extents[i].xdrNumABlks);
    }
    return ClassicHfsExtentBytes;
}

static inline int ClassicHfsDecodeExtents(const unsigned char *bytes, ClassicHfsExtent *extents)
{
    for (unsigned i = 0; i < ClassicHfsExtentCount; ++i) {
        extents[i].xdrStABN = ClassicHfsRead16(bytes + 4*i);
        extents[i].xdrNumABlks = ClassicHfsRead16(bytes + 4*i + 2);
    }
    return ClassicHfsExtentBytes;
}

enum ClassicHfsExtentLocation {
    ClassicHfsInvalidExtent,
    ClassicHfsMappedBlock,
    ClassicHfsNextExtentRecord
};

// Resolve one extent record. On overflow, return the exact logical block
// that must key the next record in the extents tree.
static inline enum ClassicHfsExtentLocation ClassicHfsLocateBlock(
    const ClassicHfsExtent *extents, unsigned volumeBlocks, unsigned recordStart,
    unsigned logicalBlock, unsigned *location)
{
    if (logicalBlock < recordStart || logicalBlock >= volumeBlocks)
        return ClassicHfsInvalidExtent;
    unsigned base = recordStart;
    for (unsigned i = 0; i < ClassicHfsExtentCount; ++i) {
        unsigned start = extents[i].xdrStABN;
        unsigned count = extents[i].xdrNumABlks;
        if (!count || start >= volumeBlocks || count > volumeBlocks - start ||
            count > volumeBlocks - base)
            return ClassicHfsInvalidExtent;
        if (logicalBlock - base < count) {
            *location = start + (logicalBlock - base);
            return ClassicHfsMappedBlock;
        }
        base += count;
    }
    *location = base;
    return ClassicHfsNextExtentRecord;
}

static inline int ClassicHfsValidExtentRecord(const unsigned char *record, int size)
{
    return size >= ClassicHfsExtentKeySize + ClassicHfsExtentBytes &&
           record[0] == ClassicHfsExtentKeySize - 1;
}
