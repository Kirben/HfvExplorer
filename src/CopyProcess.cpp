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
        selected.identity=location.Volume()->WorkerEntryIdentity(selected.identity);
        out.path.push_back(selected);
        return out;
    }
    std::set<uint32_t> ancestors;
    for (uint32_t id = location.Id(); id != 2;) {
        Check(ancestors.size() < 128 && ancestors.insert(id).second, "Invalid copy-worker source ancestry");
        auto entry = location.Volume()->Stat(id);
        entry.identity = location.Volume()->WorkerEntryIdentity(entry.identity);
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
    void Launch(const std::vector<ImageHandleTransfer::File> &files, bool session = false) {
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
        if (!session && fault == 7) {
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
        if (!session && fault == failWorkerLaunch) {
            // A Win32 path with a reserved character cannot name an executable.
            wcscpy_s(executable, L"C:\\HFV-invalid-?.exe");
        }
#endif
        auto command = L"\"" + std::wstring(executable) + (session ? L"\" --volume-session " : L"\" --copy-worker ") +
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

enum class SessionCommandCode {
    Root, List, Stat, Read, Attributes, SetAttributes, MakeDirectory, Rename, Remove,
    HardLink, ValidateRemoval, ValidateCopy, Compression, Put, FinalizeDirectory, ValidateEntry, CopyWithin, GetClassicProperties, SetClassicProperties, CloseClassic, FindMfs, SetBasicMetadata, TestCrashRename, TestCrashPut
};
constexpr size_t sessionReadLimit = 256 * 1024;
#ifdef HFV_TESTING
std::atomic<HANDLE> readPauseEntered{nullptr}, readPauseRelease{nullptr};
#endif

void SessionReply(HANDLE output, unsigned code = 0, const std::string& error = {},
                  Storage::Rollback rollback = Storage::Rollback::None) {
    Data result;
    result.put(code); result.put(error); result.put(uint64_t(rollback));
    Send(output, SessionResult, std::move(result));
}

class SessionError : public HfsPlus::Error {
public:
    using HfsPlus::Error::Error;
};

class PersistentVolume final : public Volume {
    std::wstring path_, journal_;
    std::string key_;
    std::unique_ptr<HfsPlus::Source> guard_, journalGuard_;
    std::unique_ptr<VolumeCoord::ImageDependency> dependency_;
    mutable std::mutex mutex_;
    std::unique_ptr<Process> worker_;
    std::u16string name_;
    std::string description_;
    const ImageFormat format_;
    bool Classic()const{return format_==ImageFormat::Classic;}
    bool Mfs()const{return format_==ImageFormat::Mfs;}
    bool hfsx_ = false, caseSensitive_ = false, journaled_ = false;
    bool writeAllowed_ = false, initialized_ = false;
    std::atomic<bool> writable_{false}, lost_{false};
#ifdef HFV_TESTING
    bool crashRename_ = false, crashPut_ = false;
#endif
    uint64_t generation_;
    static uint64_t NextGeneration() {
        static std::atomic<uint64_t> sequence{0};
        return ++sequence;
    }
    bool Alive() const {
        return worker_ && WaitForSingleObject(worker_->process.value, 0) == WAIT_TIMEOUT;
    }
    void Lost() {
        if (!lost_) {
            generation_ = NextGeneration();
            lost_ = true;
            if (!journaled_) writeAllowed_ = false;
        }
        worker_.reset();
    }
    void Ready() {
        if (Alive()) return;
        if (worker_) Lost();
        throw HfsPlus::Error("The volume worker stopped. Refresh the volume before retrying any changes.");
    }
    void RecordRollback(Storage::Rollback rollback) {
        Storage::OutcomeEvidence::NoteRollback(rollback);
        if (format_!=ImageFormat::HfsPlus && rollback == Storage::Rollback::Failed) {
            writable_ = false;
            writeAllowed_ = false;
        }
    }
    void ReadResult(Data& packet) {
            auto code = packet.get();
            auto error = packet.text();
            auto rollback = packet.get();
            packet.end();
            Check(code <= 2 && rollback <= uint64_t(Storage::Rollback::Failed), "Invalid volume session result");
            RecordRollback(Storage::Rollback(rollback));
            if (code == 2) throw HfsPlus::ExternalJournalRequired(error);
            if (code == 1) throw SessionError(error);
    }
    Data Response(Message expected) {
        Data packet;
        const auto message = Receive(worker_->output.value, packet);
        if (message == SessionResult) ReadResult(packet);
        Check(message == expected, "Unexpected volume session response");
        return packet;
    }
    template<class Read>
    void Call(SessionCommandCode command, Data data, bool mutation, Read read,
              const HfsPlus::Attributes* attributes = nullptr) {
        Ready();
        try {
            Data request;
            request.put(uint64_t(command));
            request.bytes.insert(request.bytes.end(), data.bytes.begin(), data.bytes.end());
            if (attributes) {
                Storage::Job transfer;
                SendAttributes(worker_->input.value, 0, *attributes, transfer);
            }
            Send(worker_->input.value, SessionCommand, std::move(request));
            read();
            Response(SessionResult);
        } catch (const SessionError&) {
            throw;
        } catch (...) {
            Lost();
            if (mutation) Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Unverified);
            throw HfsPlus::Error("The volume worker stopped. Refresh and inspect the volume before retrying.");
        }
    }
    void Simple(SessionCommandCode command, Data data, bool mutation) {
        Call(command, std::move(data), mutation, [] {});
    }
    void Stop() {
        if (Classic() && Alive()) {
            try { Simple(SessionCommandCode::CloseClassic, {}, true); }
            catch (...) { Lost(); throw; }
        }
        worker_.reset();
    }
    void Start() {
        auto process = std::make_unique<Process>();
        struct Inherited {
            std::vector<ImageHandleTransfer::File> files;
            ~Inherited() { for (const auto& file : files) CloseHandle(file.handle); }
        } inherited;
        for (const auto& file : WorkerFiles()) {
            auto handle = ImageHandleTransfer::Duplicate(HANDLE(file.second), true);
            try { inherited.files.push_back({file.first, handle, writeAllowed_}); }
            catch (...) { CloseHandle(handle); throw; }
        }
        process->Launch(inherited.files, true);
        for (const auto& file : inherited.files) {
            Data value;
            value.wide(file.path); value.put(uintptr_t(file.handle)); value.put(file.writable);
            Send(process->input.value, File, std::move(value));
        }
        Data mount;
        mount.wide(path_); mount.wide(journal_); mount.put(writeAllowed_); mount.put(uint64_t(format_));
        Send(process->input.value, SessionMount, std::move(mount));
        worker_ = std::move(process);
        try {
            auto info = Response(SessionBytes);
            auto name = info.u16();
            auto description = info.text();
            auto hfsx = info.flag(), caseSensitive = info.flag(), journaled = info.flag();
            auto writable = info.flag();
            info.end();
            Response(SessionResult);
            // These descriptors are immutable after publication, so menu
            // updates do not wait on the pipe while a write is in flight.
            if (Mfs() && initialized_ && !lost_) generation_ = NextGeneration();
            if (!initialized_) {
                name_ = std::move(name);
                description_ = std::move(description);
                hfsx_ = hfsx;
                caseSensitive_ = caseSensitive;
                journaled_ = journaled;
                initialized_ = true;
            }
            writable_ = writable;
            lost_ = false;
        } catch (...) {
            worker_.reset();
            throw;
        }
    }
    Entry ReadEntry() {
        auto packet = Response(SessionEntry);
        auto entry = DecodeEntry(packet);
        packet.end();
        entry.identity = std::to_string(generation_) + "/" + entry.identity;
        return entry;
    }
public:
    PersistentVolume(std::wstring path, std::string key, std::wstring journal, ImageFormat format)
        : path_(std::move(path)), journal_(std::move(journal)), key_(std::move(key)), format_(format),
          generation_(NextGeneration()) {
        auto open = [&](bool write) {
            guard_ = HfsPlus::OpenImage(path_.c_str(), write);
            if (!journal_.empty()) journalGuard_ = HfsPlus::OpenImage(journal_.c_str(), write);
        };
        try { open(true); }
        catch (const HfsPlus::Error&) { guard_.reset(); open(false); }
        writeAllowed_ = guard_->Writable();
        if (!journal_.empty())
            dependency_ = std::make_unique<VolumeCoord::ImageDependency>(key_, journal_);
        Start();
    }

    ~PersistentVolume() override {
        try { Stop(); } catch (...) {}
    }
    bool HasPersistentWorker() const override { return true; }
    std::string ResourceKey() const override { return key_; }
    std::u16string Name() const override { return name_; }
    const char* Kind() const override { return Mfs() ? "MFS" : Classic() ? "HFS" : hfsx_ ? "HFSX" : "HFS+"; }
    std::string Description() const override { return description_; }
    bool Writable() const override { return writable_ && !lost_; }
    bool CaseSensitive() const override { return caseSensitive_; }
    bool Journaled() const override { return journaled_; }
    bool SupportsHardLinks() const override { return format_==ImageFormat::HfsPlus; }
    bool SupportsCompression() const override { return format_==ImageFormat::HfsPlus; }
    bool SupportsAttributes() const override { return format_==ImageFormat::HfsPlus; }
    Capabilities Supports() const override {
        auto result = Volume::Supports();
        result.unicodeNames = format_==ImageFormat::HfsPlus;
        result.createFolders = !Mfs();
        return result;
    }
    bool GetClassicProperties(uint32_t id, hfsdirent& entry) override {
        if (!Classic()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id);
        Call(SessionCommandCode::GetClassicProperties, std::move(args), false, [&] {
            auto result = Response(SessionBytes);
            entry = DecodeClassic(result); result.end();
        });
        return true;
    }
    bool FindChild(uint32_t parent,const std::u16string& name,Entry& found)override {
        return Mfs()?FindMfs(parent,name,{},found):Volume::FindChild(parent,name,found);
    }
    bool FindTransferChild(uint32_t parent,const Entry& source,Entry& found)override {
        return Mfs()?FindMfs(parent,source.name,source.version,found):Volume::FindTransferChild(parent,source,found);
    }
    bool FindMfs(uint32_t parent,const std::u16string& name,std::optional<unsigned> version,Entry& found) {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args;args.put(parent);args.u16(name);args.put(version.has_value());
        if(version)args.put(*version);
        bool exists=false;
        Call(SessionCommandCode::FindMfs,std::move(args),false,[&]{
            auto packet=Response(SessionBytes);
            exists=packet.flag();packet.end();
            if(exists)found=ReadEntry();
        });
        return exists;
    }
    bool SupportsBasicMetadata()const override{return Mfs();}
    void SetBasicMetadata(uint32_t id,const Entry& entry)override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args;args.put(id);EncodeEntry(args,entry);
        Simple(SessionCommandCode::SetBasicMetadata,std::move(args),true);
    }
    void SetClassicProperties(uint32_t id, const hfsdirent& entry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id); EncodeClassic(args, entry);
        Simple(SessionCommandCode::SetClassicProperties, std::move(args), true);
    }
    std::string WorkerEntryIdentity(const std::string& identity) const override {
        auto separator = identity.find('/');
        return separator == identity.npos ? identity : identity.substr(separator + 1);
    }
    WorkerOrigin WorkerDescription() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkerOrigin origin;
        origin.kind = WorkerOrigin::Image;
        origin.path = path_;
        origin.journal = journal_;
        origin.writable = writable_ && !lost_;
        return origin;
    }
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles() const override {
        auto files = guard_->WorkerFiles();
        if (journalGuard_) {
            auto journal = journalGuard_->WorkerFiles();
            files.insert(files.end(), journal.begin(), journal.end());
        }
        return files;
    }
    void SuspendWorker() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_ && !Alive()) Lost();
        Stop();
    }
    void WorkerUncleanExit() override {
        std::lock_guard<std::mutex> lock(mutex_);
        Lost();
    }
    void ResumeWorker() override { Reload(); }
    void Reload() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (Alive()) return;
        if (worker_) Lost();
        Start();
    }
    uint64_t FreeBytes() const override {
        auto& self = *const_cast<PersistentVolume*>(this);
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t bytes = 0;
        self.Call(SessionCommandCode::Root, {}, false, [&] {
            auto result = self.Response(SessionBytes); bytes = result.get(); result.end();
        });
        return bytes;
    }
    std::vector<Entry> List(uint32_t parent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(parent);
        std::vector<Entry> entries;
        Call(SessionCommandCode::List, std::move(args), false, [&] {
            auto packet = Response(SessionBytes); auto count = packet.get(); packet.end();
            Check(count <= UINT32_MAX, "Invalid volume directory count");
            for (uint64_t i = 0; i < count; ++i) entries.push_back(ReadEntry());
        });
        return entries;
    }
    Entry Stat(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id);
        Entry entry;
        Call(SessionCommandCode::Stat, std::move(args), false, [&] { entry = ReadEntry(); });
        return entry;
    }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void* out, size_t bytes) override {
        auto output = static_cast<uint8_t*>(out);
        uint64_t generation=0;
        do {
            const auto count = (std::min)(bytes, sessionReadLimit);
            {
                // Leave the pipe available for browsing between fork chunks.
                std::lock_guard<std::mutex> lock(mutex_);
                Check(!generation || generation == generation_, "The mounted volume changed during a fork read");
                generation=generation_;
                Data args; args.put(id); args.put(fork); args.put(offset); args.put(count);
                Call(SessionCommandCode::Read, std::move(args), false, [&] {
                    auto packet = Response(SessionBytes);
                    Check(packet.bytes.size() == count, "Short volume session read");
                    if (count) memcpy(output, packet.bytes.data(), count);
                });
            }
#ifdef HFV_TESTING
            if (!offset && bytes>count) {
                auto entered=readPauseEntered.load();
                if (entered) {
                    SetEvent(entered);
                    Check(WaitForSingleObject(readPauseRelease.load(),15000)==WAIT_OBJECT_0,
                          "The test did not release the paused fork read");
                }
            }
#endif
            if (count) output += count;
            offset += count; bytes -= count;
        } while (bytes);
    }
    HfsPlus::Attributes GetAttributes(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id);
        AttributeSets sets;
        Call(SessionCommandCode::Attributes, std::move(args), false, [&] {
            auto packet = Response(AttributeSet);
            ReceiveAttributes(worker_->output.value, packet, sets);
            Check(sets[0].has_value(), "Missing volume attributes");
        });
        return std::move(*sets[0]);
    }
    void SetAttributes(uint32_t id, const HfsPlus::Attributes& attributes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id);
        Call(SessionCommandCode::SetAttributes, std::move(args), true, [] {}, &attributes);
    }
    void SetCompressionMethod(uint32_t id, HfsPlus::Volume::CompressionMode method,
                              std::function<void(uint64_t,uint64_t)> progress) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id); args.put(uint64_t(method));
        std::exception_ptr stopped;
        try {
            Call(SessionCommandCode::Compression, std::move(args), true, [&] {
                for (;;) {
                    Data packet;
                    auto message = Receive(worker_->output.value, packet);
                    if (message == SessionResult) ReadResult(packet); // Early failure.
                    if (message == SessionBytes) { packet.end(); break; }
                    Check(message == SessionProgress, "Unexpected volume compression response");
                    auto done = packet.get(), total = packet.get(); packet.end();
                    try { if (progress) progress(done,total); }
                    catch (...) { stopped = std::current_exception(); }
                    Send(worker_->input.value, stopped ? Cancel : SessionContinue);
                }
            });
        } catch (const SessionError&) {
            if(stopped)std::rethrow_exception(stopped);
            throw;
        }
    }
    void SetCompression(uint32_t id,bool compressed,std::function<void(uint64_t,uint64_t)> progress) override {
        using Mode = HfsPlus::Volume::CompressionMode;
        SetCompressionMethod(id, compressed ? Mode::Zlib : Mode::Uncompressed, std::move(progress));
    }
    uint32_t MakeDirectory(uint32_t parent,const std::u16string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(parent); args.u16(name);
        uint32_t id = 0;
        Call(SessionCommandCode::MakeDirectory, std::move(args), true, [&] {
            auto packet = Response(SessionBytes); id = uint32_t(packet.get()); packet.end();
        });
        return id;
    }
    uint32_t Put(uint32_t parent, const Entry& entry, Reader reader, bool replace) override {
        return StreamPut(parent, entry, std::move(reader), replace, 0, {});
    }
    uint32_t PutResourceReplacement(uint32_t id, const Entry& entry, Reader reader,
                                     std::function<void(uint64_t,size_t)> localProgress) override {
        return StreamPut(entry.parent, entry, std::move(reader), true, id, std::move(localProgress));
    }
