// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "StorageHandoff.h"
#include <algorithm>
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
} // namespace MacBrowser
