// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileLocation.h"

namespace StorageLocation {
using FileOperations::Location;

// A storage version and a removable-media identity are checked separately.
bool RevisionCurrent(const std::string& resource, uint64_t revision);
std::optional<DWORD> WindowsSerial(const std::wstring& path);
void ValidateImage(const std::wstring& path, const std::string& expectedResource);
void ValidateClassicGeneration(const Location&, std::optional<DWORD> observed);
enum class FloppyProbe { OpenMount, FreshHandle };
void RequireCapturedIdentity(const Location&);
void ValidateMedia(const Location&, FloppyProbe = FloppyProbe::OpenMount);

bool MatchesEntry(const Location&, const MacBrowser::Entry&);
void ValidateFolderIdentity(const Location&, const MacBrowser::Entry&);

struct ResolvedEntry {
    std::shared_ptr<MacBrowser::Volume> volume;
    uint32_t id;
    MacBrowser::Entry entry;
};
// Callers acquire any operation lease before resolving. Read-only is the
// default for opening a classic image or device.
ResolvedEntry ResolveEntry(const Location&, bool writable = false);

// The callback may open writable filesystem handles. It must release its own
// handles before returning so the backend can finish and verify the write.
void WithVerifiedWrite(const Location&, const std::function<void()>&);
bool ClassicPropertiesWritable(const Location&);

// Recheck every Cut source before deleting any, then reopen legacy media
// under its exclusive write scope for the final identity check and removal.
ResolvedEntry PrepareCutSource(const Location&, const std::string& snapshot, Storage::Job&,
                               const MacBrowser::SnapshotVisitor& visit={});
void FinalizeCutSource(const Location&, ResolvedEntry prepared, const std::string& snapshot,
                       Storage::Job&, const std::function<void(MacBrowser::Volume&,uint32_t)>& remove);
std::vector<uint8_t> ReadDataPrefix(const Location&, size_t maximum, bool& truncated, Storage::Job&);
}
