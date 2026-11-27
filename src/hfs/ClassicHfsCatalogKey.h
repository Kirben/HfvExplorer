// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"
#include "ClassicHfsNames.h"

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