private:
    uint32_t StreamPut(uint32_t parent, const Entry& entry, Reader reader, bool replace, uint32_t original,
                       std::function<void(uint64_t,size_t)> localProgress) {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args;
        args.put(parent); args.put(replace); args.put(entry.attributesKnown); args.put(original);
        EncodeEntry(args, entry);
        uint32_t id = 0;
        std::exception_ptr sourceFailure;
        try {
            auto command = SessionCommandCode::Put;
#ifdef HFV_TESTING
            if (crashPut_) { crashPut_ = false; command = SessionCommandCode::TestCrashPut; }
#endif
            Call(command, std::move(args), true, [&] {
                for (;;) {
                    Data packet;
                    auto message = Receive(worker_->output.value, packet);
                    if (message == SessionResult) ReadResult(packet);
                    if (message == SessionBytes) {
                        const auto value = packet.get(); packet.end();
                        Check(value && value <= UINT32_MAX, "Invalid copied entry ID");
                        id = uint32_t(value);
                        break;
                    }
                    Check(message == SessionReadRequest && !sourceFailure, "Unexpected fork transfer response");
                    const auto fork = packet.get(), offset = packet.get(), size = packet.get();
                    packet.end();
                    Check(fork <= 1 && size && size <= sessionReadLimit, "Invalid fork transfer range");
                    const auto length = fork ? entry.forks[1].size : entry.forks[0].size;
                    Check(offset <= length && size <= length - offset, "Fork transfer exceeds source length");
                    const bool localRead = original && !fork;
                    Data bytes;
                    try {
                        if (localRead) {
                            if (localProgress) localProgress(offset, size_t(size));
                        } else {
                            bytes.bytes.resize(size_t(size));
                            reader(unsigned(fork), offset, bytes.bytes.data(), bytes.bytes.size());
                        }
                    } catch (...) {
                        sourceFailure = std::current_exception();
                    }
                    if (sourceFailure) Send(worker_->input.value, Cancel);
                    else if (localRead) Send(worker_->input.value, SessionContinue);
                    else Send(worker_->input.value, SessionBytes, std::move(bytes));
                }
            }, &entry.attributes);
        } catch (const SessionError&) {
            if (sourceFailure) std::rethrow_exception(sourceFailure);
            throw;
        }
        return id;
    }
