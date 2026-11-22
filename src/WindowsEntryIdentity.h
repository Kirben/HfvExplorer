// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include "EntryIdentity.h"

namespace Storage { class Job; }

namespace MacBrowser {
inline ObjectIdentity WindowsIdentity(const BY_HANDLE_FILE_INFORMATION& info)
{
    return {std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+
            std::to_string(info.nFileIndexLow),
            (uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime};
}

}

int DeleteHostSelectionJob(const std::wstring& path,bool directory,
                           const MacBrowser::EntryIdentity& expected,Storage::Job* job);
