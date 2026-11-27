// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ClassicHfsRecordBytes.h"

// Directory and file threads have identical fields after the two type bytes.
#pragma pack(push, 1)
typedef struct {
    uint32_t thdResrv[2];
    uint32_t thdParID;
    char thdCName[32];
} ClassicHfsCatalogThread;
#pragma pack(pop)

enum { ClassicHfsThreadBodySize = 44 };

static inline void ClassicHfsDecodeThread(unsigned char *bytes, ClassicHfsCatalogThread *thread)
{
    thread->thdResrv[0] = ClassicHfsRead32(bytes);
    thread->thdResrv[1] = ClassicHfsRead32(bytes + 4);
    thread->thdParID = ClassicHfsRead32(bytes + 8);
    bytes += 12;
    ClassicHfsTakeName(&bytes, thread->thdCName, sizeof(thread->thdCName));
}

static inline void ClassicHfsEncodeThread(unsigned char *bytes, const ClassicHfsCatalogThread *thread)
{
    ClassicHfsWrite32(bytes, thread->thdResrv[0]);
    ClassicHfsWrite32(bytes + 4, thread->thdResrv[1]);
    ClassicHfsWrite32(bytes + 8, thread->thdParID);
    bytes += 12;
    ClassicHfsAppendName(&bytes, thread->thdCName, sizeof(thread->thdCName));
}