public:
    bool CopyWithin(const std::vector<uint32_t>& ids, uint32_t parent, Storage::Job& job) {
        // Leave room for the command, destination catalog ID, and selection count.
        Check(!ids.empty() && ids.size() <= FrameLimit(SessionCommand) / 8 - 3, "Same-image copy selection exceeds the worker request size");
        std::lock_guard<std::mutex> lock(mutex_);
        Data args;
        args.put(parent); args.put(uint64_t(ids.size()));
        for (auto id : ids) args.put(id);
        bool complete = false, cancelled = false;
        std::string error;
        Call(SessionCommandCode::CopyWithin, std::move(args), true, [&] {
            for (;;) {
                Data packet;
                const auto message = Receive(worker_->output.value, packet);
                if (message == SessionResult) ReadResult(packet);
                if (message == SessionBytes) {
                    complete = packet.flag(); cancelled = packet.flag(); error = packet.text();
                    const auto rollback = packet.get();
                    const auto retained = packet.flag();
                    packet.end();
                    Check(rollback <= uint64_t(Storage::Rollback::Failed), "Invalid same-image copy rollback");
                    RecordRollback(Storage::Rollback(rollback));
                    if (retained) Storage::OutcomeEvidence::NoteRetainedChange();
                    break;
                }
                if (message == Progress) {
                    job.progress(DecodeProgress(packet)); packet.end();
                    Send(worker_->input.value, job.cancelled() ? Cancel : SessionContinue);
                } else {
                    Check(message == Conflict, "Unexpected same-image copy response");
                    auto path = packet.text(); packet.end();
                    Data choice;
                    choice.put(uint64_t(job.replace(path)));
                    Send(worker_->input.value, Choice, std::move(choice));
                }
            }
        });
        if (cancelled) throw Storage::Cancelled();
        if (!error.empty()) throw HfsPlus::Error(error);
        return complete;
    }
    void ValidateEntry(const Entry& entry) const override {
        auto& self = const_cast<PersistentVolume&>(*this);
        std::lock_guard<std::mutex> lock(self.mutex_);
        Data args;
        EncodeEntry(args, entry);
        self.Call(SessionCommandCode::ValidateEntry, std::move(args), false, [] {}, &entry.attributes);
    }
    void FinalizeDirectory(uint32_t id, const Entry& desired, const Entry& expected, bool merge) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args;
        args.put(id); args.put(merge); args.put(desired.attributesKnown);
        EncodeEntry(args, desired);
        EncodeEntry(args, expected);
        Call(SessionCommandCode::FinalizeDirectory, std::move(args), true, [] {}, &desired.attributes);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id); args.put(parent); args.u16(name);
        auto command = SessionCommandCode::Rename;
