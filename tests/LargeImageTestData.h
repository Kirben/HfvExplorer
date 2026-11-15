// Host-only test manifest; never serialized into an HFS volume.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef HFV_LARGE_IMAGE_TEST_DATA_H
#define HFV_LARGE_IMAGE_TEST_DATA_H
#include <stdint.h>
typedef struct { uint16_t start, count; } TestExtent;
typedef struct {
    char name[32];
    uint32_t dataSize, resourceSize, seed;
    TestExtent data[3], resource[3];
} LargeImageCase;
typedef struct {
    char magic[8], image[1024];
    uint64_t imageSize;
    uint32_t allocationSize, allocationStart;
    LargeImageCase cases[2];
} LargeImageManifest;
static unsigned char LargeImagePattern(uint32_t offset,uint32_t seed) {
    return (unsigned char)(offset*37+seed);
}
#endif
