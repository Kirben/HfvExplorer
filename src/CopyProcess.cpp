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
std::atomic<HANDLE> ioGate{nullptr};
#endif
namespace {
using namespace FileOperations;
using namespace MacBrowser;
constexpr uint64_t protocol = 3;
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
    IoGate,
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
void EncodeEndpoint(Data &d, const Endpoint &e) {
    const auto &l = e.location;
    d.put(uint64_t(l.kind()));
    d.put(l.cut);
    d.put(l.directory);
    d.put(l.expectedCatalogId);
    d.put(l.expectedParent);
    d.put(l.expectedIdentity);
    d.put(l.expectedResource);
    d.u16(l.expectedName);
    switch (l.kind()) {
    case Location::Kind::None:
        break;
    case Location::Kind::Windows:
        d.wide(l.WindowsPath());
        break;
    case Location::Kind::ClassicImage:
        d.put(l.ClassicImage());
        d.put(l.ClassicPath());
        break;
    case Location::Kind::ClassicMedia:
        d.put(l.Drive());
        d.put(uint64_t(l.Media()));
        d.put(l.ClassicPath());
        d.put(l.MediaGenerationKnown());
        if (l.MediaGenerationKnown())
            d.put(l.MediaGeneration());
        break;
    case Location::Kind::Mounted:
        d.put(l.Id());
        EncodeOrigin(d, e.origin);
        d.put(e.path.size());
        for (auto &p : e.path) {
            d.u16(p.name);
            d.put(p.resolvedId);
            d.put(p.version);
            d.put(p.directory);
            d.put(p.identity);
        }
        break;
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
uint32_t CatalogNumber(Data &d) {
    auto value = d.get();
    Check(value <= UINT32_MAX, "Catalog identifier exceeds its field width");
    return uint32_t(value);
}
Location DecodeEndpoint(Data &d, WorkerMounts &mounts, bool allowMounted = true) {
    auto tag = d.get();
    Check(tag <= uint64_t(Location::Kind::ClassicMedia), "Invalid typed worker location");
    Check(allowMounted || tag != uint64_t(Location::Kind::Mounted),
          "Legacy conversion cannot carry a mounted backend");
    Location l;
    l.cut = d.get() != 0;
    l.directory = d.get() != 0;
    l.expectedCatalogId = CatalogNumber(d);
    l.expectedParent = CatalogNumber(d);
    l.expectedIdentity = d.text();
    l.expectedResource = d.text();
    l.expectedName = d.u16();
    switch (Location::Kind(tag)) {
    case Location::Kind::None:
        return l;
    case Location::Kind::Windows:
        l.SetWindows(d.wide());
        return l;
    case Location::Kind::ClassicImage: {
        auto image = d.text(), path = d.text();
        l.SetImage(std::move(image), std::move(path));
        break;
    }
    case Location::Kind::ClassicMedia: {
        auto drive = d.get(), media = d.get();
        Check(drive < 26 && media <= uint64_t(ClassicMedia::Cd), "Invalid physical worker location");
        auto path = d.text();
        auto generation = d.get();
        Check(generation <= 1, "Invalid media-generation presence flag");
        auto guard = l;
        l = Location::Physical(int(drive), ClassicMedia(media), std::move(path));
        l.cut = guard.cut;
        l.directory = guard.directory;
        l.expectedCatalogId = guard.expectedCatalogId;
        l.expectedParent = guard.expectedParent;
        l.expectedIdentity = std::move(guard.expectedIdentity);
        l.expectedResource = std::move(guard.expectedResource);
        l.expectedName = std::move(guard.expectedName);
        if (generation)
            l.BindMediaGeneration(CatalogNumber(d));
        return l;
    }
    case Location::Kind::Mounted: {
        auto selectedId = CatalogNumber(d);
        Check(selectedId >= 2, "Invalid mounted catalog identifier");
        auto origin = DecodeOrigin(d);
        auto count = d.get();
        Check(count <= 128, "Invalid worker path depth");
        std::vector<Entry> path;
        for (uint64_t i = 0; i < count; ++i) {
            Entry e;
            e.name = d.u16();
            e.resolvedId = CatalogNumber(d);
            e.version = CatalogNumber(d);
            e.directory = d.get() != 0;
            e.identity = d.text();
            path.push_back(std::move(e));
        }
        Data key;
        EncodeOrigin(key, origin);
        auto found = mounts.volumes.find(key.bytes);
        if (found == mounts.volumes.end())
            found = mounts.volumes.emplace(key.bytes, OpenOrigin(origin)).first;
        auto volume = found->second;
        uint32_t id = 2;
        bool stable = !strcmp(volume->Kind(), "HFS+") || !strcmp(volume->Kind(), "HFSX");
        Check(!stable || count == 1, "Invalid stable catalog endpoint");
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
        return l;
    }
    }
    return l;
}
void EncodeSettings(Data &d, const Request &r, HANDLE parent, unsigned fault) {
    d.put(uint64_t(r.OperationKind()));
    d.u16(r.name);
    d.put(r.fork);
    if (const auto* legacy = r.Conversion()) {
        EncodeEndpoint(d, Endpoint{legacy->source, {}, {}});
        EncodeEndpoint(d, Endpoint{legacy->destination, {}, {}});
        d.put(legacy->directory);
        d.put(legacy->move);
        d.put(uint8_t(legacy->options.output));
        d.put(uint8_t(legacy->options.input));
        d.put(legacy->options.appledouble);
        d.put(legacy->options.verifyFloppy);
        d.put(legacy->options.targetGenerationKnown);
        d.put(legacy->options.targetGeneration);
    }
    d.put(uintptr_t(parent));
    d.put(fault);
}
void DecodeSettings(Data &d, Request &r, HANDLE &parent, unsigned &fault) {
    auto kind = d.get();
    Check(kind == uint64_t(Kind::Copy) || kind == uint64_t(Kind::LegacyCopy) ||
              kind == uint64_t(Kind::ExportFork) || kind == uint64_t(Kind::ReplaceResource),
          "Invalid worker operation");
    auto name = d.u16();
    auto fork = unsigned(d.get());
    if (kind == uint64_t(Kind::LegacyCopy)) {
        WorkerMounts unused;
        LegacyConversion legacy;
        legacy.source = DecodeEndpoint(d, unused, false);
        legacy.destination = DecodeEndpoint(d, unused, false);
        legacy.directory = d.get() != 0;
        legacy.move = d.get() != 0;
        legacy.options.output = char(d.get());
        legacy.options.input = char(d.get());
        legacy.options.appledouble = d.get() != 0;
        legacy.options.verifyFloppy = d.get() != 0;
        legacy.options.targetGenerationKnown = d.get() != 0;
        legacy.options.targetGeneration = DWORD(d.get());
        r = std::move(legacy);
    } else {
        r.SetKind(Kind(kind));
    }
    r.name = std::move(name);
    r.fork = fork;
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
#ifdef HFV_TESTING
    Handle gate;
#endif
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
    WorkerMounts none;
    auto location = DecodeEndpoint(data, none, false);
    data.end();
    return location;
}
#endif
bool UsesWorker(const Request &request) {
#ifdef HFV_TESTING
    if (!enabled)
        return false;
#endif
    return request.OperationKind() == Kind::Copy || request.OperationKind() == Kind::LegacyCopy ||
           request.OperationKind() == Kind::ExportFork || request.OperationKind() == Kind::ReplaceResource;
}
Storage::Result Run(const Request &request, Storage::Job &job, Completion &completion,
                    const std::vector<std::string> &keys, const std::vector<std::string> &writes) {
    std::vector<Endpoint> sources;
    for (auto &source : request.Sources())
        sources.push_back(Capture(source));
    auto target = Capture(request.Target());
    auto execution = request;
    if (request.OperationKind() == Kind::LegacyCopy) {
        execution.Legacy().source = Capture(request.Legacy().source).location;
        execution.Legacy().destination = Capture(request.Legacy().destination).location;
    }
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
        EncodeSettings(settings, execution, worker.parent.value, testFault);
        Send(worker.input.value, Settings, std::move(settings));

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
        for (auto &file : handoff.files) {
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
        HANDLE ioGate = nullptr;
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
                    request.Sources().push_back(DecodeEndpoint(packet, mounts));
                    packet.end();
                }
                request.Target() = DecodeEndpoint(target, mounts);
                target.end();
                if (request.OperationKind() == Kind::ReplaceResource && !request.Target().Volume() &&
                    request.Target().LegacyType() == LIST_FAT) {
                    auto endpoint = ResolveWindowsFile(request.Target().WindowsPath());
                    request.Target().SetMounted(endpoint.first, endpoint.second);
                    auto e = endpoint.first->Stat(endpoint.second);
                    request.Target().expectedParent = e.parent;
                }
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
            request.Sources().clear();
            request.Target() = {};
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
