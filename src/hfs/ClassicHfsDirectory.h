// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsBytes.h"

enum { ClassicHfsDirectoryBodySize = 68 };

#pragma pack(push, 1)
typedef struct {
    int16_t dirFlags;
    uint16_t dirVal;
    uint32_t dirDirID;
    int32_t dirCrDat, dirMdDat, dirBkDat;
    struct {
        struct { int16_t top, left, bottom, right; } frRect;
        int16_t frFlags;
        struct { int16_t v, h; } frLocation;
        int16_t frView;
    } dirUsrInfo;
    struct {
        struct { int16_t v, h; } frScroll;
        int32_t frOpenChain;
        int16_t frUnused, frComment;
        int32_t frPutAway;
    } dirFndrInfo;
    int32_t dirResrv[4];
} ClassicHfsDirectory;
#pragma pack(pop)

static inline void ClassicHfsDecodeDirectory(const unsigned char *bytes, ClassicHfsDirectory *dir)
{
    dir->dirFlags = ClassicHfsRead16(bytes);
    dir->dirVal = ClassicHfsRead16(bytes + 2);
    dir->dirDirID = ClassicHfsRead32(bytes + 4);
    dir->dirCrDat = ClassicHfsRead32(bytes + 8);
    dir->dirMdDat = ClassicHfsRead32(bytes + 12);
    dir->dirBkDat = ClassicHfsRead32(bytes + 16);
    dir->dirUsrInfo.frRect.top = ClassicHfsRead16(bytes + 20);
    dir->dirUsrInfo.frRect.left = ClassicHfsRead16(bytes + 22);
    dir->dirUsrInfo.frRect.bottom = ClassicHfsRead16(bytes + 24);
    dir->dirUsrInfo.frRect.right = ClassicHfsRead16(bytes + 26);
    dir->dirUsrInfo.frFlags = ClassicHfsRead16(bytes + 28);
    dir->dirUsrInfo.frLocation.v = ClassicHfsRead16(bytes + 30);
    dir->dirUsrInfo.frLocation.h = ClassicHfsRead16(bytes + 32);
    dir->dirUsrInfo.frView = ClassicHfsRead16(bytes + 34);
    dir->dirFndrInfo.frScroll.v = ClassicHfsRead16(bytes + 36);
    dir->dirFndrInfo.frScroll.h = ClassicHfsRead16(bytes + 38);
    dir->dirFndrInfo.frOpenChain = ClassicHfsRead32(bytes + 40);
    dir->dirFndrInfo.frUnused = ClassicHfsRead16(bytes + 44);
    dir->dirFndrInfo.frComment = ClassicHfsRead16(bytes + 46);
    dir->dirFndrInfo.frPutAway = ClassicHfsRead32(bytes + 48);
    for (unsigned i = 0; i < 4; ++i)
        dir->dirResrv[i] = ClassicHfsRead32(bytes + 52 + 4 * i);
}

static inline void ClassicHfsEncodeDirectory(unsigned char *bytes, const ClassicHfsDirectory *dir)
{
    ClassicHfsWrite16(bytes, dir->dirFlags);
    ClassicHfsWrite16(bytes + 2, dir->dirVal);
    ClassicHfsWrite32(bytes + 4, dir->dirDirID);
    ClassicHfsWrite32(bytes + 8, dir->dirCrDat);
    ClassicHfsWrite32(bytes + 12, dir->dirMdDat);
    ClassicHfsWrite32(bytes + 16, dir->dirBkDat);
    ClassicHfsWrite16(bytes + 20, dir->dirUsrInfo.frRect.top);
    ClassicHfsWrite16(bytes + 22, dir->dirUsrInfo.frRect.left);
    ClassicHfsWrite16(bytes + 24, dir->dirUsrInfo.frRect.bottom);
    ClassicHfsWrite16(bytes + 26, dir->dirUsrInfo.frRect.right);
    ClassicHfsWrite16(bytes + 28, dir->dirUsrInfo.frFlags);
    ClassicHfsWrite16(bytes + 30, dir->dirUsrInfo.frLocation.v);
    ClassicHfsWrite16(bytes + 32, dir->dirUsrInfo.frLocation.h);
    ClassicHfsWrite16(bytes + 34, dir->dirUsrInfo.frView);
    ClassicHfsWrite16(bytes + 36, dir->dirFndrInfo.frScroll.v);
    ClassicHfsWrite16(bytes + 38, dir->dirFndrInfo.frScroll.h);
    ClassicHfsWrite32(bytes + 40, dir->dirFndrInfo.frOpenChain);
    ClassicHfsWrite16(bytes + 44, dir->dirFndrInfo.frUnused);
    ClassicHfsWrite16(bytes + 46, dir->dirFndrInfo.frComment);
    ClassicHfsWrite32(bytes + 48, dir->dirFndrInfo.frPutAway);
    for (unsigned i = 0; i < 4; ++i)
        ClassicHfsWrite32(bytes + 52 + 4 * i, dir->dirResrv[i]);
}
