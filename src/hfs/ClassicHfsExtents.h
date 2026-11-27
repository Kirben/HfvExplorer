// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsExtentKey.h"
#include "ClassicHfsNode.h"
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

enum { ClassicHfsCannotAppend = -1, ClassicHfsExtendedRecord, ClassicHfsNewOverflowRecord };

// Append to the final record only if it accounts for exactly the remaining
// physical length. The returned record is staged separately for publication.
static inline int ClassicHfsAppendExtent(const ClassicHfsExtent *current, unsigned recordStart,
                                         unsigned eofBlocks, unsigned volumeBlocks,
                                         ClassicHfsExtent added, ClassicHfsExtent *output)
{
    unsigned first = added.xdrStABN, length = added.xdrNumABlks;
    if (!length || recordStart > eofBlocks || eofBlocks > volumeBlocks ||
        length > volumeBlocks - eofBlocks || first >= volumeBlocks || length > volumeBlocks - first)
        return ClassicHfsCannotAppend;
    unsigned used = 0, logical = recordStart;
    for (unsigned i = 0; i < ClassicHfsExtentCount; ++i) {
        unsigned start = current[i].xdrStABN, size = current[i].xdrNumABlks;
        if (!size)
            continue;
        if (i != used || size > eofBlocks - logical || start >= volumeBlocks ||
            size > volumeBlocks - start || (start < first + length && first < start + size))
            return ClassicHfsCannotAppend;
        logical += size;
        ++used;
    }
    if (logical != eofBlocks)
        return ClassicHfsCannotAppend;
    memcpy(output, current, ClassicHfsExtentCount * sizeof(*output));
    if (used && current[used - 1].xdrStABN + current[used - 1].xdrNumABlks == first) {
        output[used - 1].xdrNumABlks += added.xdrNumABlks;
        return ClassicHfsExtendedRecord;
    }
    if (used < ClassicHfsExtentCount) {
        output[used] = added;
        return ClassicHfsExtendedRecord;
    }
    memset(output, 0, ClassicHfsExtentCount * sizeof(*output));
    output[0] = added;
    return ClassicHfsNewOverflowRecord;
}

// Plan a complete extent record before releasing any of its blocks.
typedef struct {
    ClassicHfsExtent kept[ClassicHfsExtentCount], released[ClassicHfsExtentCount];
    unsigned next;
} ClassicHfsTruncation;

static inline int ClassicHfsPlanTruncation(const ClassicHfsExtent *record, unsigned start,
                                          unsigned keep, unsigned oldEnd, unsigned volumeBlocks,
                                          ClassicHfsTruncation *plan)
{
    ClassicHfsTruncation result = {0};
    if (keep > oldEnd || oldEnd > volumeBlocks || start >= oldEnd)
        return 0;
    unsigned pos = start;
    for (unsigned i = 0; i < ClassicHfsExtentCount; ++i) {
        unsigned first = record[i].xdrStABN, length = record[i].xdrNumABlks;
        if (!length) {
            if (pos != oldEnd)
                return 0;
            continue;
        }
        if (first >= volumeBlocks || length > volumeBlocks - first || length > oldEnd - pos)
            return 0;
        for (unsigned j = 0; j < i; ++j) {
            unsigned other = record[j].xdrStABN;
            if (first < other + record[j].xdrNumABlks && other < first + length)
                return 0;
        }
        unsigned retained = pos < keep ? keep - pos : 0;
        if (retained > length)
            retained = length;
        if (retained) {
            result.kept[i].xdrStABN = (uint16_t)first;
            result.kept[i].xdrNumABlks = (uint16_t)retained;
        }
        if (retained < length) {
            result.released[i].xdrStABN = (uint16_t)(first + retained);
            result.released[i].xdrNumABlks = (uint16_t)(length - retained);
        }
        pos += length;
    }
    result.next = pos;
    *plan = result;
    return 1;
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

static inline unsigned ClassicHfsExtentPayload(const unsigned char *sector, const uint16_t *offsets,
                                              unsigned count, int slot, const unsigned char *key)
{
    if (slot < 0 || (unsigned)slot >= count || !ClassicHfsValidOffsets(count, offsets))
        return 0;
    const unsigned char *record = sector + offsets[slot];
    if (!ClassicHfsValidExtentRecord(record, offsets[slot + 1] - offsets[slot]) ||
        ClassicHfsCompareExtentKeyBytes(record, key))
        return 0;
    return offsets[slot] + ClassicHfsExtentKeySize;
}

enum { ClassicHfsBadExtentLeaf = -1, ClassicHfsExtentMissing, ClassicHfsExtentFound,
       ClassicHfsContinueExtentScan };

// The extents file may need to find its own overflow records before its
// index nodes can be read. Inspect one decoded leaf in forward-link order.
static inline int ClassicHfsScanExtentLeaf(const unsigned char *bytes, const uint16_t *offsets,
                                           unsigned count, const unsigned char *key, int *slot)
{
    if (!ClassicHfsValidOffsets(count, offsets))
        return ClassicHfsBadExtentLeaf;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *record = bytes + offsets[i];
        if (!ClassicHfsValidExtentRecord(record, offsets[i + 1] - offsets[i]))
            return ClassicHfsBadExtentLeaf;
        int order = ClassicHfsCompareExtentKeyBytes(record, key);
        if (!order) {
            *slot = (int)i;
            return ClassicHfsExtentFound;
        }
        if (order > 0)
            return ClassicHfsExtentMissing;
    }
    return ClassicHfsContinueExtentScan;
}
