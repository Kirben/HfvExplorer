// SPDX-License-Identifier: GPL-2.0-or-later
#include "ClassicHfsTime.h"
#include <time.h>

// The classic adapter uses the current local UTC offset in both directions.
// Keep it stable within the worker thread, including across a DST transition.
static uint32_t ClassicEpochOffset(void)
{
    static __declspec(thread) uint32_t offset;
    static __declspec(thread) int initialized;
    if (!initialized) {
        time_t now = time(NULL);
        struct tm local;
        int64_t east = 0;
        if (localtime_s(&local, &now) == 0) {
            // Interpret today's local wall clock as UTC to obtain its offset.
            __time64_t wall = _mkgmtime64(&local);
            if (wall != -1)
                east = wall - now;
        }
        offset = (uint32_t)(INT64_C(2082844800) + east);
        initialized = 1;
    }
    return offset;
}

uint32_t ClassicHfsFromUnixSeconds(uint32_t seconds)
{
    return seconds + ClassicEpochOffset();
}

uint32_t ClassicHfsToUnixSeconds(uint32_t seconds)
{
    return seconds - ClassicEpochOffset();
}
