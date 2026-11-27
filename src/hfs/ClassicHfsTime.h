// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
uint32_t ClassicHfsFromUnixSeconds(uint32_t seconds);
uint32_t ClassicHfsToUnixSeconds(uint32_t seconds);
#ifdef __cplusplus
}
#endif
