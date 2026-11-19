// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace MacBrowser {
class Volume;

struct WorkerOrigin {
    enum Kind { Unavailable, Image, Classic, Cd, Removable } kind = Unavailable;
    std::wstring path, journal;
    std::string root, identity;
    int drive = -1;
    uint64_t offset = 0;
    DWORD generation = 0;
    bool generationKnown = false, writable = false;
};

struct WorkerExecution {
    enum class Placement { Local, PersistentSession } placement = Placement::Local;
    bool windowsPaths = false;
    bool classicConversion = false;
    bool stableCatalogIds = false;

    bool Persistent() const { return placement == Placement::PersistentSession; }
};

// Optional process support for a filesystem adapter. The worker transport
// and handoff use this interface without adding process methods to Volume.
class WorkerEndpoint {
public:
    virtual ~WorkerEndpoint() = default;
    virtual WorkerExecution Execution() const { return {}; }
    virtual WorkerOrigin WorkerDescription() const { return {}; }
    virtual std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles() const { return {}; }
    virtual std::string WorkerEntryIdentity(const std::string& identity) const { return identity; }
    virtual void SuspendWorker() {}
    virtual void ResumeWorker() {}
    virtual void WorkerUncleanExit() {}
    virtual void ReleaseWorkerReaders(const std::vector<std::string>&) {}
    virtual bool FinishWorkerWrite(Volume& volume);

    static WorkerEndpoint* Find(Volume& volume);
    static const WorkerEndpoint* Find(const Volume& volume);
};

WorkerExecution ExecutionFor(const Volume& volume);
} // namespace MacBrowser