#ifdef HFV_TESTING
        if (crashRename_) { crashRename_ = false; command = SessionCommandCode::TestCrashRename; }
#endif
        Simple(command, std::move(args), true);
    }
    void Remove(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id); Simple(SessionCommandCode::Remove, std::move(args), true);
    }
    uint32_t MakeHardLink(uint32_t source,uint32_t parent,const std::u16string& name,bool replace=false) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(source); args.put(parent); args.u16(name); args.put(replace);
        uint32_t id = 0;
        Call(SessionCommandCode::HardLink, std::move(args), true, [&] {
            auto packet = Response(SessionBytes); id = uint32_t(packet.get()); packet.end();
        });
        return id;
    }
    void ValidateRemoval(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id); Simple(SessionCommandCode::ValidateRemoval, std::move(args), false);
    }
    void ValidateCopyTo(uint32_t id,Volume& target,uint32_t parent) override {
        if (target.ResourceKey()!=key_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        Data args; args.put(id); args.put(parent); Simple(SessionCommandCode::ValidateCopy, std::move(args), false);
    }
#ifdef HFV_TESTING
    DWORD Pid() const { std::lock_guard<std::mutex> lock(mutex_); return worker_ ? worker_->pid : 0; }
    void CrashOnRename() {
        std::lock_guard<std::mutex> lock(mutex_);
        Check(Alive(), "Fixture volume worker is not running");
        crashRename_ = true;
    }
    void CrashDuringPut() {
        std::lock_guard<std::mutex> lock(mutex_);
        Check((Classic() || Mfs()) && Alive(), "Non-journaled fixture worker is not running");
        crashPut_ = true;
    }
#endif
};

