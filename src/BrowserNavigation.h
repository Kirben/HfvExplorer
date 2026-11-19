// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserDirectory.h"
#include "StorageWorker.h"

namespace BrowserNavigation {
enum class Intent { SelectFolder, RefreshFolder, RevealFindHit, StartupFolder, OpenImage };

struct ImageSource {
    std::wstring path, externalJournal;
};

struct Request {
    FileOperations::Location folder;
    Intent intent;
    std::string resource;
    FileOperations::Location reveal;
    std::vector<uint32_t> catalogRoute;
    int mediaDrive=-1;
    std::optional<ImageSource> image;
    bool tryNextStartupRoot=false;
};

struct Result {
    FileOperations::Location folder;
    BrowserDirectory::Snapshot listing;
    Storage::Result status;
    std::string journalPrompt;
};

using Read = std::function<Storage::Result(const Request&, Result&, Storage::Job&)>;
using Publish = std::function<void(Result&)>;
} // namespace BrowserNavigation
