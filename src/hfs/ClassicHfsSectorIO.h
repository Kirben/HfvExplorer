// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "../Filehook.h"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>

static inline int ClassicHfsSectorOffset(uint32_t start, uint32_t count, uint32_t sector,
                                        int writing, hfv_offset_t *offset)
{
    // Mount and format probe sectors before the volume length is known.
    if (count ? sector >= count : writing)
        return 0;
    *offset = ((hfv_offset_t)start + sector) * 512;
    return 1;
}

static inline int ClassicHfsTransferSector(int fd, hfv_offset_t offset, void *buffer,
                                          int writing, ftype_lseek seekBlock,
                                          ftype_read readBlock, ftype_write writeBlock)
{
    hfv_offset_t position = seekBlock(fd, offset, SEEK_SET);
    if (position < 0)
        return -1;
    if (position == offset) {
        int bytes = writing ? writeBlock(fd, buffer, 512) : readBlock(fd, buffer, 512);
        if (bytes == 512)
            return 0;
        if (bytes < 0)
            return -1;
    }
    errno = EIO;
    return -1;
}
