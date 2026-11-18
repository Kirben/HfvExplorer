// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CopyProcess.h"
#include "CopyWorkerProtocol.h"
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
std::atomic<DWORD> lastProcess{0}, lastExitCode{0};
std::atomic<bool> enabled{false};
std::atomic<unsigned> fault{0};
std::atomic<HANDLE> ioGate{nullptr};
#endif
namespace {
using namespace FileOperations;
using namespace MacBrowser;
using namespace CopyWorkerProtocol;
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {};
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    void reset(HANDLE h = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
        value = h;
    }
};
void Exact(HANDLE pipe, void *data, DWORD n, bool write) {
    auto p = static_cast<uint8_t *>(data);
    while (n) {
        DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe, p, n, &done, nullptr) : ReadFile(pipe, p, n, &done, nullptr);
        Check(ok && done, "Copy-worker connection closed unexpectedly; completed changes may remain. Inspect "
                          "the destination before retrying");
        p += done;
        n -= done;
    }
}
void Send(HANDLE pipe, Message message, Data data = {}) {
    Check(data.bytes.size() <= FrameLimit(message), "Copy-worker packet exceeds the protocol frame size");
    DWORD header[] = {DWORD(protocol), DWORD(message), DWORD(data.bytes.size())};
    Exact(pipe, header, sizeof(header), true);
    if (!data.bytes.empty())
        Exact(pipe, data.bytes.data(), DWORD(data.bytes.size()), true);
}
Message Receive(HANDLE pipe, Data &data) {
    DWORD header[3];
    Exact(pipe, header, sizeof(header), false);
    auto message = ValidateFrame(header);
    data = Data();
    data.bytes.resize(header[2]);
    if (header[2])
        Exact(pipe, data.bytes.data(), header[2], false);
    return message;
}
bool Available(HANDLE pipe) {
    DWORD n = 0;
    Check(PeekNamedPipe(pipe, nullptr, 0, nullptr, &n, nullptr) != FALSE,
          "Copy-worker control connection closed");
    return n != 0;
}
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
        CaptureMedia(out.location);
        return out;
    }
    auto host = location.HostPath();
    if (!host.empty()) {
        out.location.SetWindows(host);
        return out;
    }
    out.origin = location.Volume()->WorkerDescription();
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
    if (!strcmp(location.Volume()->Kind(), "HFS+") || !strcmp(location.Volume()->Kind(), "HFSX")) {
        out.path.push_back(selected);
        return out;
    }
    std::set<uint32_t> ancestors;
    for (uint32_t id = location.Id(); id != 2;) {
        Check(ancestors.size() < 128 && ancestors.insert(id).second, "Invalid copy-worker source ancestry");
        auto entry = location.Volume()->Stat(id);
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
        volume = MacBrowser::OpenImage(o.path, o.journal);
    else if (o.kind == WorkerOrigin::Classic) {
        auto root = o.root;
        std::vector<char> converted(root.begin(), root.end());
        converted.push_back(0);
        mac_to_pc_charset(reinterpret_cast<unsigned char *>(converted.data()));
        volume = MacBrowser::OpenClassic(AnsiPath(o.path), converted.data(), o.writable);
    } else if (o.kind == WorkerOrigin::Cd)
        volume = MacBrowser::OpenCd(o.drive);
    else if (o.kind == WorkerOrigin::Removable)
        volume = Removable::Reopen(o.path, o.offset);
    Check(bool(volume), "Cannot reopen selected volume in the copy worker");
    if (o.generationKnown) {
        auto now = volume->WorkerDescription();
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
    bool stable = !strcmp(volume->Kind(), "HFS+") || !strcmp(volume->Kind(), "HFSX");
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
using AttributeSets = std::array<std::optional<HfsPlus::Attributes>, 2>;
constexpr size_t attributeChunk = 65536;

void SendAttributes(HANDLE pipe, unsigned slot, const HfsPlus::Attributes& attributes,
                    Storage::Job& job) {
    Data set;
    set.put(slot); set.put(attributes.size());
    Send(pipe, AttributeSet, std::move(set));
    std::vector<char> buffer(attributeChunk);
    for (const auto& attribute : attributes) {
        Data header;
        header.u16(attribute.first); header.put(attribute.second.size());
        Send(pipe, AttributeHeader, std::move(header));
        for (uint64_t at = 0; at < attribute.second.size();) {
            if (job.cancelled()) throw Storage::Cancelled();
            const auto count = size_t((std::min)(uint64_t(buffer.size()), attribute.second.size() - at));
            attribute.second.Read(at, buffer.data(), count);
            Data chunk;
            chunk.bytes.assign(buffer.data(), buffer.data() + count);
            Send(pipe, AttributeChunk, std::move(chunk));
            at += count;
        }
    }
}

void ReceiveAttributes(HANDLE pipe, Data& set, AttributeSets& sets) {
    auto slot = set.get(), count = set.get();
    set.end();
    Check(slot < sets.size() && !sets[size_t(slot)], "Invalid worker attribute set");
    HfsPlus::Attributes attributes;
    for (uint64_t i = 0; i < count; ++i) {
        Data header;
        Check(Receive(pipe, header) == AttributeHeader, "Expected worker attribute header");
        auto name = header.u16();
        auto length = header.get();
        header.end();
        Check(!name.empty() && !attributes.count(name), "Invalid or duplicate worker attribute name");
        auto value = HfsPlus::AttributeData::Snapshot(length, [&](uint64_t, void* bytes, size_t size) {
            auto output = static_cast<uint8_t*>(bytes);
            while (size) {
                Data chunk;
                Check(Receive(pipe, chunk) == AttributeChunk &&
                      chunk.bytes.size() == (std::min)(size, attributeChunk),
                      "Invalid worker attribute chunk");
                memcpy(output, chunk.bytes.data(), chunk.bytes.size());
                output += chunk.bytes.size();
                size -= chunk.bytes.size();
            }
        });
        attributes.emplace(std::move(name), std::move(value));
    }
    sets[size_t(slot)] = std::move(attributes);
}

bool ReportsAffectedStorage(Kind kind) {
    switch (kind) {
    case Kind::Rename: case Kind::Delete: case Kind::CreateFolder: case Kind::CreateHardLink:
    case Kind::SetProperties: case Kind::SetBasicMetadata: case Kind::SetAttributes:
    case Kind::SetCompression: return true;
    default: return false;
    }
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
    if (wire.legacy) {
        if (!wire.sources.empty())
            wire.legacy->selection = ConversionSelection{
                ResolveEndpoint(std::move(wire.sources.front()), mounts), std::move(target)};
        return std::move(*wire.legacy);
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
    if (wire.kind == Kind::Rename)
        return RenameEntry{std::move(source), std::move(wire.name), std::move(target), wire.checkNameLock};
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
struct Process {
    Handle input, output, process, thread, parent;
    DWORD pid = 0;
#ifdef HFV_TESTING
    Handle gate;
#endif
    ~Process() {
        input.reset();
        output.reset();
        if (process.value) {
            WaitForSingleObject(process.value, INFINITE);
#ifdef HFV_TESTING
            DWORD code = 0;
            if (GetExitCodeProcess(process.value, &code)) lastExitCode = code;
#endif
        }
    }
    void Launch(const std::vector<ImageHandleTransfer::File> &files) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
        Check(CreatePipe(&inRead, &inWrite, &security, 0) != FALSE, "Cannot create worker control pipe");
        Handle childInput(inRead);
        input.reset(inWrite);
        Check(SetHandleInformation(input.value, HANDLE_FLAG_INHERIT, 0) != FALSE,
              "Cannot protect control pipe handle");
        Check(CreatePipe(&outRead, &outWrite, &security, 0) != FALSE, "Cannot create worker progress pipe");
        output.reset(outRead);
        Handle childOutput(outWrite);
        Check(SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0) != FALSE,
              "Cannot protect progress pipe handle");
        parent.reset(OpenProcess(SYNCHRONIZE, TRUE, GetCurrentProcessId()));
        Check(parent.value != nullptr, "Cannot monitor browser lifetime");
        std::vector<HANDLE> inherited{childInput.value, childOutput.value, parent.value};
        for (auto &f : files)
            inherited.push_back(f.handle);
#ifdef HFV_TESTING
        if (fault == 7) {
            Check(ioGate.load() != nullptr, "Missing process-test gate");
            gate.reset(ImageHandleTransfer::Duplicate(ioGate.load(), true));
            inherited.push_back(gate.value);
        }
#endif
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<uint8_t> attributes(size);
        auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        Check(InitializeProcThreadAttributeList(list, 1, 0, &size) != FALSE,
              "Cannot initialize worker handle whitelist");
        struct DeleteList {
            LPPROC_THREAD_ATTRIBUTE_LIST list;
            ~DeleteList() { DeleteProcThreadAttributeList(list); }
        } cleanup{list};
        Check(UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                        inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE,
              "Cannot restrict inherited worker handles");
        wchar_t executable[32768];
        DWORD n = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
        Check(n && n < ARRAYSIZE(executable), "Cannot locate copy worker executable");
#ifdef HFV_TESTING
        // Lets fixture-only tests exercise the actual shipping worker binary.
        wchar_t overridePath[32768];
        DWORD overrideSize = GetEnvironmentVariableW(L"HFV_COPY_WORKER_TEST_EXECUTABLE", overridePath,
                                                     ARRAYSIZE(overridePath));
        if (overrideSize) {
            Check(overrideSize < ARRAYSIZE(overridePath), "Invalid test worker executable path");
            wcscpy_s(executable, overridePath);
        }
        if (fault == failWorkerLaunch) {
            // A Win32 path with a reserved character cannot name an executable.
            wcscpy_s(executable, L"C:\\HFV-invalid-?.exe");
        }
#endif
        auto command = L"\"" + std::wstring(executable) + L"\" --copy-worker " +
                       std::to_wstring(uintptr_t(childInput.value)) + L" " +
                       std::to_wstring(uintptr_t(childOutput.value));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = list;
        PROCESS_INFORMATION info{};
        Check(CreateProcessW(executable, &command[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                             &startup.StartupInfo, &info) != FALSE,
              "Cannot start copy worker; no copy was performed");
        process.reset(info.hProcess);
        thread.reset(info.hThread);
        pid = info.dwProcessId;
    }
};
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
bool UsesWorker(const Request &request) {
#ifdef HFV_TESTING
    if (!enabled)
        return false;
#endif
    return SupportsOperation(request.OperationKind());
}
Storage::Result Run(const Request &request, Storage::Job &job, Completion &completion,
                    const std::vector<std::string> &keys, const std::vector<std::string> &writes) {
    std::vector<Endpoint> sources;
    Endpoint target;
    if (auto* copy = request.Get<OrdinaryCopy>()) {
        for (const auto& source : copy->sources) sources.push_back(Capture(source));
        target = Capture(copy->destination);
    } else if (auto* legacy = request.Conversion()) {
        if (legacy->selection) {
            sources.push_back(Capture(legacy->selection->source));
            target = Capture(legacy->selection->folder);
        }
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
    auto execution = request;
    if (request.OperationKind() == Kind::LegacyCopy) {
        execution.Legacy().source = Capture(request.Legacy().source).location;
        execution.Legacy().destination = Capture(request.Legacy().destination).location;
    }
    WorkerStorageHandoff handoff;
    Storage::Result result;
    bool requestSent = false;
    try {
        handoff.Prepare(keys, writes);
        Process worker;
        worker.Launch(handoff.Files());
#ifdef HFV_TESTING
        lastProcess = worker.pid;
        unsigned testFault = fault.load();
#else
        unsigned testFault = 0;
#endif
#ifdef HFV_TESTING
        if (worker.gate.value) {
            Data gate;
            gate.put(uintptr_t(worker.gate.value));
            Send(worker.input.value, IoGate, std::move(gate));
        }
        if (!floppyFixture.empty()) {
            Data d;
            d.wide(floppyFixture);
            d.put(floppyGeneration);
            Send(worker.input.value, Fixture, std::move(d));
        }
#endif
        for (auto &file : handoff.Files()) {
            Data d;
            d.wide(file.path);
            d.put(uintptr_t(file.handle));
            d.put(file.writable);
            Send(worker.input.value, File, std::move(d));
        }
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
        if (auto* metadata = execution.Metadata()) {
            if (auto* edit = std::get_if<EditAttributes>(metadata)) {
                SendAttributes(worker.input.value, 0, edit->original, job);
                SendAttributes(worker.input.value, 1, edit->desired, job);
            } else if (auto* compression = std::get_if<ChangeCompression>(metadata))
                SendAttributes(worker.input.value, 0, compression->originalAttributes, job);
        }
        Send(worker.input.value, RequestPacket,
             EncodeRequest(execution, worker.parent.value, testFault, sources, target));
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
                    auto choice = job.replace(path);
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
        if (requestSent && ReportsAffectedStorage(request.OperationKind()))
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
                Check(attributes[0].has_value() == (wire->kind == Kind::SetAttributes || wire->kind == Kind::SetCompression) &&
                      attributes[1].has_value() == (wire->kind == Kind::SetAttributes),
                      "Wrong attribute sets for worker operation");
                break;
            } else if (message == AttributeSet) {
                ReceiveAttributes(input, d, attributes);
                continue;
            } else if (message == File) {
                ImageHandleTransfer::File f;
                f.path = d.wide();
                f.handle = HANDLE(uintptr_t(d.get()));
                f.writable = d.get() != 0;
                files.push_back(std::move(f));
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
                    if (!pending.second.empty() && pending.first != conflict) {
                        Data d;
                        d.put(pending.first);
                        d.put(pending.second);
                        {
                            std::lock_guard<std::mutex> lock(sending);
                            Send(output, Conflict, std::move(d));
                        }
                        conflict = pending.first;
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
                for (auto &volume : writableMounts)
                    if (!volume->Writable())
                        safe = false;
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
        if (ReportsAffectedStorage(wire->kind))
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
