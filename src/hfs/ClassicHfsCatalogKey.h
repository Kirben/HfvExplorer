// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"
#include "ClassicHfsNames.h"

#include <string.h>

#pragma pack(push, 1)
typedef struct {
    unsigned char ckrKeyLen;
    unsigned char ckrResrv1;
    uint32_t ckrParID;
    char ckrCName[32];
} ClassicHfsCatalogKey;
#pragma pack(pop)

// The catalog uses even-sized records and a Pascal MacRoman name.
static inline int ClassicHfsWriteCatalogKey(unsigned char *bytes, uint32_t parent, const char *name)
{
    size_t length = 0;
    while (length < 32 && name[length])
        ++length;
    if (length == 32)
        return -1;
    unsigned size = (7 + (unsigned)length + 1) & ~1u;
    bytes[0] = (unsigned char)(size - 1);
    bytes[1] = 0;
    ClassicHfsWrite32(bytes + 2, parent);
    bytes[6] = (unsigned char)length;
    memcpy(bytes + 7, name, length);
    if (7 + length < size)
        bytes[7 + length] = 0;
    return (int)size;
}

static inline int ClassicHfsReadCatalogKey(const unsigned char *bytes, int available,
                                          ClassicHfsCatalogKey *key)
{
    if (available < 7 || bytes[0] < 6 || bytes[0] > 37 || bytes[0] >= available ||
        bytes[6] > 31 || bytes[6] > bytes[0] - 6)
        return 0;
    key->ckrKeyLen = bytes[0];
    key->ckrResrv1 = bytes[1];
    key->ckrParID = ClassicHfsRead32(bytes + 2);
    memcpy(key->ckrCName, bytes + 7, bytes[6]);
    key->ckrCName[bytes[6]] = 0;
    return 1;
}

// Compare names in place. A leaf key may end immediately after its name;
// the corresponding index key may have padding out to the maximum width.
static inline unsigned ClassicHfsCatalogNameLength(const unsigned char *key)
{
    if (key[0] < 6)
        return 0;
    unsigned length = key[6];
    return length <= 31 && length <= (unsigned)key[0] - 6 ? length : 0;
}

static inline int ClassicHfsCompareCatalogKeys(unsigned char *left, unsigned char *right)
{
    if (left[0] < 5 || right[0] < 5)
        return (int)(left[0] >= 5) - (int)(right[0] >= 5);
    uint32_t leftParent = ClassicHfsRead32(left + 2);
    uint32_t rightParent = ClassicHfsRead32(right + 2);
    if (leftParent != rightParent)
        return leftParent < rightParent ? -1 : 1;

    unsigned leftLength = ClassicHfsCatalogNameLength(left);
    unsigned rightLength = ClassicHfsCatalogNameLength(right);
    for (unsigned i = 0; i < leftLength && i < rightLength; ++i) {
        int difference = ClassicHfsNameWeight(left[7 + i]) - ClassicHfsNameWeight(right[7 + i]);
        if (difference || !left[7 + i])
            return difference;
    }
    // The existing catalog API treats an embedded NUL as the end of a name.
    if (leftLength < rightLength)
        return right[7 + leftLength] ? -1 : 0;
    if (rightLength < leftLength)
        return left[7 + rightLength] ? 1 : 0;
    return 0;
}
