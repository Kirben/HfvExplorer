// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <string.h>

typedef int (*ClassicHfsReadForkBlock)(void *, uint32_t, unsigned char (*)[512]);

// The public API returns a signed 32-bit transfer count. On error its
// position reflects only the bytes from successfully completed sectors.
static inline int32_t ClassicHfsReadFork(void *context, ClassicHfsReadForkBlock load,
                                        uint32_t eof, unsigned long *position,
                                        void *destination, uint32_t requested)
{
    if (*position >= eof)
        return 0;
    uint32_t length = eof - (uint32_t)*position;
    if (length > requested)
        length = requested;
    if (length > INT32_MAX)
        length = INT32_MAX;
    unsigned char *out = (unsigned char *)destination;
    uint32_t done = 0;
    while (done < length) {
        unsigned offset = *position % 512;
        unsigned chunk = 512 - offset;
        if (chunk > length - done)
            chunk = length - done;
        unsigned char temporary[512];
        unsigned char (*buffer)[512] = chunk == 512 ? (unsigned char (*)[512])(out + done) : &temporary;
        if (load(context, (uint32_t)(*position / 512), buffer) < 0)
            return -1;
        if (chunk != 512)
            memcpy(out + done, temporary + offset, chunk);
        done += chunk;
        *position += chunk;
    }
    return (int32_t)done;
}
