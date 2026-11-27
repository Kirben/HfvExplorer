// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsDirectory.h"
#include "ClassicHfsCatalogFile.h"
#include "ClassicHfsCatalogThread.h"

typedef enum { cdrDirRec = 1, cdrFilRec, cdrThdRec, cdrFThdRec } CatDataType;
#pragma pack(push, 1)
typedef struct {
    int8_t cdrType, cdrResrv2;
    union {
        ClassicHfsDirectory dir;
        ClassicHfsCatalogFile fil;
        ClassicHfsCatalogThread dthd;
    } u;
} ClassicHfsCatalogRecord;
#pragma pack(pop)

static inline unsigned ClassicHfsRecordSize(int type)
{
    switch (type) {
    case cdrDirRec: return 2 + ClassicHfsDirectoryBodySize;
    case cdrFilRec: return 2 + ClassicHfsFileBodySize;
    case cdrThdRec: case cdrFThdRec: return 2 + ClassicHfsThreadBodySize;
    default: return 0;
    }
}

static inline unsigned ClassicHfsEncodeCatalogRecord(unsigned char *bytes, size_t capacity,
                                                     const ClassicHfsCatalogRecord *record)
{
    unsigned size = ClassicHfsRecordSize(record->cdrType);
    if (!size || capacity < size)
        return 0;
    bytes[0] = record->cdrType;
    bytes[1] = record->cdrResrv2;
    switch (record->cdrType) {
    case cdrDirRec: ClassicHfsEncodeDirectory(bytes + 2, &record->u.dir); break;
    case cdrFilRec: ClassicHfsEncodeFile(bytes + 2, &record->u.fil); break;
    default: ClassicHfsEncodeThread(bytes + 2, &record->u.dthd); break;
    }
    return size;
}

static inline unsigned ClassicHfsDecodeCatalogRecord(unsigned char *bytes, size_t available,
                                                     ClassicHfsCatalogRecord *record)
{
    unsigned size = available ? ClassicHfsRecordSize(bytes[0]) : 0;
    if (!size || available < size)
        return 0;
    record->cdrType = (int8_t)bytes[0];
    record->cdrResrv2 = (int8_t)bytes[1];
    switch (record->cdrType) {
    case cdrDirRec: ClassicHfsDecodeDirectory(bytes + 2, &record->u.dir); break;
    case cdrFilRec: ClassicHfsDecodeFile(bytes + 2, &record->u.fil); break;
    default: ClassicHfsDecodeThread(bytes + 2, &record->u.dthd); break;
    }
    return size;
}
