// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"
#include <string.h>

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

// Remove zero-key records in place. Validate the original table before
// touching either the payload or the caller's offsets.
static inline int ClassicHfsCompactNode(unsigned char *bytes, uint16_t *offsets, uint16_t *count)
{
    if (!ClassicHfsValidOffsets(*count, offsets))
        return 0;
    uint16_t writeOffset = ClassicHfsNodeHeaderSize;
    unsigned kept = 0;
    for (unsigned i = 0; i < *count; ++i) {
        uint16_t start = offsets[i];
        uint16_t length = offsets[i + 1] - start;
        if (!bytes[start])
            continue;
        if (start != writeOffset)
            memmove(bytes + writeOffset, bytes + start, length);
        offsets[kept++] = writeOffset;
        writeOffset += length;
    }
    offsets[kept] = writeOffset;
    *count = (uint16_t)kept;
    return 1;
}

// Insert an external record at a slot, leaving room for the enlarged offset table.
static inline int ClassicHfsInsertNodeRecord(unsigned char *bytes, uint16_t *offsets,
                                             uint16_t *count, unsigned slot,
                                             const unsigned char *record, unsigned length)
{
    if (*count >= ClassicHfsMaxRecords || slot > *count || !length || (length & 1) ||
        !ClassicHfsValidOffsets(*count, offsets))
        return 0;
    unsigned end = offsets[*count];
    unsigned limit = ClassicHfsNodeSize - 2 * (*count + 2);
    if (end > limit || length > limit - end)
        return 0;
    unsigned start = offsets[slot];
    memmove(bytes + start + length, bytes + start, end - start);
    memcpy(bytes + start, record, length);
    for (unsigned i = *count + 1; i > slot; --i)
        offsets[i] = (uint16_t)(offsets[i - 1] + length);
    ++*count;
    return 1;
}

// Append a sibling's records, retaining the left node's descriptor and the
// original record order. Both offset tables refer to their own node buffer.
static inline int ClassicHfsMergeNodeRecords(unsigned char *left, uint16_t *offsets,
                                             uint16_t *count, const unsigned char *right,
                                             const uint16_t *rightOffsets, unsigned rightCount)
{
    if (!ClassicHfsValidOffsets(*count, offsets) ||
        !ClassicHfsValidOffsets(rightCount, rightOffsets))
        return 0;
    unsigned total = *count + rightCount;
    if (total > ClassicHfsMaxRecords)
        return 0;
    unsigned end = offsets[*count];
    unsigned start = rightOffsets[0];
    unsigned length = rightOffsets[rightCount] - start;
    unsigned limit = ClassicHfsNodeSize - 2 * (total + 1);
    if (end + length > limit)
        return 0;
    memcpy(left + end, right + start, length);
    for (unsigned i = 1; i <= rightCount; ++i)
        offsets[*count + i] = (uint16_t)(end + rightOffsets[i] - start);
    *count = (uint16_t)total;
    return 1;
}

// Search a decoded node for its last live key <= the requested key.
static inline int ClassicHfsSearchNode(unsigned char *bytes, const uint16_t *offsets,
                                       int count, unsigned char *key,
                                       int (*compare)(unsigned char *, unsigned char *),
                                       int *predecessor)
{
    int first = 0, end = count, exact = 0;
    *predecessor = -1;
    while (first < end) {
        int middle = first + (end - first) / 2;
        int live = middle;
        while (live >= first && !bytes[offsets[live]])
            --live;
        if (live < first) {
            first = middle + 1;
            continue;
        }
        int order = compare(bytes + offsets[live], key);
        if (order > 0) {
            end = live;
        } else {
            *predecessor = live;
            exact = order == 0;
            first = middle + 1;
        }
    }
    return exact;
}

typedef enum { ClassicHfsLookup, ClassicHfsInsert, ClassicHfsDelete } ClassicHfsSearchPurpose;

// Index keys occupy fixed-width slots. Validate the source record before
// publishing the padded separator and its big-endian child pointer.
static inline int ClassicHfsWriteIndexRecord(unsigned char *out, unsigned capacity,
                                            const unsigned char *key, unsigned available,
                                            int catalog, uint32_t child)
{
    unsigned width = catalog ? 38 : 8;
    if (!available || !child || capacity < width + 4)
        return 0;
    unsigned length = key[0];
    if (length >= available ||
        (catalog ? (length < 6 || length > 37 || key[6] > length - 6) : length != 7))
        return 0;
    memmove(out + 1, key + 1, length);
    out[0] = (unsigned char)(width - 1);
    memset(out + 1 + length, 0, width - 1 - length);
    ClassicHfsWrite32(out + width, child);
    return (int)(width + 4);
}

enum { ClassicHfsInvalidSearch = -1, ClassicHfsMissingKey, ClassicHfsFoundKey, ClassicHfsVisitChild };

// Decide one level of a root-to-leaf lookup on a decoded node. The caller
// decrements the expected height for every child read, bounding the descent.
static inline int ClassicHfsLookupLevel(unsigned char *bytes, const ClassicHfsNodeDescriptor *node,
                                        const uint16_t *offsets, unsigned height, int catalog,
                                        unsigned char *key, int (*compare)(unsigned char *, unsigned char *),
                                        int *slot, uint32_t totalNodes, uint32_t *child,
                                        ClassicHfsSearchPurpose purpose)
{
    if (!height || (unsigned)node->ndNHeight != height ||
        node->ndType != (height == 1 ? -1 : 0))
        return ClassicHfsInvalidSearch;
    for (unsigned i = 0; i < node->ndNRecs; ++i) {
        unsigned char *record = bytes + offsets[i];
        unsigned keyLength = record[0];
        if (purpose != ClassicHfsLookup &&
            (!keyLength || (height > 1 && catalog && keyLength != 37)))
            return ClassicHfsInvalidSearch;
        if (!keyLength)
            continue;
        unsigned padded = (keyLength + 2) & ~1u;
        if (padded + (height > 1 ? 4u : 0u) > (unsigned)(offsets[i + 1] - offsets[i]))
            return ClassicHfsInvalidSearch;
        if (catalog ? (keyLength < 6 || keyLength > 37 || record[6] > keyLength - 6) : keyLength != 7)
            return ClassicHfsInvalidSearch;
    }
    int exact = ClassicHfsSearchNode(bytes, offsets, node->ndNRecs, key, compare, slot);
    if (height == 1 ||
        (*slot < 0 && purpose != ClassicHfsInsert))
        return exact ? ClassicHfsFoundKey : ClassicHfsMissingKey;
    if (!node->ndNRecs)
        return ClassicHfsInvalidSearch;
    unsigned char *record = bytes + offsets[*slot < 0 ? 0 : *slot];
    uint32_t next = ClassicHfsRead32(record + ((record[0] + 2) & ~1u));
    if (!next || next >= totalNodes)
        return ClassicHfsInvalidSearch;
    *child = next;
    return ClassicHfsVisitChild;
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
