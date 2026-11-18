// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserDirectory.h"
#include "StorageWorker.h"

namespace BrowserNavigation {
enum class Intent { SelectFolder, RefreshFolder, RevealFindHit };

struct Request {
    FileOperations::Location folder;
    Intent intent;
    std::string resource;
    FileOperations::Location reveal;
    std::vector<uint32_t> catalogRoute;
    std::optional<DWORD> windowsMediaSerial;
    int mediaDrive=-1;
};

struct Result {
    FileOperations::Location folder;
    BrowserDirectory::Snapshot listing;
    Storage::Result status;
};

using Read = std::function<Storage::Result(const Request&, Result&, Storage::Job&)>;
using Publish = std::function<void(Result&)>;
} // namespace BrowserNavigation
