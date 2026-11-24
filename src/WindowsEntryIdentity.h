// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include "EntryIdentity.h"
#include <vector>
#include <functional>
#include <map>

namespace Storage { class Job; }

namespace MacBrowser {
inline ObjectIdentity WindowsIdentity(const BY_HANDLE_FILE_INFORMATION& info)
{
    return {std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+
            std::to_string(info.nFileIndexLow),
            (uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime};
}

struct WindowsCutEntry {
    std::wstring path;
    EntryIdentity identity;
    bool directory;
    std::optional<std::string> primaryStreams,companionStreams;
};
using WindowsCutPlan=std::vector<WindowsCutEntry>;
using WindowsPublications=std::map<std::wstring,EntryIdentity>;
void TrackWindowsPublications(WindowsPublications&,const std::function<void()>&);
class Volume;
void FinishWindowsCut(Volume&,const WindowsCutPlan&,Storage::Job&);
}

int DeleteHostSelectionJob(const std::wstring& path,bool directory,
                           const MacBrowser::EntryIdentity& expected,Storage::Job* job);
int DeleteHostCutEntry(const MacBrowser::WindowsCutEntry&,Storage::Job&);
