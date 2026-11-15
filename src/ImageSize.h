// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HostOffsets.h"
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
inline bool ParseImageSize(const char* text, hfv_size_t* result,hfv_size_t maximum=0xffffffffULL*512) {
    if (!text || !result) return false;
    while (isspace((unsigned char)*text)) ++text;
    if (!isdigit((unsigned char)*text)) return false;
    errno = 0; char* end;
    hfv_size_t value = _strtoui64(text, &end, 10), unit = 1;
    if (errno || !value) return false;
    while (isspace((unsigned char)*end)) ++end;
    char suffix[8]; size_t n = strlen(end);
    while (n && isspace((unsigned char)end[n-1])) --n;
    if (n >= sizeof(suffix)) return false;
    memcpy(suffix,end,n); suffix[n] = 0;
    if (!suffix[0] || !_stricmp(suffix,"B")) unit=1;
    else if (!_stricmp(suffix,"K") || !_stricmp(suffix,"KB") || !_stricmp(suffix,"KiB")) unit=1024;
    else if (!_stricmp(suffix,"M") || !_stricmp(suffix,"MB") || !_stricmp(suffix,"MiB")) unit=1024ULL*1024;
    else if (!_stricmp(suffix,"G") || !_stricmp(suffix,"GB") || !_stricmp(suffix,"GiB")) unit=1024ULL*1024*1024;
    else if (!_stricmp(suffix,"T") || !_stricmp(suffix,"TB") || !_stricmp(suffix,"TiB")) unit=1024ULL*1024*1024*1024;
    else return false;
    if (value > maximum/unit) return false;
    value *= unit;
    if (value < 800*1024 || value % 512) return false;
    *result = value; return true;
}
