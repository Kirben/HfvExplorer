// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "StorageLocation.h"
#include "LegacyEngine.h"
#include "ReadOnlyCdBackend.h"
#include "ReadOnlyFloppy.h"
#include "VolumeCoordinator.h"

namespace StorageLocation {
namespace {
void Cancel(Storage::Job& job) {
    if (job.cancelled()) throw Storage::Cancelled();
}

std::string FolderIdentity(std::string identity) {
    for (int i=0; i<2; ++i) {
        const auto colon=identity.find_last_of(':');
        if (colon==identity.npos) return {};
        identity.resize(colon);
    }
    return identity;
}

void ValidateSelection(const Location& location, const MacBrowser::Entry& entry) {
    if (!MatchesEntry(location,entry))
        throw HfsPlus::Error("The selected entry was replaced, renamed or moved.");
}
}

bool RevisionCurrent(const std::string& key, uint64_t revision) {
    return key.empty() || VolumeCoord::Revision(key)==revision;
}

std::optional<DWORD> WindowsSerial(const std::wstring& path) {
    wchar_t root[MAX_PATH]{};
    DWORD serial=0;
    if (!GetVolumePathNameW(path.c_str(),root,ARRAYSIZE(root)) ||
        !GetVolumeInformationW(root,nullptr,0,&serial,nullptr,nullptr,nullptr,0))
        return {};
    return serial;
}

void ValidateImage(const std::wstring& path, const std::string& expectedResource) {
    if (!expectedResource.empty() && VolumeCoord::ImageKey(path)!=expectedResource)
        throw Storage::MediaChanged("The selected image was replaced.");
}

void ValidateClassicGeneration(const Location& location, std::optional<DWORD> observed) {
    if (location.MediaGenerationKnown() && observed!=location.MediaGeneration())
        throw Storage::MediaChanged("The selected media changed. Select its drive again.");
}

void ValidateMedia(const Location& location, FloppyProbe probe) {
    if (location.IsWindows()) {
        const auto serial=location.WindowsMediaSerial();
        if (serial && WindowsSerial(location.WindowsPath())!=serial)
            throw Storage::MediaChanged("The selected Windows media changed.");
    } else if (location.IsMounted()) {
        location.Volume()->ValidateMedia();
    } else if (location.MediaGenerationKnown()) {
        const auto drive=location.Drive();
        DWORD generation=0;
        bool valid=false;
        if (location.Media()==FileOperations::ClassicMedia::Cd) {
            valid=physical_cd_generation(drive,&generation)!=0;
        } else if (probe==FloppyProbe::OpenMount) {
            valid=physical_floppy_generation(drive,&generation)!=0;
        } else {
            FloppyReadApi* api=nullptr;
#ifdef HFV_FLOPPY_TESTING
            api=physical_floppy_test_api(drive);
            if (!api) throw Storage::MediaChanged("The floppy is no longer attached.");
#endif
            ReadOnlyFloppy floppy(api);
            valid=floppy.OpenDrive(drive);
            if (valid) generation=floppy.Generation();
        }
        ValidateClassicGeneration(location,valid?std::optional<DWORD>(generation):std::nullopt);
    }
}

bool MatchesEntry(const Location& location, const MacBrowser::Entry& entry) {
    const auto catalogId=entry.resolvedId?entry.resolvedId:entry.id;
    return (!location.expectedCatalogId || location.expectedCatalogId==catalogId) &&
           (location.expectedIdentity.empty() || location.expectedIdentity==entry.identity) &&
           (location.expectedName.empty() || location.expectedName==entry.name) &&
           (!location.expectedParent || location.expectedParent==entry.parent);
}

void ValidateFolderIdentity(const Location& location, const MacBrowser::Entry& entry) {
    if (location.expectedIdentity.empty()) return;
    const auto stableId=FolderIdentity(entry.identity);
    if (stableId.empty() || stableId!=FolderIdentity(location.expectedIdentity))
        throw HfsPlus::Error("The destination folder was replaced.");
}

ResolvedEntry ResolveEntry(const Location& location, bool writable) {
    if (location.IsMounted()) {
        ValidateMedia(location);
        auto entry=location.Volume()->Stat(location.Id());
        ValidateSelection(location,entry);
        return {location.Volume(),location.Id(),std::move(entry)};
    }
    if (location.IsWindows()) {
        ValidateMedia(location);
        auto [volume,id]=MacBrowser::ResolveWindowsFile(location.WindowsPath());
        auto entry=volume->Stat(id);
        ValidateSelection(location,entry);
        return {std::move(volume),id,std::move(entry)};
    }
    if (!location.IsClassic()) throw HfsPlus::Error("Select a file or folder.");
    ValidateImage(VolumeCoord::Wide(location.ClassicImage()),location.expectedResource);
    const auto& path=location.ClassicPath();
    const auto separator=path.find_last_of(':');
    auto volume=MacBrowser::OpenClassic(location.ClassicImage(),path.substr(0,separator+1),writable);
    ValidateMedia(location);
    const auto name=MacBrowser::U16(VolumeCoord::Wide(path.substr(separator+1)));
    for (auto& entry:volume->List()) {
        if (HfsPlus::CompareNames(entry.name,name,volume->CaseSensitive())!=0) continue;
        ValidateSelection(location,entry);
        const auto id=entry.id;
        return {std::move(volume),id,std::move(entry)};
    }
    throw HfsPlus::Error("The selected source no longer exists.");
}

std::vector<uint8_t> ReadDataPrefix(const Location& location, size_t maximum, bool& truncated,
                                    Storage::Job& job) {
    Cancel(job);
    LegacyEngine::Scope engine;
    std::vector<VolumeCoord::Access> access{{location.ResourceKey(),VolumeCoord::Mode::Read}};
    const auto path=location.HostPath();
    if (!path.empty()) {
        const auto separator=path.find_last_of(L"\\/");
        const auto companion=path.substr(0,separator+1)+L"%"+path.substr(separator+1);
        access.push_back({VolumeCoord::PathKey(companion),VolumeCoord::Mode::Read});
    }
    VolumeCoord::Lease lease(std::move(access));
    Cancel(job);
    auto source=ResolveEntry(location);
    const auto& before=source.entry;
    if (before.directory || !before.readError.empty()) throw HfsPlus::Error("The selected file cannot be read.");
    const auto size=before.forks[0].size;
    std::vector<uint8_t> bytes(size_t((std::min)(size,uint64_t(maximum))));
    if (!bytes.empty()) source.volume->Read(source.id,0,0,bytes.data(),bytes.size());
    Cancel(job);
    ValidateMedia(location,FloppyProbe::FreshHandle);
    if (location.IsClassic()) ValidateImage(VolumeCoord::Wide(location.ClassicImage()),location.expectedResource);
    const auto after=source.volume->Stat(source.id);
    if (before.identity!=after.identity || before.name!=after.name || before.parent!=after.parent ||
        before.modified!=after.modified || size!=after.forks[0].size)
        throw HfsPlus::Error("The selected file changed while reading.");
    if (!location.IsMounted()) source.volume->Close();
    truncated=size>bytes.size();
    return bytes;
}
}
