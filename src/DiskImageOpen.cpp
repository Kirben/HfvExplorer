// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DiskImageOpen.h"
#include "DmgEncryption.h"
#include "DmgImage.h"
#include "NdifImage.h"
#include "SparseDisk.h"
#include "StorageLocation.h"

namespace DiskImage {
#ifdef HFV_TESTING
thread_local std::function<void(const std::wstring&)> beforeReopenTest;
#endif
Opened Open(const std::wstring& path, const OpenOptions& options)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (!options.externalJournal.empty()) return {};
        auto bundle = SparseDisk::OpenBundle(path, options.password);
        return {std::move(bundle), Container::SparseBundle};
    }

    auto source = HfsPlus::OpenImage(path.c_str(), options.writable,
        options.externalJournal.empty() ? nullptr : options.externalJournal.c_str());
    if(!options.expectedImage.empty())
        StorageLocation::ValidateImageFile(reinterpret_cast<HANDLE>(source->WorkerFiles().front().second),
                                           options.expectedImage);
    const auto encrypted = Dmg::Unlock(source, options.password, options.privateKey);
    if (SparseDisk::Unwrap(source))
        return {std::move(source), Container::SparseImage};
    if (Dmg::Unwrap(source))
        return {std::move(source), Container::Udif};
    if (encrypted)
        return {std::move(source), Container::EncryptedDisk};
    if (options.externalJournal.empty() && Ndif::Unwrap(source, options.job))
        return {std::move(source), Container::Ndif};
    return {std::move(source), Container::Raw};
}

std::unique_ptr<HfsPlus::Source> Reopen(const std::wstring& path, Container expected,
                                     const OpenOptions& options)
{
#ifdef HFV_TESTING
    if(beforeReopenTest)beforeReopenTest(path);
#endif
    auto opened = Open(path, options);
    if (!opened.source || opened.container != expected)
        throw HfsPlus::Error("The selected disk image changed container format before reopening");
    return std::move(opened.source);
}
}
