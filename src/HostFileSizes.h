// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include "HostOffsets.h"
inline hfv_size_t HostDirectoryEntrySize(const WIN32_FIND_DATAA& entry) {
 // Directory callers use the low field for their separately computed item count.
 if(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)return entry.nFileSizeLow;
 return (hfv_size_t(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow;
}