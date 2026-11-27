// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsNames.h"
#include <string.h>

enum { ClassicHfsPathEmpty = -3, ClassicHfsPathWrongVolume, ClassicHfsPathTooLong,
       ClassicHfsPathRelative, ClassicHfsPathAbsolute };
enum { ClassicHfsPathEnd, ClassicHfsPathParent, ClassicHfsPathName };

static inline int ClassicHfsBeginPath(const char **cursor, const char *volume, int ignoreLabel)
{
    const char *path = *cursor;
    if (!*path)
        return ClassicHfsPathEmpty;
    const char *colon = strchr(path, ':');
    if (!colon || colon == path || ignoreLabel) {
        if (colon)
            *cursor = colon + 1;
        return ClassicHfsPathRelative;
    }
    char label[28];
    size_t length = (size_t)(colon - path);
    if (length >= sizeof(label))
        return ClassicHfsPathTooLong;
    memcpy(label, path, length);
    label[length] = 0;
    // For an absolute path the volume name is also its first catalog entry.
    return ClassicHfsCompareNames(volume, label) ? ClassicHfsPathWrongVolume : ClassicHfsPathAbsolute;
}

static inline int ClassicHfsNextPathPart(const char **cursor, char name[32])
{
    const char *path = *cursor;
    if (!*path)
        return ClassicHfsPathEnd;
    if (*path == ':') {
        *cursor = path + 1;
        return ClassicHfsPathParent;
    }
    size_t length = strcspn(path, ":");
    if (length > 31)
        return ClassicHfsPathTooLong;
    memcpy(name, path, length);
    name[length] = 0;
    *cursor = path + length + (path[length] == ':');
    return ClassicHfsPathName;
}
