// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "LargeImageTestData.h"
typedef struct {
    char image[1024], catalog[1024];
    uint32_t fileID, forkSize, catalogSize;
    TestExtent data[3], resource[3];
} FragmentedManifest;
