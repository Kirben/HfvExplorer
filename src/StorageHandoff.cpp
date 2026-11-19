// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "StorageHandoff.h"
#include "VolumeCoordinator.h"
#include "ReadOnlyCdBackend.h"
#include <algorithm>
namespace MacBrowser {
namespace {
std::mutex mutex;
std::vector<std::weak_ptr<Volume>> volumes;
} // namespace
WorkerEndpoint* WorkerEndpoint::Find(Volume& volume) { return dynamic_cast<WorkerEndpoint*>(&volume); }
const WorkerEndpoint* WorkerEndpoint::Find(const Volume& volume) { return dynamic_cast<const WorkerEndpoint*>(&volume); }
WorkerExecution ExecutionFor(const Volume& volume) {
    const auto* endpoint = WorkerEndpoint::Find(volume);
    return endpoint ? endpoint->Execution() : WorkerExecution{};
}
bool WorkerEndpoint::FinishWorkerWrite(Volume& volume) { return volume.Writable(); }
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
    for (auto& volume : volumes)
        if (auto* endpoint = WorkerEndpoint::Find(*volume); endpoint && endpoint->Execution().windowsPaths)
            endpoint->ReleaseWorkerReaders(writes);

    std::vector<std::shared_ptr<Volume>> toSuspend;
    for (auto& volume : volumes) {
        auto* endpoint = WorkerEndpoint::Find(*volume);
        if (!endpoint) continue;
        const auto files = endpoint->WorkerFiles();
        if (!files.empty() || endpoint->WorkerDescription().kind == WorkerOrigin::Classic)
            toSuspend.push_back(volume);
        for (const auto& file : files) {
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
    }
    // Capture every handle before closing any classic mounts shared by adapters.
    for (auto& volume : toSuspend) {
        suspended_.push_back(volume);
        WorkerEndpoint::Find(*volume)->SuspendWorker();
    }
    for (int drive = 0; drive < 26; ++drive)
        if (std::find(keys.begin(), keys.end(), VolumeCoord::DeviceKey(drive)) != keys.end())
            physical_floppy_suspend(drive);
}

void WorkerStorageHandoff::Restore() {
    if (restored_) return;
    restored_ = true;
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
        for (auto& volume : suspended_) attempt([&] { WorkerEndpoint::Find(*volume)->WorkerUncleanExit(); });
    for (auto& volume : suspended_) attempt([&] { WorkerEndpoint::Find(*volume)->ResumeWorker(); });
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
