// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CopyProcess.h"
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
extern "C" {
void HFSIFACE_get_dir_flush(void);
}
namespace CopyProcess {
#ifdef HFV_TESTING
std::wstring floppyFixture;
DWORD floppyGeneration = 7;
std::atomic<DWORD> lastProcess{0};
std::atomic<bool> enabled{false};
std::atomic<unsigned> fault{0};
#endif
namespace {
using namespace FileOperations;
using namespace MacBrowser;
constexpr uint64_t protocol = 1;
constexpr DWORD maximumFrame = 1024 * 1024;
enum Message : DWORD {
    Settings = 1,
    File,
    SourceLocation,
    TargetLocation,
    Go,
    Progress,
    Conflict,
    Choice,
    Cancel,
    Fixture,
    Mapping,
    Finished
};
void Check(bool ok, const char *message) {
    if (!ok)
        throw HfsPlus::Error(message);
}
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
struct Data {
    std::vector<uint8_t> bytes;
    size_t at = 0;
    void put(uint64_t value) {
        for (unsigned i = 0; i < 8; ++i)
            bytes.push_back(uint8_t(value >> (i * 8)));
    }
    uint64_t get() {
        Check(at <= bytes.size() && bytes.size() - at >= 8, "Truncated copy-worker packet");
        uint64_t out = 0;
        for (unsigned i = 0; i < 8; ++i)
            out |= uint64_t(bytes[at++]) << (i * 8);
        return out;
    }
    void put(const std::string &s) {
        put(uint64_t(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    std::string text() {
        auto n = get();
        Check(n <= bytes.size() - at, "Invalid copy-worker string length");
        std::string out(reinterpret_cast<const char *>(bytes.data() + at), size_t(n));
        at += size_t(n);
        return out;
    }
    void wide(const std::wstring &s) {
        put(std::string(reinterpret_cast<const char *>(s.data()), s.size() * sizeof(wchar_t)));
    }
    std::wstring wide() {
        auto s = text();
        Check(s.size() % 2 == 0, "Invalid copy-worker UTF-16 string");
        std::wstring out(s.size() / 2, 0);
        if (!s.empty())
            memcpy(&out[0], s.data(), s.size());
        return out;
    }
    void u16(const std::u16string &s) { wide(MacBrowser::Wide(s)); }
    std::u16string u16() { return U16(wide()); }
    void end() { Check(at == bytes.size(), "Trailing copy-worker packet data"); }
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
    Check(data.bytes.size() <= maximumFrame, "Copy-worker packet exceeds the protocol frame size");
    DWORD header[] = {DWORD(protocol), DWORD(message), DWORD(data.bytes.size())};
    Exact(pipe, header, sizeof(header), true);
    if (!data.bytes.empty())
        Exact(pipe, data.bytes.data(), DWORD(data.bytes.size()), true);
}
Message Receive(HANDLE pipe, Data &data) {
    DWORD header[3];
    Exact(pipe, header, sizeof(header), false);
    Check(header[0] == protocol && header[1] >= Settings && header[1] <= Finished &&
              header[2] <= maximumFrame,
          "Invalid copy-worker protocol frame");
    data = Data();
    data.bytes.resize(header[2]);
    if (header[2])
        Exact(pipe, data.bytes.data(), header[2], false);
    return Message(header[1]);
}
bool Available(HANDLE pipe) {
    DWORD n = 0;
    Check(PeekNamedPipe(pipe, nullptr, 0, nullptr, &n, nullptr) != FALSE,
          "Copy-worker control connection closed");
    return n != 0;
}
void EncodeProgress(Data &d, const Storage::Progress &p) {
    d.put(p.path);
    d.put(p.phase);
    d.put(p.detail);
    d.put(p.bytes);
    d.put(p.processed);
    d.put(p.fileBytes);
    d.put(p.fileTotal);
    d.put(p.totalBytes);
    d.put(p.files);
    d.put(p.skipped);
    d.put(p.totalFiles);
    d.put(p.scanning);
    d.put(p.fileCopy);
    d.put(p.byteTransfer);
    d.put(p.utf8);
}
Storage::Progress DecodeProgress(Data &d) {
    Storage::Progress p;
    p.path = d.text();
    p.phase = d.text();
    p.detail = d.text();
    p.bytes = d.get();
    p.processed = d.get();
    p.fileBytes = d.get();
    p.fileTotal = d.get();
    p.totalBytes = d.get();
    p.files = unsigned(d.get());
    p.skipped = unsigned(d.get());
    p.totalFiles = unsigned(d.get());
    p.scanning = d.get() != 0;
    p.fileCopy = d.get() != 0;
    p.byteTransfer = d.get() != 0;
    p.utf8 = d.get() != 0;
    return p;
}
void EncodeOrigin(Data &d, const WorkerOrigin &o) {
    d.put(o.kind);
    d.wide(o.path);
    d.wide(o.journal);
    d.put(o.root);
    d.put(o.identity);
    d.put(uint64_t(o.drive + 1));
    d.put(o.offset);
    d.put(o.generation);
    d.put(o.generationKnown);
    d.put(o.writable);
}
WorkerOrigin DecodeOrigin(Data &d) {
    WorkerOrigin o;
    auto kind = d.get();
    Check(kind <= WorkerOrigin::Removable, "Invalid worker volume kind");
    o.kind = WorkerOrigin::Kind(kind);
    o.path = d.wide();
    o.journal = d.wide();
    o.root = d.text();
    o.identity = d.text();
    o.drive = int(d.get()) - 1;
    o.offset = d.get();
    o.generation = DWORD(d.get());
    o.generationKnown = d.get() != 0;
    o.writable = d.get() != 0;
    return o;
}
struct Endpoint {
    Location location;
    WorkerOrigin origin;
    std::vector<Entry> path;
};
void CaptureMedia(Location &l) {
    if (l.mediaGenerationKnown || l.type != LIST_HFS || l.classicImage.empty())
        return;
    int floppy = physical_floppy_path(l.classicImage.c_str());
    if (floppy >= 0) {
        if (!physical_floppy_generation(floppy, &l.mediaGeneration)) {
            HANDLE opened = floppy_init(floppy, TRUE);
            bool valid = opened && get_floppy_size(opened, floppy) > 0 &&
                         physical_floppy_generation(floppy, &l.mediaGeneration);
            if (opened)
                floppy_final(floppy);
            Check(valid, "Cannot establish selected floppy identity");
        }
        l.mediaGenerationKnown = true;
    } else if (readonly_media_path(l.classicImage.c_str())) {
        l.mediaGenerationKnown =
            physical_cd_generation(toupper((unsigned char)l.classicImage[0]) - 'A', &l.mediaGeneration) != 0;
    }
}
Endpoint Capture(const Location &location) {
    Endpoint out;
    out.location = location;
    if (!location.volume) {
        CaptureMedia(out.location);
        return out;
    }
    auto host = location.volume->HostPath(location.id);
    if (!host.empty()) {
        out.location.volume.reset();
        out.location.type = LIST_FAT;
        out.location.windowsPath = host;
        out.location.id = 2;
        return out;
    }
    out.origin = location.volume->WorkerDescription();
    if (out.origin.kind == WorkerOrigin::Classic) {
        out.location.classicImage = location.classicImage;
        if (out.location.classicImage.empty()) {
            int n = WideCharToMultiByte(CP_ACP, 0, out.origin.path.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string path(n, 0);
            WideCharToMultiByte(CP_ACP, 0, out.origin.path.c_str(), -1, &path[0], n, nullptr, nullptr);
            path.pop_back();
            out.location.classicImage = path;
        }
        CaptureMedia(out.location);
    }
    Check(out.origin.kind != WorkerOrigin::Unavailable,
          "This storage backend cannot be transferred to the copy worker");
    auto selected = location.volume->Stat(location.id);
    Check(location.expectedIdentity.empty() || location.expectedIdentity == selected.identity,
          "The selected entry was replaced before the operation started");
    Check(location.expectedName.empty() || location.expectedName == selected.name,
          "The selected entry was renamed before the operation started");
    Check(!location.expectedParent || location.expectedParent == selected.parent,
          "The selected entry was moved before the operation started");
    if (!strcmp(location.volume->Kind(), "HFS+") || !strcmp(location.volume->Kind(), "HFSX")) {
        out.path.push_back(selected);
        return out;
    }
    std::set<uint32_t> ancestors;
    for (uint32_t id = location.id; id != 2;) {
        Check(ancestors.size() < 128 && ancestors.insert(id).second, "Invalid copy-worker source ancestry");
        auto entry = location.volume->Stat(id);
        out.path.push_back(entry);
        id = entry.parent;
    }
    std::reverse(out.path.begin(), out.path.end());
    return out;
}
void EncodeEndpoint(Data &d, const Endpoint &e) {
    const auto &l = e.location;
    d.put(uint64_t(l.type));
    d.put(l.id);
    d.put(l.cut);
    d.put(l.directory);
    d.wide(l.windowsPath);
    d.put(l.classicImage);
    d.put(l.classicPath);
    d.put(l.expectedCatalogId);
    d.put(l.expectedParent);
    d.put(l.mediaGenerationKnown);
    d.put(l.mediaGeneration);
    d.put(l.expectedIdentity);
    d.put(l.expectedResource);
    d.u16(l.expectedName);
    d.put(bool(l.volume));
    if (l.volume) {
        EncodeOrigin(d, e.origin);
        d.put(e.path.size());
        for (auto &p : e.path) {
            d.u16(p.name);
            d.put(p.resolvedId);
            d.put(p.version);
            d.put(p.directory);
            d.put(p.identity);
        }
    }
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
        Check(now.generationKnown && now.generation == o.generation,
              "Media changed before the copy worker opened it; select the volume again");
    }
    Check(!o.writable || volume->Writable(), "The worker could not obtain writable storage access");
    return volume;
}
struct WorkerMounts {
    std::map<std::vector<uint8_t>, std::shared_ptr<Volume>> volumes;
};
Location DecodeEndpoint(Data &d, WorkerMounts &mounts) {
    Location l;
    l.type = int(d.get());
    l.id = uint32_t(d.get());
    l.cut = d.get() != 0;
    l.directory = d.get() != 0;
    l.windowsPath = d.wide();
    l.classicImage = d.text();
    l.classicPath = d.text();
    l.expectedCatalogId = uint32_t(d.get());
    l.expectedParent = uint32_t(d.get());
    l.mediaGenerationKnown = d.get() != 0;
    l.mediaGeneration = DWORD(d.get());
    l.expectedIdentity = d.text();
    l.expectedResource = d.text();
    l.expectedName = d.u16();
    bool native = d.get() != 0;
    if (native) {
        auto origin = DecodeOrigin(d);
        Data key;
        EncodeOrigin(key, origin);
        auto found = mounts.volumes.find(key.bytes);
        if (found == mounts.volumes.end())
            found = mounts.volumes.emplace(key.bytes, OpenOrigin(origin)).first;
        l.volume = found->second;
        auto selectedId = l.id;
        l.id = 2;
        bool stable = !strcmp(l.volume->Kind(), "HFS+") || !strcmp(l.volume->Kind(), "HFSX");
        auto count = d.get();
        Check(!stable || count == 1, "Invalid stable catalog endpoint");
        Check(count <= 128, "Invalid worker path depth");
        for (uint64_t i = 0; i < count; ++i) {
            auto name = d.u16();
            auto raw = d.get(), version = d.get();
            bool directory = d.get() != 0;
            auto identity = d.text();
            bool matched = false;
            auto candidates = stable ? std::vector<Entry>{l.volume->Stat(selectedId)} : l.volume->List(l.id);
            for (auto &e : candidates)
                if (e.name == name && e.version == version && e.directory == directory &&
                    (!raw || e.resolvedId == raw)) {
                    Check(!matched, "Ambiguous worker source path");
                    Check(origin.kind == WorkerOrigin::Removable || e.identity == identity,
                          "The selected entry changed before worker execution");
                    l.id = e.id;
                    matched = true;
                }
            Check(matched, "The selected entry is no longer present");
        }
        auto e = l.volume->Stat(l.id);
        l.expectedIdentity = e.identity;
        l.expectedParent = e.parent;
        l.expectedName = e.name;
    }
    return l;
}
void EncodeSettings(Data &d, const Request &r, HANDLE parent, unsigned fault) {
    d.put(uint64_t(r.kind));
    d.u16(r.name);
    d.put(r.fork);
    d.put(r.sourceVolume);
    d.put(r.sourcePath);
    d.put(r.targetVolume);
    d.put(r.targetPath);
    d.put(r.directory);
    d.put(r.move);
    d.put(uint8_t(r.conversion.output));
    d.put(uint8_t(r.conversion.input));
    d.put(r.conversion.appledouble);
    d.put(r.conversion.verifyFloppy);
    d.put(r.conversion.targetGenerationKnown);
    d.put(r.conversion.targetGeneration);
    d.put(uintptr_t(parent));
    d.put(fault);
}
void DecodeSettings(Data &d, Request &r, HANDLE &parent, unsigned &fault) {
    auto kind = d.get();
    Check(kind == uint64_t(Kind::Copy) || kind == uint64_t(Kind::LegacyCopy) ||
              kind == uint64_t(Kind::ExportFork) || kind == uint64_t(Kind::ReplaceResource),
          "Invalid worker operation");
    r.kind = Kind(kind);
    r.name = d.u16();
    r.fork = unsigned(d.get());
    r.sourceVolume = d.text();
    r.sourcePath = d.text();
    r.targetVolume = d.text();
    r.targetPath = d.text();
    r.directory = d.get() != 0;
    r.move = d.get() != 0;
    r.conversion.output = char(d.get());
    r.conversion.input = char(d.get());
    r.conversion.appledouble = d.get() != 0;
    r.conversion.verifyFloppy = d.get() != 0;
    r.conversion.targetGenerationKnown = d.get() != 0;
    r.conversion.targetGeneration = DWORD(d.get());
    parent = HANDLE(uintptr_t(d.get()));
    fault = unsigned(d.get());
}
struct Handoff {
    std::vector<std::shared_ptr<Volume>> volumes, suspended;
    std::vector<ImageHandleTransfer::File> files;
    std::vector<HANDLE> owned;
    bool restored = false;
    ~Handoff() {
        if (!restored) {
            try {
                Restore();
            } catch (...) {
            }
        }
        for (auto h : owned)
            CloseHandle(h);
    }
    void Prepare(const std::vector<std::string> &keys, const std::vector<std::string> &writes) {
        volumes = WorkerVolumes(keys);
        HFSIFACE_get_dir_flush();
        for (auto &volume : volumes)
            if (!volume->HostPath(2).empty())
                volume->ReleaseWorkerReaders(writes);
        for (auto &volume : volumes)
            for (auto &file : volume->WorkerFiles()) {
                auto existing =
                    std::find_if(files.begin(), files.end(), [&](const ImageHandleTransfer::File &p) {
                        return ImageHandleTransfer::Canonical(p.path) ==
                               ImageHandleTransfer::Canonical(file.first);
                    });
                if (existing != files.end()) {
                    if (volume->Writable())
                        existing->writable = true;
                    continue;
                }
                HANDLE copy = ImageHandleTransfer::Duplicate(HANDLE(file.second), true);
                owned.push_back(copy);
                files.push_back({file.first, copy, volume->Writable()});
            }
        // Capture every handle before closing any classic mounts shared by adapters.
        for (auto &volume : volumes) {
            if (!volume->WorkerFiles().empty() || volume->WorkerDescription().kind == WorkerOrigin::Classic) {
                suspended.push_back(volume);
                volume->SuspendWorker();
            }
        }
        for (int drive = 0; drive < 26; ++drive)
            if (std::find(keys.begin(), keys.end(), VolumeCoord::DeviceKey(drive)) != keys.end())
                physical_floppy_suspend(drive);
    }
    void Failed() {
        for (auto &volume : suspended)
            volume->WorkerUncleanExit();
    }
    void Restore() {
        if (restored)
            return;
        restored = true;
        HFSIFACE_get_dir_flush();
        ImageHandleTransfer::Scope transferred(files);
        std::string failure;
        for (auto &volume : suspended)
            try {
                volume->ResumeWorker();
            } catch (const std::exception &error) {
                if (!failure.empty())
                    failure += "; ";
                failure += error.what();
            }
        if (!failure.empty())
            throw HfsPlus::Error("Copy worker finished, but browser storage could not be reopened: " +
                                 failure);
    }
};
struct Process {
    Handle input, output, process, thread, parent;
    DWORD pid = 0;
    ~Process() {
        input.reset();
        output.reset();
        if (process.value)
            WaitForSingleObject(process.value, INFINITE);
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
bool UsesWorker(const Request &request) {
#ifdef HFV_TESTING
    if (!enabled)
        return false;
#endif
    return request.kind == Kind::Copy || request.kind == Kind::LegacyCopy ||
           request.kind == Kind::ExportFork || request.kind == Kind::ReplaceResource;
}
Storage::Result Run(const Request &request, Storage::Job &job, Completion &completion,
                    const std::vector<std::string> &keys, const std::vector<std::string> &writes) {
    std::vector<Endpoint> sources;
    for (auto &source : request.sources)
        sources.push_back(Capture(source));
    auto target = Capture(request.target);
    Handoff handoff;
    Storage::Result result;
    bool launched = false;
    try {
        handoff.Prepare(keys, writes);
        Process worker;
        worker.Launch(handoff.files);
        launched = true;
#ifdef HFV_TESTING
        lastProcess = worker.pid;
        unsigned testFault = fault.load();
#else
        unsigned testFault = 0;
#endif
        Data settings;
        EncodeSettings(settings, request, worker.parent.value, testFault);
        Send(worker.input.value, Settings, std::move(settings));

#ifdef HFV_TESTING
        if (!floppyFixture.empty()) {
            Data d;
            d.wide(floppyFixture);
            d.put(floppyGeneration);
            Send(worker.input.value, Fixture, std::move(d));
        }
#endif
        for (auto &file : handoff.files) {
            Data d;
            d.wide(file.path);
            d.put(uintptr_t(file.handle));
            d.put(file.writable);
            Send(worker.input.value, File, std::move(d));
        }
        if (request.kind == Kind::LegacyCopy) {
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
        // Mount and resolve only after the child's complete inherited-file table exists.
        for (auto &source : sources) {
            Data d;
            EncodeEndpoint(d, source);
            Send(worker.input.value, SourceLocation, std::move(d));
        }
        Data destination;
        EncodeEndpoint(destination, target);
        Send(worker.input.value, TargetLocation, std::move(destination));
        Send(worker.input.value, Go);
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
                } else if (message == Finished) {
                    result.code = int(d.get());
                    result.error = DWORD(d.get());
                    result.message = d.text();
                    result.moved = d.get() != 0;
                    completion.complete = d.get() != 0;
                    completion.completed = unsigned(d.get());
                    if (!d.get())
                        handoff.Failed();
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
        auto failure = std::current_exception();
        if (launched)
            handoff.Failed();
        try {
            handoff.Restore();
        } catch (const std::exception &restore) {
            std::string original = "Copy worker failed";
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception &e) {
                original = e.what();
            } catch (...) {
            }
            throw HfsPlus::Error(original + "; " + restore.what());
        }
        std::rethrow_exception(failure);
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
        std::wstring floppyPath;
        DWORD floppyGeneration = 7;
        TypeMappings::Snapshot mappings;
        Request request;
        HANDLE parent = nullptr;
        unsigned testFault = 0;
        std::vector<ImageHandleTransfer::File> files;
        std::vector<Data> sourcePackets;
        Data target;
        bool settings = false, hasTarget = false;
        for (;;) {
            Data d;
            auto message = Receive(input, d);
            if (message == Settings) {
                Check(!settings, "Repeated worker settings");
                DecodeSettings(d, request, parent, testFault);
                settings = true;
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
            } else if (message == Mapping) {
                auto direction = d.get();
                Check(direction < 2, "Invalid type-mapping direction");
                auto key = d.text(), value = d.text();
                (direction ? mappings.windowsToHfs : mappings.hfsToWindows)
                    .emplace_back(std::move(key), std::move(value));
            } else if (message == SourceLocation) {
                sourcePackets.push_back(std::move(d));
                continue;
            } else if (message == TargetLocation) {
                Check(!hasTarget, "Repeated worker target");
                target = std::move(d);
                hasTarget = true;
                continue;
            } else if (message == Go) {
                d.end();
                break;
            } else
                throw HfsPlus::Error("Invalid worker initialization");
            d.end();
        }
        Check(settings && hasTarget && parent, "Incomplete worker initialization");
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
                for (auto &packet : sourcePackets) {
                    request.sources.push_back(DecodeEndpoint(packet, mounts));
                    packet.end();
                }
                request.target = DecodeEndpoint(target, mounts);
                target.end();
                if (request.kind == Kind::ReplaceResource && !request.target.volume &&
                    request.target.type == LIST_FAT) {
                    auto endpoint = ResolveWindowsFile(request.target.windowsPath);
                    request.target.volume = endpoint.first;
                    request.target.id = endpoint.second;
                    auto e = endpoint.first->Stat(endpoint.second);
                    request.target.expectedParent = e.parent;
                }
#ifdef HFV_TESTING
                bool injected = false;
                HfsPlus::workerIoFault = [&](bool write, uint64_t, size_t n) {
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
                result = FileOperations::ExecuteCopyWorker(request, job, completion);
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
            request.sources.clear();
            request.target.volume.reset();
            mounts.volumes.clear();
        }
        if (!safe && !result.code)
            result = {1, ERROR_WRITE_FAULT,
                      "Worker storage recovery was incomplete; inspect the volume before further writes"};
        stop = true;
        control.join();
        Data final;
        final.put(uint64_t(result.code));
        final.put(result.error);
        final.put(result.message);
        final.put(result.moved);
        final.put(completion.complete);
        final.put(completion.completed);
        final.put(safe);
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