void ServeVolume(HANDLE input, HANDLE output, Volume& volume) {
    auto number = [](Data& packet) {
        auto value = packet.get();
        Check(value <= UINT32_MAX, "Invalid volume session catalog number");
        return uint32_t(value);
    };
    auto writeNumber = [&](uint64_t value) {
        Data packet; packet.put(value); Send(output, SessionBytes, std::move(packet));
    };
    auto writeEntry = [&](const Entry& entry) {
        Data packet; EncodeEntry(packet, entry); Send(output, SessionEntry, std::move(packet));
    };
    for (;;) {
        Data packet;
        auto message = Receive(input, packet);
        AttributeSets attributes;
        if (message == AttributeSet) {
            ReceiveAttributes(input, packet, attributes);
            message = Receive(input, packet);
        }
        Check(message == SessionCommand, "Invalid volume session command frame");
        auto opcode = packet.get();
    #ifdef HFV_TESTING
        constexpr auto maximumCommand = SessionCommandCode::TestCrashPut;
#else
        constexpr auto maximumCommand = SessionCommandCode::SetBasicMetadata;
#endif
        Check(opcode <= uint64_t(maximumCommand), "Invalid volume session command");
        auto command = SessionCommandCode(opcode);
        const bool carriesAttributes = command == SessionCommandCode::SetAttributes ||
            command == SessionCommandCode::Put || command == SessionCommandCode::TestCrashPut ||
            command == SessionCommandCode::FinalizeDirectory ||
            command == SessionCommandCode::ValidateEntry;
        Check(attributes[0].has_value() == carriesAttributes && !attributes[1],
              "Unexpected volume session attributes");
        Storage::OutcomeEvidence evidence;
        hfs_take_rollback();
        try {
            switch (command) {
            case SessionCommandCode::Root: packet.end(); writeNumber(volume.FreeBytes()); break;
            case SessionCommandCode::List: {
                auto id = number(packet); packet.end();
                auto entries = volume.List(id);
                writeNumber(entries.size());
                for (const auto& entry : entries) writeEntry(entry);
                break;
            }
            case SessionCommandCode::Stat: {
                auto id = number(packet); packet.end(); writeEntry(volume.Stat(id)); break;
            }
            case SessionCommandCode::Read: {
                auto id = number(packet), fork = number(packet);
                auto offset = packet.get(), size = packet.get(); packet.end();
                Check(fork <= 1 && size <= sessionReadLimit, "Invalid volume read range");
                Data bytes; bytes.bytes.resize(size_t(size));
                volume.Read(id, fork, offset, bytes.bytes.data(), bytes.bytes.size());
                Send(output, SessionBytes, std::move(bytes));
                break;
            }
            case SessionCommandCode::Attributes: {
                auto id = number(packet); packet.end();
                auto values = volume.GetAttributes(id);
                Storage::Job transfer;
                SendAttributes(output, 0, values, transfer);
                break;
            }
            case SessionCommandCode::SetAttributes: {
                auto id = number(packet); packet.end(); volume.SetAttributes(id, *attributes[0]); break;
            }
            case SessionCommandCode::MakeDirectory: {
                auto parent = number(packet); auto name = packet.u16(); packet.end();
                writeNumber(volume.MakeDirectory(parent, name)); break;
            }
#ifdef HFV_TESTING
            case SessionCommandCode::TestCrashRename:
#endif
            case SessionCommandCode::Rename: {
                auto id = number(packet), parent = number(packet);
                auto name = packet.u16(); packet.end(); volume.Rename(id, parent, name);
#ifdef HFV_TESTING
                if (command == SessionCommandCode::TestCrashRename) ExitProcess(79);
#endif
                break;
            }
            case SessionCommandCode::Remove: {
                auto id = number(packet); packet.end(); volume.Remove(id); break;
            }
            case SessionCommandCode::HardLink: {
                auto id = number(packet), parent = number(packet);
                auto name = packet.u16(); auto replace = packet.flag(); packet.end();
                writeNumber(volume.MakeHardLink(id, parent, name, replace)); break;
            }
            case SessionCommandCode::Compression: {
                auto id = number(packet); auto method = packet.get(); packet.end();
                using Mode = HfsPlus::Volume::CompressionMode;
                Check(method <= uint64_t(Mode::StoredInline), "Invalid volume compression method");
                volume.SetCompressionMethod(id, Mode(method), [&](uint64_t done, uint64_t total) {
                    Data progress; progress.put(done); progress.put(total);
                    Send(output, SessionProgress, std::move(progress));
                    Data answer; auto decision = Receive(input, answer); answer.end();
                    if (decision == Cancel) throw Storage::Cancelled();
                    Check(decision == SessionContinue, "Invalid volume compression decision");
                });
                Send(output, SessionBytes);
                break;
            }
#ifdef HFV_TESTING
            case SessionCommandCode::TestCrashPut:
#endif
            case SessionCommandCode::Put: {
                const auto parent = number(packet);
                const auto replace = packet.flag(), attributesKnown = packet.flag();
                const auto original = number(packet);
                auto entry = DecodeEntry(packet); packet.end();
                entry.attributesKnown = attributesKnown;
                entry.attributes = std::move(*attributes[0]);
                if (original) {
                    const auto current = volume.Stat(original);
                    Check(replace && entry.id == original && !current.directory && current.parent == parent &&
                          current.name == entry.name && current.forks[0].size == entry.forks[0].size,
                          "The file changed before resource fork replacement");
                }
#ifdef HFV_TESTING
                const bool crashMfs=command==SessionCommandCode::TestCrashPut && !strcmp(volume.Kind(),"MFS");
                struct CrashMfs {
                    explicit CrashMfs(bool enabled){MfsImageSource::crashAfterSectorWrite=enabled;}
                    ~CrashMfs(){MfsImageSource::crashAfterSectorWrite=false;}
                } crashGuard(crashMfs);
#endif
                auto read = [&](unsigned fork, uint64_t offset, void* out, size_t size) {
#ifdef HFV_TESTING
                    if (command == SessionCommandCode::TestCrashPut && !crashMfs && (fork || offset)) ExitProcess(80);
#endif
                    auto* buffer = static_cast<uint8_t*>(out);
                    while (size) {
                        const auto count = (std::min)(size, sessionReadLimit);
                        const bool localRead = original && !fork;
                        if (localRead) volume.Read(original, 0, offset, buffer, count);
                        Data range; range.put(fork); range.put(offset); range.put(count);
                        Send(output, SessionReadRequest, std::move(range));
                        Data bytes;
                        const auto reply = Receive(input, bytes);
                        if (reply == Cancel) { bytes.end(); throw Storage::Cancelled(); }
                        if (localRead) {
                            Check(reply == SessionContinue, "Invalid original data fork response");
                            bytes.end();
                        } else {
                            Check(reply == SessionBytes && bytes.bytes.size() == count, "Invalid fork transfer data");
                            memcpy(buffer, bytes.bytes.data(), count);
                        }
                        offset += count; buffer += count; size -= count;
                    }
                };
                const auto id = original ? volume.PutResourceReplacement(original, entry, std::move(read), {})
                                         : volume.Put(parent, entry, std::move(read), replace);
                writeNumber(id);
                break;
            }
            case SessionCommandCode::FinalizeDirectory: {
                const auto id = number(packet);
                const auto merge = packet.flag(), attributesKnown = packet.flag();
                auto desired = DecodeEntry(packet);
                auto expected = DecodeEntry(packet); packet.end();
                desired.attributesKnown = attributesKnown;
                desired.attributes = std::move(*attributes[0]);
                volume.FinalizeDirectory(id, desired, expected, merge);
                break;
            }
            case SessionCommandCode::ValidateEntry: {
                auto entry = DecodeEntry(packet); packet.end();
                entry.attributes = std::move(*attributes[0]);
                volume.ValidateEntry(entry);
                break;
            }
            case SessionCommandCode::CopyWithin: {
                const auto parent = number(packet);
                const auto count = packet.get();
                Check(count && count <= (packet.bytes.size() - packet.at) / 8, "Invalid same-image copy selection");
                const std::shared_ptr<Volume> borrowed(&volume, [](Volume*) {});
                std::vector<std::pair<std::shared_ptr<Volume>, uint32_t>> sources;
                sources.reserve(size_t(count));
                for (uint64_t i = 0; i < count; ++i) sources.emplace_back(borrowed, number(packet));
                packet.end();
                Storage::Job job;
                job.transport([&](const Storage::Progress& progress) {
                    Data update;
                    EncodeProgress(update, progress);
                    Send(output, Progress, std::move(update));
                    Data response;
                    const auto message = Receive(input, response);
                    response.end();
                    Check(message == Cancel || message == SessionContinue, "Invalid same-image progress response");
                    if (message == Cancel) job.cancel();
                }, [&](const std::string& path) {
                    Data conflict;
                    conflict.put(path);
                    Send(output, Conflict, std::move(conflict));
                    Data response;
                    Check(Receive(input, response) == Choice, "Missing same-image overwrite response");
                    const auto choice = response.get();
                    response.end();
                    Check(choice >= uint64_t(Storage::ConflictChoice::Replace) &&
                          choice <= uint64_t(Storage::ConflictChoice::Cancel) &&
                          choice != uint64_t(Storage::ConflictChoice::ReplaceAll), "Invalid same-image overwrite choice");
                    return Storage::ConflictChoice(choice);
                });
                bool complete = false, cancelled = false;
                std::string error;
                try { complete = MacBrowser::CopyMany(sources, volume, parent, job); }
                catch (const Storage::Cancelled&) { cancelled = true; }
                catch (const std::exception& failure) { error = failure.what(); }
                Storage::OutcomeEvidence::NoteRollback(Storage::Rollback(hfs_take_rollback()));
                Data result;
                result.put(complete); result.put(cancelled); result.put(error);
                result.put(uint64_t(evidence.RollbackState())); result.put(evidence.RetainedChanges());
                Send(output, SessionBytes, std::move(result));
                break;
            }
            case SessionCommandCode::FindMfs: {
                const auto parent=number(packet);
                Entry selector,found;
                selector.name=packet.u16();
                const auto byVersion=packet.flag();
                if(byVersion)selector.version=number(packet);
                packet.end();
                const auto exists=byVersion?volume.FindTransferChild(parent,selector,found)
                                           :volume.FindChild(parent,selector.name,found);
                writeNumber(exists);
                if(exists)writeEntry(found);
                break;
            }
            case SessionCommandCode::SetBasicMetadata: {
                const auto id=number(packet);auto entry=DecodeEntry(packet);packet.end();
                volume.SetBasicMetadata(id,entry);
                break;
            }
            case SessionCommandCode::CloseClassic:
                packet.end();
                Check(!strcmp(volume.Kind(),"HFS"), "Unexpected classic close command");
                volume.Close();
                SessionReply(output);
                return;
            case SessionCommandCode::GetClassicProperties: {
                auto id = number(packet); packet.end();
                hfsdirent entry{};
                Check(volume.GetClassicProperties(id, entry), "Classic Finder properties are unavailable");
                Data result; EncodeClassic(result, entry);
                Send(output, SessionBytes, std::move(result)); break;
            }
            case SessionCommandCode::SetClassicProperties: {
                auto id = number(packet); auto entry = DecodeClassic(packet); packet.end();
                volume.SetClassicProperties(id, entry); break;
            }
            case SessionCommandCode::ValidateRemoval: {
                auto id = number(packet); packet.end(); volume.ValidateRemoval(id); break;
            }
            case SessionCommandCode::ValidateCopy: {
                auto id = number(packet), parent = number(packet);
                packet.end(); volume.ValidateCopyTo(id, volume, parent); break;
            }
            }
            SessionReply(output);
        } catch (const std::exception& error) {
            Storage::OutcomeEvidence::NoteRollback(Storage::Rollback(hfs_take_rollback()));
            SessionReply(output, 1, error.what(), evidence.RollbackState());
        }
    }
}
} // namespace
std::shared_ptr<MacBrowser::Volume> MountPersistentImage(const std::wstring& path, const std::string& key,
                                                       const std::wstring& journal, ImageFormat format) {
    return std::make_shared<PersistentVolume>(path, key, journal, format);
}

