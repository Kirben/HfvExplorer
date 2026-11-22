// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CopyProcess.h"
#include "WorkerTransport.h"
#include "FileOperationWorkerPolicy.h"
#include "TypeMappingSnapshot.h"
#include "StorageHandoff.h"
#include "VolumeCoordinator.h"
#include "PhysicalRemovable.h"
#include "ReadOnlyCdBackend.h"
#include "LegacyEngine.h"
#include "charset.h"
#include "floppy.h"
#include <algorithm>
#include <atomic>
#include <set>
#include <thread>
#ifdef HFV_TESTING
#include "ProcessFloppy.h"
#endif
namespace CopyProcess {
#ifdef HFV_TESTING
std::wstring floppyFixture;
DWORD floppyGeneration = 7;
std::atomic<DWORD> lastProcess{0};
std::atomic<bool> enabled{false};
std::atomic<unsigned> fault{0};
std::atomic<HANDLE> ioGate{nullptr};
#endif
namespace {
using namespace FileOperations;
using namespace MacBrowser;
using namespace CopyWorkerProtocol;
using namespace WorkerTransport;
void CaptureMedia(Location &l) {
    if (!l.IsPhysical() || l.MediaGenerationKnown())
        return;
    DWORD generation = 0;
    if (l.Media() == ClassicMedia::Floppy) {
        if (!physical_floppy_generation(l.Drive(), &generation)) {
            HANDLE opened = floppy_init(l.Drive(), TRUE);
            bool valid = opened && get_floppy_size(opened, l.Drive()) > 0 &&
                         physical_floppy_generation(l.Drive(), &generation);
            if (opened)
                floppy_final(l.Drive());
            Check(valid, "Cannot establish selected floppy identity");
        }
        l.BindMediaGeneration(generation);
    } else if (physical_cd_generation(l.Drive(), &generation))
        l.BindMediaGeneration(generation);
}
Endpoint Capture(const Location &location) {
    Endpoint out;
    out.location = location;
    if (!location.IsMounted()) {
        if(out.location.IsClassic()&&out.location.expectedResource.empty())
            out.location.expectedResource=out.location.ResourceKey();
        CaptureMedia(out.location);
        return out;
    }
    auto host = location.HostPath();
    if (!host.empty()) {
        out.location.SetWindows(host);
        return out;
    }
    const auto* endpoint=WorkerEndpoint::Find(*location.Volume());
    Check(endpoint!=nullptr,"This storage backend cannot be transferred to the copy worker");
    out.origin = endpoint->WorkerDescription();
    if (out.origin.kind == WorkerOrigin::Classic) {
        int n = WideCharToMultiByte(CP_ACP, 0, out.origin.path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        Check(n > 0, "Invalid classic worker path");
        std::string path(n, 0);
        WideCharToMultiByte(CP_ACP, 0, out.origin.path.c_str(), -1, &path[0], n, nullptr, nullptr);
        path.pop_back();
        auto media = Location::Classic(path, ":");
        CaptureMedia(media);
        if (media.MediaGenerationKnown()) {
            out.origin.generationKnown = true;
            out.origin.generation = media.MediaGeneration();
        }
    }
    Check(out.origin.kind != WorkerOrigin::Unavailable,
          "This storage backend cannot be transferred to the copy worker");
    auto selected = location.Volume()->Stat(location.Id());
    Check(location.expectedIdentity.empty() || location.expectedIdentity == selected.identity,
          "The selected entry was replaced before the operation started");
    Check(location.expectedName.empty() || location.expectedName == selected.name,
          "The selected entry was renamed before the operation started");
    Check(!location.expectedParent || location.expectedParent == selected.parent,
          "The selected entry was moved before the operation started");
    if (endpoint->Execution().stableCatalogIds) {
        selected.identity=endpoint->WorkerEntryIdentity(selected.identity);
        out.path.push_back(selected);
        return out;
    }
    std::set<uint32_t> ancestors;
    for (uint32_t id = location.Id(); id != 2;) {
        Check(ancestors.size() < 128 && ancestors.insert(id).second, "Invalid copy-worker source ancestry");
        auto entry = location.Volume()->Stat(id);
        // Browser catalog IDs can depend on which folders the parent has opened.
        // A reopened worker matches the full path and on-disk identity instead.
        entry.resolvedId = 0;
        entry.identity = endpoint->WorkerEntryIdentity(entry.identity);
        out.path.push_back(entry);
        id = entry.parent;
    }
    std::reverse(out.path.begin(), out.path.end());
    return out;
}
std::string AnsiPath(const std::wstring &path) {
    BOOL lost = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    Check(n > 0, "Invalid classic image path");
    std::string out(n, 0);
    Check(WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, path.c_str(), -1, &out[0], n, nullptr, &lost) &&
              !lost,
          "Classic image path cannot be represented");
    out.pop_back();
    return out;
}
std::shared_ptr<Volume> OpenOrigin(const WorkerOrigin &o) {
    std::shared_ptr<Volume> volume;
    if (o.kind == WorkerOrigin::Image)
        volume = MacBrowser::OpenWorkerImage(o);
    else if (o.kind == WorkerOrigin::Classic) {
        auto root = o.root;
        std::vector<char> converted(root.begin(), root.end());
        converted.push_back(0);
        mac_to_pc_charset(reinterpret_cast<unsigned char *>(converted.data()));
        volume = MacBrowser::OpenClassic(AnsiPath(o.path), converted.data(), o.writable, o.identity);
    } else if (o.kind == WorkerOrigin::Cd)
        volume = MacBrowser::OpenCd(o.drive);
    else if (o.kind == WorkerOrigin::Removable)
        volume = Removable::Reopen(o.path, o.offset);
    Check(bool(volume), "Cannot reopen selected volume in the copy worker");
    if (o.generationKnown) {
        const auto* endpoint=WorkerEndpoint::Find(*volume);
        auto now=endpoint?endpoint->WorkerDescription():WorkerOrigin{};
        if (o.kind == WorkerOrigin::Classic) {
            auto media = Location::Classic(AnsiPath(o.path), ":");
            CaptureMedia(media);
            now.generationKnown = media.MediaGenerationKnown();
            if (now.generationKnown)
                now.generation = media.MediaGeneration();
        }
        Check(now.generationKnown && now.generation == o.generation,
              "Media changed before the copy worker opened it; select the volume again");
    }
    Check(!o.writable || volume->Writable(), "The worker could not obtain writable storage access");
    return volume;
}
struct WorkerMounts {
    std::map<std::vector<uint8_t>, std::shared_ptr<Volume>> volumes;
};
Location ResolveEndpoint(WireEndpoint endpoint, WorkerMounts &mounts) {
    auto& l = endpoint.location;
    if (endpoint.kind != Location::Kind::Mounted) return std::move(l);
    const auto& origin = endpoint.origin;
    const auto& path = endpoint.path;
    auto selectedId = endpoint.selectedId;
    Data key;
    EncodeOrigin(key, origin);
    auto found = mounts.volumes.find(key.bytes);
    if (found == mounts.volumes.end())
        found = mounts.volumes.emplace(key.bytes, OpenOrigin(origin)).first;
    auto volume = found->second;
    uint32_t id = 2;
    const bool stable=ExecutionFor(*volume).stableCatalogIds;
    Check(!stable || path.size() == 1, "Invalid stable catalog endpoint");
    for (auto &segment : path) {
        bool matched = false;
        auto candidates = stable ? std::vector<Entry>{volume->Stat(selectedId)} : volume->List(id);
        for (auto &e : candidates)
            if (e.name == segment.name && e.version == segment.version &&
                e.directory == segment.directory &&
                (!segment.resolvedId || e.resolvedId == segment.resolvedId)) {
                Check(!matched, "Ambiguous worker source path");
                Check(origin.kind == WorkerOrigin::Removable || e.identity == segment.identity,
                      "The selected entry changed before worker execution");
                id = e.id;
                matched = true;
            }
        Check(matched, "The selected entry is no longer present");
    }
    l.SetMounted(volume, id);
    auto e = volume->Stat(id);
    l.expectedIdentity = e.identity;
    l.expectedParent = e.parent;
    l.expectedName = e.name;
    return std::move(l);
}

void AddAffected(Completion& completion, const std::string& key) {
    if (std::none_of(completion.affected.begin(), completion.affected.end(),
        [&](const Change& entry) { return entry.resourceKey == key; }))
        completion.affected.push_back({key});
}

Request ResolveRequest(WireRequest wire, WorkerMounts &mounts, AttributeSets& attributes) {
    auto target = ResolveEndpoint(std::move(wire.target), mounts);
    if (wire.kind == Kind::Copy) {
        std::vector<Location> sources;
        sources.reserve(wire.sources.size());
        for (auto& endpoint : wire.sources)
            sources.push_back(ResolveEndpoint(std::move(endpoint), mounts));
        return OrdinaryCopy{std::move(sources), std::move(target)};
    }
    if(wire.legacy){
        auto source=ResolveEndpoint(std::move(wire.sources.front()),mounts);
        const auto& conversion=*wire.legacy;
        return LegacyConversion{std::move(source),std::move(target),conversion.directory,conversion.move,
                                false,conversion.options,conversion.target};
    }
    if (wire.kind == Kind::CreateFolder)
        return CreateFolder{std::move(target), std::move(wire.name)};
    if (wire.kind == Kind::Delete) {
        DeleteEntries deletion;
        for (auto& endpoint : wire.sources)
            deletion.selection.push_back(ResolveEndpoint(std::move(endpoint), mounts));
        return deletion;
    }
    if (wire.basic && wire.sources.front().kind == Location::Kind::Mounted)
        Check(wire.basic->original.id == wire.sources.front().selectedId,
              "The Properties selection changed");
    auto source = ResolveEndpoint(std::move(wire.sources.front()), mounts);
    if (wire.rename)
        return RenameEntry{std::move(source),std::move(wire.rename->name),std::move(target),wire.rename->checkNameLock};
    if (wire.kind == Kind::CreateHardLink)
        return FileOperations::CreateHardLink{std::move(source), std::move(target), std::move(wire.name)};
    if (wire.classic) {
        wire.classic->item = std::move(source);
        return std::move(*wire.classic);
    }
    if (wire.basic) {
        if (source.IsMounted()) wire.basic->original.id = source.Id();
        wire.basic->item = std::move(source);
        return std::move(*wire.basic);
    }
    if (wire.kind == Kind::SetAttributes)
        return EditAttributes{std::move(source), std::move(*attributes[0]), std::move(*attributes[1])};
    if (wire.compression) {
        wire.compression->item = std::move(source);
        wire.compression->originalAttributes = std::move(*attributes[0]);
        return std::move(*wire.compression);
    }
    if (wire.kind == Kind::ExportFork)
        return ExportFileFork{std::move(source), std::move(target), wire.fork};
    if (target.IsWindows()) {
        auto endpoint = ResolveWindowsFile(target.WindowsPath());
        target.SetMounted(endpoint.first, endpoint.second);
        target.expectedParent = endpoint.first->Stat(endpoint.second).parent;
    }
    return ImportResourceFork{std::move(source), std::move(target)};
}
} // namespace
#ifdef HFV_TESTING
std::vector<uint8_t> EncodeLocationForTest(const Location &location) {
    Check(!location.IsMounted(), "This codec test does not open mounted volumes");
    Data data;
    EncodeEndpoint(data, Endpoint{location, {}, {}});
    return data.bytes;
}
Location DecodeLocationForTest(const std::vector<uint8_t> &bytes) {
    Data data;
    data.bytes = bytes;
    auto location = ParseEndpoint(data, false).location;
    data.end();
    return location;
}
#endif
Storage::Result Run(const Request &request, Storage::Job &job, Completion &completion,
                    const std::vector<std::string> &keys, const std::vector<std::string> &writes) {
    std::vector<Endpoint> sources;
    Endpoint target;
    if (auto* copy = request.Get<OrdinaryCopy>()) {
        for (const auto& source : copy->sources) sources.push_back(Capture(source));
        target = Capture(copy->destination);
    } else if (auto* legacy = request.Conversion()) {
        sources.push_back(Capture(legacy->source));
        target=Capture(legacy->destination);
    } else if (auto* output = request.Get<ExportFileFork>()) {
        sources.push_back(Capture(output->source));
        target = Capture(output->output);
    } else if (auto* input = request.Get<ImportResourceFork>()) {
        sources.push_back(Capture(input->input));
        target = Capture(input->file);
    } else if (auto* rename = request.Rename()) {
        sources.push_back(Capture(rename->source));
        target = Capture(rename->parent);
    } else if (auto* deletion = request.Delete()) {
        for (const auto& item : deletion->selection) sources.push_back(Capture(item));
    } else if (auto* folder = request.Get<CreateFolder>()) {
        target = Capture(folder->parent);
    } else if (auto* link = request.Get<FileOperations::CreateHardLink>()) {
        sources.push_back(Capture(link->source));
        target = Capture(link->parent);
    } else if (auto* metadata = request.Metadata()) {
        sources.push_back(Capture(std::visit([](const auto& edit) -> const Location& { return edit.item; },
                                            *metadata)));
    }
    const auto workerPolicy=WorkerPolicy(request.OperationKind());
    Check(workerPolicy.has_value(),"Operation cannot run in a temporary worker");
    WorkerStorageHandoff handoff;
    Storage::Result result;
    bool requestSent = false;
    try {
        handoff.Prepare(keys, writes);
        Process worker;
#ifdef HFV_TESTING
        const unsigned testFault = fault.load();
        if(testFault==staleImagePartition){
            auto* origin=&target.origin;
            if(!origin->imageMount&& !sources.empty())origin=&sources.front().origin;
            Check(origin->imageMount&&origin->imageMount->selection,"Missing test image partition");
            origin->imageMount->selection->partition.offset+=512;
        }
        Handle testGate;
        if (testFault == 7) {
            const auto gate = ioGate.load();
            Check(gate != nullptr, "Missing process-test gate");
            testGate.reset(ImageHandleTransfer::Duplicate(gate, true));
        }
        const wchar_t* executableOverride = testFault == failWorkerLaunch ? L"C:\\HFV-invalid-?.exe" : nullptr;
        worker.Launch(handoff.Files(), L"--copy-worker", testGate.value, executableOverride);
        lastProcess = worker.pid;
        if (testGate.value) {
            Data packet;
            packet.put(uintptr_t(testGate.value));
            Send(worker.input.value, IoGate, std::move(packet));
        }
        if (!floppyFixture.empty()) {
            Data d;
            d.wide(floppyFixture);
            d.put(floppyGeneration);
            Send(worker.input.value, Fixture, std::move(d));
        }
#else
        constexpr unsigned testFault = 0;
        worker.Launch(handoff.Files(), L"--copy-worker");
#endif
        for (const auto& file : handoff.Files()) SendImageFile(worker.input.value,file);
        if (request.OperationKind() == Kind::LegacyCopy) {
            auto mappings = TypeMappings::Capture();
            for (unsigned direction = 0; direction < 2; ++direction)
                for (auto &entry : direction ? mappings.windowsToHfs : mappings.hfsToWindows) {
                    Data d;
                    d.put(direction);
                    d.put(entry.first);
                    d.put(entry.second);
                    Send(worker.input.value, Mapping, std::move(d));
                }
        }
        if (auto* metadata = request.Metadata()) {
            if (auto* edit = std::get_if<EditAttributes>(metadata)) {
                SendAttributes(worker.input.value, 0, edit->original, job);
                SendAttributes(worker.input.value, 1, edit->desired, job);
            } else if (auto* compression = std::get_if<ChangeCompression>(metadata))
                SendAttributes(worker.input.value, 0, compression->originalAttributes, job);
        }
        Send(worker.input.value, RequestPacket,
             EncodeRequest(request, worker.parent.value, testFault, sources, target));
        requestSent = true;
        bool cancelled = false, finished = false;
        while (!finished) {
            if (job.cancelled() && !cancelled) {
                try {
                    Send(worker.input.value, Cancel);
                } catch (...) {
                    worker.input.reset();
                }
                cancelled = true;
            }
            if (Available(worker.output.value)) {
                Data d;
                auto message = Receive(worker.output.value, d);
                if (message == Progress)
                    job.progress(DecodeProgress(d));
                else if (message == Conflict) {
                    auto id = d.get();
                    auto path = d.text();
                    auto kind = d.get();
                    Check(kind <= uint64_t(Storage::CopyPrompt::Uncompressed),"Invalid copy prompt");
                    d.end();
                    auto choice = job.requestChoice(Storage::CopyPrompt(kind),path);
                    Data reply;
                    reply.put(id);
                    reply.put(uint64_t(choice));
                    try {
                        Send(worker.input.value, Choice, std::move(reply));
                    } catch (...) {
                        worker.input.reset();
                    }
                } else if (message == AffectedStorage) {
                    AddAffected(completion, d.text());
                } else if (message == Finished) {
                    result.code = int(d.get());
                    result.error = DWORD(d.get());
                    result.message = d.text();
                    result.moved = d.get() != 0;
                    completion.complete = d.get() != 0;
                    completion.completed = unsigned(d.get());
                    if (!d.get())
                        handoff.MarkUnclean();
                    DecodeOutcome(d,result);
                    finished = true;
                } else
                    throw HfsPlus::Error("Unexpected copy-worker response");
                d.end();
            } else if (WaitForSingleObject(worker.process.value, 20) == WAIT_OBJECT_0) {
                if (Available(worker.output.value))
                    continue;
                throw HfsPlus::Error("Copy worker exited without a completion result; completed changes may "
                                     "remain. Inspect the destination before retrying");
            }
        }
        worker.input.reset();
        Check(WaitForSingleObject(worker.process.value, INFINITE) == WAIT_OBJECT_0,
              "Cannot wait for copy worker cleanup");
        DWORD exit = 0;
        Check(GetExitCodeProcess(worker.process.value, &exit) && exit == 0,
              "Copy worker failed during cleanup; inspect the destination");
    } catch (...) {
        if(requestSent)Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Unverified);
        if (requestSent && workerPolicy->reportsAffectedStorage)
            for (const auto& key : writes) AddAffected(completion, key);
        handoff.RestoreAfterFailure(std::current_exception(), requestSent);
    }
    handoff.Restore();
    return result;
}
int Child(const char *inputText, const char *outputText) {
    // No document templates, profile reads, drive discovery or windows in this mode.
    HANDLE input = HANDLE(uintptr_t(_strtoui64(inputText, nullptr, 10))),
           output = HANDLE(uintptr_t(_strtoui64(outputText, nullptr, 10)));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        HANDLE ioGate = nullptr;
        std::wstring floppyPath;
        DWORD floppyGeneration = 7;
        TypeMappings::Snapshot mappings;
        std::optional<WireRequest> wire;
        AttributeSets attributes;
        std::vector<ImageHandleTransfer::File> files;
        for (;;) {
            Data d;
            auto message = Receive(input, d);
            if (message == RequestPacket) {
                wire = DecodeRequest(d);
                const auto attributeCount=WorkerPolicy(wire->kind)->streamedAttributeSets;
                Check(attributes[0].has_value()==(attributeCount>=1)&&
                      attributes[1].has_value()==(attributeCount==2),
                      "Wrong attribute sets for worker operation");
                break;
            } else if (message == AttributeSet) {
                ReceiveAttributes(input, d, attributes);
                continue;
            } else if (message == File) {
                files.push_back(DecodeImageFile(d));
            } else if (message == Fixture) {
#ifdef HFV_TESTING
                floppyPath = d.wide();
                floppyGeneration = DWORD(d.get());
#else
                throw HfsPlus::Error("Test fixtures are disabled");
#endif
            } else if (message == IoGate) {
#ifdef HFV_TESTING
                Check(!ioGate, "Repeated test gate");
                ioGate = HANDLE(uintptr_t(d.get()));
#else
                throw HfsPlus::Error("Test gates are disabled");
#endif
            } else if (message == Mapping) {
                auto direction = d.get();
                Check(direction < 2, "Invalid type-mapping direction");
                auto key = d.text(), value = d.text();
                (direction ? mappings.windowsToHfs : mappings.hfsToWindows)
                    .emplace_back(std::move(key), std::move(value));
            } else
                throw HfsPlus::Error("Invalid worker initialization");
            d.end();
        }
        const auto parent = wire->parent;
        Check(WaitForSingleObject(parent, 0) == WAIT_TIMEOUT, "Invalid or closed copy-worker parent");
        const auto testFault = wire->fault;
#ifdef HFV_TESTING
        if (testFault == 1)
            ExitProcess(71);
        if (testFault == 5) {
            DWORD invalid[] = {999, DWORD(Finished), 0};
            Exact(output, invalid, sizeof(invalid), true);
            return 75;
        }
#else
        Check(!testFault, "Test worker controls are disabled");
#endif
        Storage::Job job;
        std::mutex sending;
        std::atomic<bool> stop{false};
        std::thread control([&] {
            uint64_t conflict = 0;
            try {
                while (!stop) {
                    if (WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
                        job.cancel();
                        return;
                    }
                    if (Available(input)) {
                        Data d;
                        auto message = Receive(input, d);
                        if (message == Cancel)
                            job.cancel();
                        else if (message == Choice) {
                            auto id = d.get();
                            auto choice = d.get();
                            Check(choice <= uint64_t(Storage::ConflictChoice::Cancel),
                                  "Invalid overwrite response");
                            job.resolve(id, Storage::ConflictChoice(choice));
                        } else
                            throw HfsPlus::Error("Invalid worker control");
                        d.end();
                    }
                    auto pending = job.conflictState();
                    if (!pending.path.empty() && pending.id != conflict) {
                        Data d;
                        d.put(pending.id);
                        d.put(pending.path);
                        d.put(uint64_t(pending.kind));
                        {
                            std::lock_guard<std::mutex> lock(sending);
                            Send(output, Conflict, std::move(d));
                        }
                        conflict = pending.id;
                    }
                    Sleep(10);
                }
            } catch (...) {
                job.cancel();
            }
        });
        struct Join {
            std::atomic<bool> &stop;
            std::thread &thread;
            ~Join() {
                stop = true;
                if (thread.joinable())
                    thread.join();
            }
        } join{stop, control};
        DWORD lastReport = 0;
        job.transport(
            [&](const Storage::Progress &p) {
                DWORD now = GetTickCount();
                if (now - lastReport < 40 && p.fileBytes < p.fileTotal && p.phase == "Copying")
                    return;
                lastReport = now;
                Data d;
                EncodeProgress(d, p);
                std::lock_guard<std::mutex> lock(sending);
                Send(output, Progress, std::move(d));
            },
            {});
        Storage::Result result;
        Completion completion;
        bool safe = true;
        {
            ImageHandleTransfer::Scope transferred(files);
            TypeMappings::Apply(mappings);
            floppy_module_global_init();
            LegacyEngine::Scope engine;
            WorkerMounts mounts;
#ifdef HFV_TESTING
            std::unique_ptr<ProcessFloppy> simulatedFloppy;
            if (!floppyPath.empty()) {
                simulatedFloppy.reset(new ProcessFloppy(floppyPath));
                simulatedFloppy->generation = floppyGeneration;
                Check(readonly_floppy_attach_api(25, simulatedFloppy.get()),
                      "Cannot attach process floppy fixture");
            }
            struct Detach {
                bool attached;
                ~Detach() {
                    if (attached)
                        readonly_floppy_attach_api(25, nullptr);
                }
            } detach{bool(simulatedFloppy)};
#endif
            try {
                auto request = ResolveRequest(std::move(*wire), mounts, attributes);
#ifdef HFV_TESTING
                bool injected = false;
                unsigned bulkReads = 0;
                HfsPlus::workerIoFault = [&](bool write, uint64_t, size_t n) {
                    if (testFault == 7 && !write && n >= 65536 && ++bulkReads >= 2) {
                        Check(ioGate != nullptr, "Missing inherited test gate");
                        auto progress = job.progress();
                        progress.phase = "Worker test gate";
                        job.progress(progress);
                        DWORD until = GetTickCount() + 40000;
                        while (WaitForSingleObject(ioGate, 10) != WAIT_OBJECT_0) {
                            if (job.cancelled())
                                throw Storage::Cancelled();
                            Check(LONG(until - GetTickCount()) > 0, "Process-test gate timed out");
                        }
                    }
                    if (testFault == 2 && !write && n >= 65536)
                        Sleep(100);
                    if (testFault == 3 && !write && n >= 65536 && !injected) {
                        injected = true;
                        throw HfsPlus::Error("Injected worker read failure");
                    }
                    if (testFault == 4 && write && n >= 4096)
                        ExitProcess(74);
                };
#endif
                std::vector<std::shared_ptr<Volume>> writableMounts;
                for (auto &mounted : mounts.volumes)
                    if (mounted.second->Writable())
                        writableMounts.push_back(mounted.second);
                result = FileOperations::ExecuteInWorker(request, job, completion);
                for (auto &volume : writableMounts) {
                    try {
                        auto* endpoint=WorkerEndpoint::Find(*volume);
                        if (!(endpoint?endpoint->FinishWorkerWrite(*volume):volume->Writable())) safe=false;
                    } catch (const std::exception&) { safe=false; }
                }
            } catch (const Storage::MediaChanged& error) {
                result = {1, ERROR_MEDIA_CHANGED, error.what()};
            } catch (const std::exception &error) {
                result = {1, ERROR_UNHANDLED_EXCEPTION, error.what()};
            } catch (...) {
                result = {1, ERROR_UNHANDLED_EXCEPTION, "Unexpected worker setup failure"};
            }

#ifdef HFV_TESTING
            HfsPlus::workerIoFault = {};
#endif
            mounts.volumes.clear();
        }
        if (!safe && !result.code)
            result = {1, ERROR_WRITE_FAULT,
                      "Worker storage recovery was incomplete; inspect the volume before further writes"};
        stop = true;
        control.join();
        if (WorkerPolicy(wire->kind)->reportsAffectedStorage)
            for (const auto& change : completion.affected) {
                Data affected;
                affected.put(change.resourceKey);
                Send(output, AffectedStorage, std::move(affected));
            }
        Data final;
        final.put(uint64_t(result.code));
        final.put(result.error);
        final.put(result.message);
        final.put(result.moved);
        final.put(completion.complete);
        final.put(completion.completed);
        final.put(safe);
        if(!safe && result.rollback<Storage::Rollback::Unverified)result.rollback=Storage::Rollback::Unverified;
        Storage::OutcomeEvidence evidence;
        result.Finalize(evidence,completion.completed,job.progress().skipped);
        EncodeOutcome(final,result);
        Send(output, Finished, std::move(final));
        return 0;
    } catch (const std::exception &error) {
        try {
            Data final;
            final.put(1);
            final.put(ERROR_UNHANDLED_EXCEPTION);
            final.put(std::string("Copy worker initialization failed: ") + error.what());
            final.put(0);
            final.put(0);
            final.put(0);
            final.put(1);
            Storage::Result failed{1,ERROR_UNHANDLED_EXCEPTION,{}};
            failed.outcome=Storage::Outcome::Failed;
            EncodeOutcome(final,failed);
            Send(output, Finished, std::move(final));
            return 0;
        } catch (...) {
            return 72;
        }
    } catch (...) {
        return 72;
    }
}
} // namespace CopyProcess
