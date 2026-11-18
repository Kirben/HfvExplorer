// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include "ImageHandleTransfer.h"
namespace MacBrowser {
struct WorkerOrigin {
    enum Kind { Unavailable, Image, Classic, Cd, Removable } kind = Unavailable;
    std::wstring path, journal;
    std::string root, identity;
    int drive = -1;
    uint64_t offset = 0;
    DWORD generation = 0;
    bool generationKnown = false, writable = false;
};
std::shared_ptr<Volume> TrackWorkerVolume(std::shared_ptr<Volume>);
std::vector<std::shared_ptr<Volume>> WorkerVolumes(const std::vector<std::string> &keys);
} // namespace MacBrowser