int VolumeSessionChild(const char* inputText, const char* outputText) {
    HANDLE input = HANDLE(uintptr_t(_strtoui64(inputText, nullptr, 10)));
    HANDLE output = HANDLE(uintptr_t(_strtoui64(outputText, nullptr, 10)));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        std::vector<ImageHandleTransfer::File> files;
        for (;;) {
            Data data;
            auto message = Receive(input, data);
            if (message == File) {
                auto path = data.wide();
                auto handle = HANDLE(uintptr_t(data.get()));
                auto writable = data.flag();
                data.end();
                files.push_back({std::move(path), handle, writable});
                continue;
            }
            Check(message == SessionMount, "Expected a volume session mount");
            auto path = data.wide();
            auto journal = data.wide();
            auto writable = data.flag();
            auto format = data.get();
            Check(format<=uint64_t(ImageFormat::Mfs),"Invalid volume session image format");
            data.end();
            ImageHandleTransfer::Scope storage(files);
            std::shared_ptr<Volume> volume;
            if (format==uint64_t(ImageFormat::Mfs)) {
                Check(journal.empty(),"MFS has no external journal");
                volume=OpenMfsImage(path,writable);
            } else if (format==uint64_t(ImageFormat::Classic)) {
                Check(journal.empty(), "Classic HFS has no external journal");
                floppy_module_global_init();
                volume = OpenClassicImage(path, writable);
            } else {
                auto guard = HfsPlus::OpenImage(path.c_str(), writable, journal.empty() ? nullptr : journal.c_str());
                auto key = VolumeCoord::ImageKey(path);
                try { volume = FromHfsPlus(std::move(guard), writable, key, journal); }
                catch (const HfsPlus::ExternalJournalRequired&) { throw; }
                catch (const HfsPlus::Error&) {
                    if (!writable) throw;
                    volume = FromHfsPlus(HfsPlus::OpenImage(path.c_str(), false, journal.empty() ? nullptr : journal.c_str()), false, key, journal);
                }
            }
            Data info;
            info.u16(volume->Name()); info.put(volume->Description()); info.put(!strcmp(volume->Kind(), "HFSX"));
            info.put(volume->CaseSensitive()); info.put(volume->Journaled()); info.put(volume->Writable());
            Send(output, SessionBytes, std::move(info));
            SessionReply(output);
            ServeVolume(input, output, *volume);
            return 0;
        }
    } catch (const HfsPlus::ExternalJournalRequired& error) {
        try { SessionReply(output, 2, error.what()); } catch (...) {}
    } catch (const std::exception& error) {
        try { SessionReply(output, 1, error.what()); } catch (...) {}
    }
    return 0;
}
#ifdef HFV_TESTING
void PauseVolumeReadForTest(HANDLE entered,HANDLE release) {
    if(entered)readPauseRelease=release;
    readPauseEntered=entered;
}
DWORD VolumeSessionPidForTest(const std::shared_ptr<MacBrowser::Volume>& volume) {
    auto persistent = std::dynamic_pointer_cast<PersistentVolume>(volume);
    return persistent ? persistent->Pid() : 0;
}
void CrashVolumeSessionOnRenameForTest(const std::shared_ptr<MacBrowser::Volume>& volume) {
    auto persistent = std::dynamic_pointer_cast<PersistentVolume>(volume);
    Check(bool(persistent), "Fixture volume is not owned by a persistent worker");
    persistent->CrashOnRename();
}
void CrashVolumeSessionDuringPutForTest(const std::shared_ptr<MacBrowser::Volume>& volume) {
    auto persistent = std::dynamic_pointer_cast<PersistentVolume>(volume);
    Check(bool(persistent), "Fixture volume is not owned by a persistent worker");
    persistent->CrashDuringPut();
}
#endif
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
bool CopiesWithinMountedImage(const OrdinaryCopy& copy) {
    if (!copy.destination.IsMounted() || copy.sources.empty() ||
        !dynamic_cast<PersistentVolume*>(copy.destination.Volume().get())) return false;
    return std::all_of(copy.sources.begin(), copy.sources.end(), [&](const Location& source) {
        return source.IsMounted() && !source.cut && source.Volume() == copy.destination.Volume();
    });
}
bool CopyWithinMountedImage(Volume& volume, const std::vector<uint32_t>& ids, uint32_t parent, Storage::Job& job) {
    return dynamic_cast<PersistentVolume&>(volume).CopyWithin(ids, parent, job);
}
bool ConvertsWithMountedClassic(const LegacyConversion& conversion) {
    if (!conversion.selection) return false;
    auto classic = [](const Location& item) {
        return item.IsMounted() && item.Volume()->HasPersistentWorker() &&
               !strcmp(item.Volume()->Kind(), "HFS");
    };
    const auto& selection = *conversion.selection;
    if (conversion.source.IsWindows() && conversion.destination.IsClassic())
        return selection.source.IsWindows() && classic(selection.folder);
    if (conversion.source.IsClassic() && conversion.destination.IsWindows())
        return classic(selection.source) && selection.folder.IsWindows();
    return false;
}
bool UsesWorker(const Request &request) {
#ifdef HFV_TESTING
    if (!enabled)
        return false;
#endif
    auto persistent=[](const Location& item) { return item.IsMounted() && item.Volume()->HasPersistentWorker(); };
    auto windows=[](const Location& item) {
        return item.IsWindows() || (item.IsMounted() && !strcmp(item.Volume()->Kind(), "Windows"));
    };
    if (auto* copy = request.Get<OrdinaryCopy>(); copy && !copy->sources.empty()) {
        if (CopiesWithinMountedImage(*copy)) return false;
        if (persistent(copy->destination)) {
            const auto destinationKey = copy->destination.Volume()->ResourceKey();
            if (std::all_of(copy->sources.begin(), copy->sources.end(), [&](const Location& source) {
                return windows(source) || (persistent(source) && source.Volume()->ResourceKey() != destinationKey);
            })) return false;
        } else if (windows(copy->destination) &&
                   std::any_of(copy->sources.begin(), copy->sources.end(), persistent) &&
                   std::all_of(copy->sources.begin(), copy->sources.end(), [&](const Location& source) {
                       return windows(source) || persistent(source);
                   })) return false;
    }
    if (auto* conversion = request.Conversion(); conversion && ConvertsWithMountedClassic(*conversion)) {
        const char mode = conversion->source.IsWindows() ? conversion->options.input : conversion->options.output;
        if (mode && strchr("ambt5rR", mode)) return false;
    }
    if (auto* output=request.Get<ExportFileFork>(); output && persistent(output->source)) return false;
    if (auto* input=request.Get<ImportResourceFork>(); input && persistent(input->file) && windows(input->input))
        return false;
    if(auto* rename=request.Rename(); rename&&persistent(rename->source)) return false;
    if(auto* deletion=request.Delete(); deletion&&!deletion->selection.empty() &&
        std::all_of(deletion->selection.begin(),deletion->selection.end(),persistent)) return false;
    if(auto* folder=request.Get<CreateFolder>(); folder&&persistent(folder->parent)) return false;
    if(auto* link=request.Get<FileOperations::CreateHardLink>(); link&&persistent(link->source)&&persistent(link->parent)) return false;
    if(auto* metadata=request.Metadata()) {
        if(auto* edit=std::get_if<EditBasicMetadata>(metadata); edit && persistent(edit->item)) return false;
        if(auto* edit=std::get_if<EditClassicProperties>(metadata); edit && persistent(edit->item)) return false;
        if(auto* attrs=std::get_if<EditAttributes>(metadata); attrs&&persistent(attrs->item)) return false;
        if(auto* compression=std::get_if<ChangeCompression>(metadata);
            compression&&persistent(compression->item)) return false;
    }
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
        const auto& conversion = request.Legacy();
        const char mode = conversion.source.IsClassic() ? conversion.options.output : conversion.options.input;
        auto legacyAddress = [&](const Location& original, const Endpoint& captured, bool folder) {
            // The shared transfer engine already has typed endpoints. Only
            // the older converter needs names expressed as legacy HFS paths.
            if ((mode && strchr("ambt5", mode)) || captured.origin.kind != WorkerOrigin::Image ||
                !captured.location.IsMounted() || strcmp(captured.location.Volume()->Kind(), "HFS"))
                return Capture(original).location;
            std::string path(":");
            for (const auto& segment : captured.path) {
                hfsdirent entry{};
                Check(captured.location.Volume()->GetClassicProperties(segment.id, entry),
                      "Cannot identify a classic conversion path");
                const std::string macName(entry.name);
                mac_to_pc_charset(reinterpret_cast<unsigned char*>(entry.name));
                const std::string legacyName(entry.name);
                pc_to_mac_charset(reinterpret_cast<unsigned char*>(entry.name));
                Check(macName == entry.name, "The legacy converter cannot represent this classic HFS name");
                if (path.size() > 1) path += ':';
                path += legacyName;
            }
            if (folder && path.back() != ':') path += ':';
            return Location::Classic(AnsiPath(captured.origin.path), std::move(path));
        };
        if (request.Legacy().selection) {
            execution.Legacy().source = legacyAddress(request.Legacy().source, sources.front(), false);
            execution.Legacy().destination = legacyAddress(request.Legacy().destination, target, true);
        } else {
            execution.Legacy().source = Capture(request.Legacy().source).location;
            execution.Legacy().destination = Capture(request.Legacy().destination).location;
        }
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
                for (auto &volume : writableMounts) {
                    if (!volume->WorkerWritesSafe()) safe = false;
                    if (!strcmp(volume->Kind(), "HFS")) {
                        try { volume->Close(); }
                        catch (const std::exception&) { safe = false; }
                    }
                }
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
