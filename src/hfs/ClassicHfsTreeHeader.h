// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"
#include <string.h>

enum { ClassicHfsTreeHeaderBytes = 106 };

#pragma pack(push, 1)
typedef struct {
    uint16_t bthDepth;
    uint32_t bthRoot;
    uint32_t bthNRecs;
    uint32_t bthFNode;
    uint32_t bthLNode;
    uint16_t bthNodeSize;
    uint16_t bthKeyLen;
    uint32_t bthNNodes;
    uint32_t bthFree;
    unsigned char bthResv[76];
} ClassicHfsTreeHeader;
#pragma pack(pop)

// Returns zero for an empty tree, one for a searchable tree, and -1 when
// the root cannot describe the advertised depth or set of leaf records.
static inline int ClassicHfsCheckSearchRoot(const ClassicHfsTreeHeader *header)
{
    if (!header->bthRoot)
        return header->bthDepth || header->bthNRecs || header->bthFNode || header->bthLNode ? -1 : 0;
    if (!header->bthDepth || header->bthRoot >= header->bthNNodes ||
        header->bthDepth >= header->bthNNodes)
        return -1;
    return 1;
}

static inline int ClassicHfsDecodeTreeHeader(const unsigned char *bytes, int size,
                                             ClassicHfsTreeHeader *header)
{
    if (size < ClassicHfsTreeHeaderBytes)
        return 0;
    header->bthDepth = ClassicHfsRead16(bytes + 0);
    header->bthRoot = ClassicHfsRead32(bytes + 2);
    header->bthNRecs = ClassicHfsRead32(bytes + 6);
    header->bthFNode = ClassicHfsRead32(bytes + 10);
    header->bthLNode = ClassicHfsRead32(bytes + 14);
    header->bthNodeSize = ClassicHfsRead16(bytes + 18);
    header->bthKeyLen = ClassicHfsRead16(bytes + 20);
    header->bthNNodes = ClassicHfsRead32(bytes + 22);
    header->bthFree = ClassicHfsRead32(bytes + 26);
    memcpy(header->bthResv, bytes + 30, sizeof(header->bthResv));
    return 1;
}

static inline int ClassicHfsEncodeTreeHeader(unsigned char *bytes, int size,
                                             const ClassicHfsTreeHeader *header)
{
    if (size < ClassicHfsTreeHeaderBytes)
        return 0;
    ClassicHfsWrite16(bytes + 0, header->bthDepth);
    ClassicHfsWrite32(bytes + 2, header->bthRoot);
    ClassicHfsWrite32(bytes + 6, header->bthNRecs);
    ClassicHfsWrite32(bytes + 10, header->bthFNode);
    ClassicHfsWrite32(bytes + 14, header->bthLNode);
    ClassicHfsWrite16(bytes + 18, header->bthNodeSize);
    ClassicHfsWrite16(bytes + 20, header->bthKeyLen);
    ClassicHfsWrite32(bytes + 22, header->bthNNodes);
    ClassicHfsWrite32(bytes + 26, header->bthFree);
    memcpy(bytes + 30, header->bthResv, sizeof(header->bthResv));
    return 1;
}
