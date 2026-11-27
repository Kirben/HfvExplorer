// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsExtents.h"

enum { ClassicHfsFileBodySize = 100 };

#pragma pack(push, 1)
typedef struct {
    int8_t filFlags, filTyp;
    struct {
        int32_t fdType, fdCreator;
        int16_t fdFlags;
        struct { int16_t v, h; } fdLocation;
        int16_t fdFldr;
    } filUsrWds;
    uint32_t filFlNum;
    uint16_t filStBlk;
    unsigned long filLgLen, filPyLen;
    uint16_t filRStBlk;
    unsigned long filRLgLen, filRPyLen;
    int32_t filCrDat, filMdDat, filBkDat;
    struct {
        int16_t fdIconID, fdUnused[4], fdComment;
        int32_t fdPutAway;
    } filFndrInfo;
    uint16_t filClpSize;
    ClassicHfsExtent filExtRec[3], filRExtRec[3];
    int32_t filResrv;
} ClassicHfsCatalogFile;
#pragma pack(pop)

static inline void ClassicHfsDecodeFile(const unsigned char *bytes, ClassicHfsCatalogFile *file)
{
    file->filFlags = (int8_t)bytes[0];
    file->filTyp = (int8_t)bytes[1];
    file->filUsrWds.fdType = ClassicHfsRead32(bytes + 2);
    file->filUsrWds.fdCreator = ClassicHfsRead32(bytes + 6);
    file->filUsrWds.fdFlags = ClassicHfsRead16(bytes + 10);
    file->filUsrWds.fdLocation.v = ClassicHfsRead16(bytes + 12);
    file->filUsrWds.fdLocation.h = ClassicHfsRead16(bytes + 14);
    file->filUsrWds.fdFldr = ClassicHfsRead16(bytes + 16);
    file->filFlNum = ClassicHfsRead32(bytes + 18);
    file->filStBlk = ClassicHfsRead16(bytes + 22);
    file->filLgLen = ClassicHfsRead32(bytes + 24);
    file->filPyLen = ClassicHfsRead32(bytes + 28);
    file->filRStBlk = ClassicHfsRead16(bytes + 32);
    file->filRLgLen = ClassicHfsRead32(bytes + 34);
    file->filRPyLen = ClassicHfsRead32(bytes + 38);
    file->filCrDat = ClassicHfsRead32(bytes + 42);
    file->filMdDat = ClassicHfsRead32(bytes + 46);
    file->filBkDat = ClassicHfsRead32(bytes + 50);
    file->filFndrInfo.fdIconID = ClassicHfsRead16(bytes + 54);
    for (unsigned i = 0; i < 4; ++i)
        file->filFndrInfo.fdUnused[i] = ClassicHfsRead16(bytes + 56 + 2 * i);
    file->filFndrInfo.fdComment = ClassicHfsRead16(bytes + 64);
    file->filFndrInfo.fdPutAway = ClassicHfsRead32(bytes + 66);
    file->filClpSize = ClassicHfsRead16(bytes + 70);
    ClassicHfsDecodeExtents(bytes + 72, file->filExtRec);
    ClassicHfsDecodeExtents(bytes + 84, file->filRExtRec);
    file->filResrv = ClassicHfsRead32(bytes + 96);
}

static inline void ClassicHfsEncodeFile(unsigned char *bytes, const ClassicHfsCatalogFile *file)
{
    bytes[0] = (unsigned char)file->filFlags;
    bytes[1] = (unsigned char)file->filTyp;
    ClassicHfsWrite32(bytes + 2, file->filUsrWds.fdType);
    ClassicHfsWrite32(bytes + 6, file->filUsrWds.fdCreator);
    ClassicHfsWrite16(bytes + 10, file->filUsrWds.fdFlags);
    ClassicHfsWrite16(bytes + 12, file->filUsrWds.fdLocation.v);
    ClassicHfsWrite16(bytes + 14, file->filUsrWds.fdLocation.h);
    ClassicHfsWrite16(bytes + 16, file->filUsrWds.fdFldr);
    ClassicHfsWrite32(bytes + 18, file->filFlNum);
    ClassicHfsWrite16(bytes + 22, file->filStBlk);
    ClassicHfsWrite32(bytes + 24, file->filLgLen);
    ClassicHfsWrite32(bytes + 28, file->filPyLen);
    ClassicHfsWrite16(bytes + 32, file->filRStBlk);
    ClassicHfsWrite32(bytes + 34, file->filRLgLen);
    ClassicHfsWrite32(bytes + 38, file->filRPyLen);
    ClassicHfsWrite32(bytes + 42, file->filCrDat);
    ClassicHfsWrite32(bytes + 46, file->filMdDat);
    ClassicHfsWrite32(bytes + 50, file->filBkDat);
    ClassicHfsWrite16(bytes + 54, file->filFndrInfo.fdIconID);
    for (unsigned i = 0; i < 4; ++i)
        ClassicHfsWrite16(bytes + 56 + 2 * i, file->filFndrInfo.fdUnused[i]);
    ClassicHfsWrite16(bytes + 64, file->filFndrInfo.fdComment);
    ClassicHfsWrite32(bytes + 66, file->filFndrInfo.fdPutAway);
    ClassicHfsWrite16(bytes + 70, file->filClpSize);
    ClassicHfsEncodeExtents(bytes + 72, file->filExtRec);
    ClassicHfsEncodeExtents(bytes + 84, file->filRExtRec);
    ClassicHfsWrite32(bytes + 96, file->filResrv);
}
