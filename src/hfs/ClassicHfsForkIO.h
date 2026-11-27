// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Classic fork seeks clamp to the logical file bounds. Compare the signed
// displacement with the available distance before adding it to the base.
static inline int ClassicHfsSeekFork(uint32_t eof, unsigned long *position,
                                    int64_t displacement, int origin)
{
    int64_t base;
    switch (origin) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = *position; break;
    case SEEK_END: base = eof; break;
    default: return 0;
    }
    if (displacement <= -base)
        *position = 0;
    else if (displacement >= (int64_t)eof - base)
        *position = eof;
    else
        *position = (unsigned long)(base + displacement);
    return 1;
}

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

typedef int (*ClassicHfsGrowFork)(void *);

// Caller checks access and the format's logical length limit before writing.
static inline int32_t ClassicHfsWriteFork(void *context, ClassicHfsReadForkBlock load,
                                         int (*store)(void *, uint32_t, unsigned char (*)[512]),
                                         ClassicHfsGrowFork grow, unsigned long *physical,
                                         unsigned long *logical, unsigned long *position,
                                         void *source, uint32_t length)
{
    unsigned char *input = (unsigned char *)source;
    uint32_t done = 0;
    while (done < length) {
        uint32_t sector = (uint32_t)(*position / 512);
        unsigned offset = *position % 512;
        unsigned chunk = 512 - offset;
        if (chunk > length - done)
            chunk = length - done;
        if (*position + chunk > *physical && grow(context) < 0)
            return -1;
        unsigned char scratch[512];
        unsigned char (*bytes)[512] = (unsigned char (*)[512])(input + done);
        if (chunk != 512) {
            if (load(context, sector, &scratch) < 0)
                return -1;
            memcpy(scratch + offset, input + done, chunk);
            bytes = &scratch;
        }
        if (store(context, sector, bytes) < 0)
            return -1;
        done += chunk;
        *position += chunk;
        if (*position > *logical)
            *logical = *position;
    }
    return (int32_t)done;
}
