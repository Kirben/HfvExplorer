// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include "ImageHandleTransfer.h"
#include <exception>
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

// Owns suspended mounts and the duplicated image handles until the worker exits.
// Call Restore after the child has stopped so only the browser can use the handles.
class WorkerStorageHandoff {
public:
    WorkerStorageHandoff() = default;
    WorkerStorageHandoff(const WorkerStorageHandoff&) = delete;
    WorkerStorageHandoff& operator=(const WorkerStorageHandoff&) = delete;
    ~WorkerStorageHandoff();

    void Prepare(const std::vector<std::string>& keys, const std::vector<std::string>& writes);
    const std::vector<ImageHandleTransfer::File>& Files() const { return files_; }
    void MarkUnclean() noexcept { unclean_ = true; }
    void Restore();
    [[noreturn]] void RestoreAfterFailure(std::exception_ptr failure, bool workerLaunched);

private:
    std::vector<std::shared_ptr<Volume>> suspended_;
    std::vector<ImageHandleTransfer::File> files_;
    bool unclean_ = false;
    bool restored_ = false;
};
} // namespace MacBrowser
