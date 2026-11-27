// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsExtentKey.h"
#include <string.h>

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

typedef int (*ClassicHfsLoadExtentRecord)(void *, unsigned, ClassicHfsExtent (*)[ClassicHfsExtentCount]);

// Keep each successfully resolved overflow record available to nested reads
// of the extents tree itself. A failed lookup must never publish its output.
static inline int ClassicHfsResolveForkSector(uint32_t number, unsigned sectorsPerBlock,
                                             unsigned volumeBlocks, const ClassicHfsExtent *initial,
                                             ClassicHfsExtent *cached, unsigned *cacheStart,
                                             ClassicHfsLoadExtentRecord load, void *context,
                                             unsigned *physical, unsigned *sector)
{
    if (!sectorsPerBlock || number / sectorsPerBlock >= volumeBlocks)
        return -1;
    unsigned logical = number / sectorsPerBlock;
    if (logical < *cacheStart) {
        memcpy(cached, initial, ClassicHfsExtentCount * sizeof(*cached));
        *cacheStart = 0;
    }
    for (;;) {
        unsigned location;
        enum ClassicHfsExtentLocation result =
            ClassicHfsLocateBlock(cached, volumeBlocks, *cacheStart, logical, &location);
        if (result == ClassicHfsMappedBlock) {
            *physical = location;
            *sector = number % sectorsPerBlock;
            return 1;
        }
        if (result != ClassicHfsNextExtentRecord)
            return -1;
        ClassicHfsExtent next[ClassicHfsExtentCount];
        if (load(context, location, &next) <= 0)
            return 0;
        memcpy(cached, next, sizeof(next));
        *cacheStart = location;
    }
}

static inline int ClassicHfsValidExtentRecord(const unsigned char *record, int size)
{
    return size >= ClassicHfsExtentKeySize + ClassicHfsExtentBytes &&
           record[0] == ClassicHfsExtentKeySize - 1;
}
