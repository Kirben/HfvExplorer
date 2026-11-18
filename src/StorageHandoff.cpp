// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "StorageHandoff.h"
#include "VolumeCoordinator.h"
#include "ReadOnlyCdBackend.h"
#include <algorithm>
extern "C" void HFSIFACE_get_dir_flush(void);
namespace MacBrowser {
namespace {
std::mutex mutex;
std::vector<std::weak_ptr<Volume>> volumes;
} // namespace
WorkerOrigin Volume::WorkerDescription() const { return {}; }
std::shared_ptr<Volume> TrackWorkerVolume(std::shared_ptr<Volume> volume) {
    std::lock_guard<std::mutex> lock(mutex);
    volumes.erase(std::remove_if(volumes.begin(), volumes.end(),
                                 [](const std::weak_ptr<Volume> &v) { return v.expired(); }),
                  volumes.end());
    volumes.push_back(volume);
    return volume;
}
std::vector<std::shared_ptr<Volume>> WorkerVolumes(const std::vector<std::string> &keys) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::shared_ptr<Volume>> out;
    for (auto &weak : volumes)
        if (auto volume = weak.lock())
            if (!volume->HostPath(2).empty() ||
                std::find(keys.begin(), keys.end(), volume->ResourceKey()) != keys.end())
                out.push_back(volume);
    return out;
}

WorkerStorageHandoff::~WorkerStorageHandoff() {
    try {
        Restore();
    } catch (...) {
        // Explicit restoration reports failures. Destruction is the fallback.
    }
    for (const auto& file : files_) CloseHandle(file.handle);
}

void WorkerStorageHandoff::Prepare(const std::vector<std::string>& keys,
                                   const std::vector<std::string>& writes) {
    auto volumes = WorkerVolumes(keys);
    HFSIFACE_get_dir_flush();
    for (auto &volume : volumes)
        if (!volume->HostPath(2).empty())
            volume->ReleaseWorkerReaders(writes);
    for (auto &volume : volumes)
        for (auto &file : volume->WorkerFiles()) {
            auto existing =
                std::find_if(files_.begin(), files_.end(), [&](const ImageHandleTransfer::File &p) {
                    return ImageHandleTransfer::Canonical(p.path) ==
                           ImageHandleTransfer::Canonical(file.first);
                });
            if (existing != files_.end()) {
                if (volume->Writable())
                    existing->writable = true;
                continue;
            }
            HANDLE copy = ImageHandleTransfer::Duplicate(HANDLE(file.second), true);
            try {
                files_.push_back({file.first, copy, volume->Writable()});
            } catch (...) {
                CloseHandle(copy);
                throw;
            }
        }
    // Capture every handle before closing any classic mounts shared by adapters.
    for (auto &volume : volumes) {
        if (!volume->WorkerFiles().empty() || volume->WorkerDescription().kind == WorkerOrigin::Classic) {
            suspended_.push_back(volume);
            volume->SuspendWorker();
        }
    }
    for (int drive = 0; drive < 26; ++drive)
        if (std::find(keys.begin(), keys.end(), VolumeCoord::DeviceKey(drive)) != keys.end())
            physical_floppy_suspend(drive);
}

void WorkerStorageHandoff::Restore() {
    if (restored_) return;
    restored_ = true;
    HFSIFACE_get_dir_flush();
    ImageHandleTransfer::Scope transferred(files_);
    std::string failures;
    auto attempt = [&](auto restore) {
        try {
            restore();
        } catch (const std::exception& error) {
            if (!failures.empty()) failures += "; ";
            failures += error.what();
        } catch (...) {
            if (!failures.empty()) failures += "; ";
            failures += "Unexpected browser storage recovery failure";
        }
    };
    if (unclean_)
        for (auto& volume : suspended_) attempt([&] { volume->WorkerUncleanExit(); });
    for (auto& volume : suspended_) attempt([&] { volume->ResumeWorker(); });
    if (!failures.empty())
        throw HfsPlus::Error("Copy worker finished, but browser storage could not be reopened: " + failures);
}

void WorkerStorageHandoff::RestoreAfterFailure(std::exception_ptr failure, bool workerLaunched) {
    if (workerLaunched) MarkUnclean();
    try {
        Restore();
    } catch (const std::exception& restore) {
        std::string original = "Copy worker failed";
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            original = error.what();
        } catch (...) {
        }
        throw HfsPlus::Error(original + "; " + restore.what());
    }
    std::rethrow_exception(failure);
}
} // namespace MacBrowser
