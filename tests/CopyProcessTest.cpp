// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "CopyProcess.h"
#include "TypeMappingSnapshot.h"
#include "StorageHandoff.h"
#include "VolumeCoordinator.h"
#include "LegacyEngine.h"
#include "floppy.h"
#include "ReadOnlyCdBackend.h"
#include "ProcessFloppy.h"
#include "BackgroundCopies.h"
#include "StorageJobDialog.h"
#include <fcntl.h>
#include <cstdio>
extern "C" {
#include "hfs/libhfs/hfs.h"
}
namespace {
using namespace MacBrowser;
using namespace FileOperations;
void Check(bool ok, const char *text) {
    if (!ok)
        throw HfsPlus::Error(text);
}
std::wstring Wide(const std::string &p) { return {p.begin(), p.end()}; }
Location Native(std::shared_ptr<Volume> v, uint32_t id = 2) {
    Location l;
    l.type = LIST_MAC;
    l.volume = std::move(v);
    l.id = id;
    return l;
}
std::shared_ptr<Volume> Image(const std::wstring &path,
                              HfsPlus::Volume::FormatKind kind = HfsPlus::Volume::FormatKind::HfsPlus) {
    auto source = HfsPlus::CreateImage(path.c_str(), 16 * 1024 * 1024);
    HfsPlus::Volume::Format(*source, u"Process fixture", true, kind);
    source.reset();
    return OpenImage(path);
}
uint32_t Put(Volume &v, uint32_t parent, const std::u16string &name, uint64_t size = 300000) {
    Entry e;
    e.name = name;
    e.forks[0].size = size;
    e.forks[1].size = 173;
    e.finder[0] = 42;
    return v.Put(
        parent, e,
        [](unsigned fork, uint64_t at, void *output, size_t n) {
            auto p = static_cast<BYTE *>(output);
            for (size_t i = 0; i < n; ++i)
                p[i] = BYTE((at + i) * 7 + fork * 13);
        },
        false);
}
void Verify(Volume &v, uint32_t id, uint64_t size = 300000) {
    auto e = v.Stat(id);
    Check(e.forks[0].size == size && e.forks[1].size == 173 && e.finder[0] == 42,
          "worker fork/metadata lengths");
    std::vector<BYTE> buffer(65536);
    for (unsigned fork = 0; fork < 2; ++fork)
        for (uint64_t at = 0; at < e.forks[fork].size;) {
            auto n = size_t((std::min)(uint64_t(buffer.size()), e.forks[fork].size - at));
            v.Read(id, fork, at, buffer.data(), n);
            for (size_t i = 0; i < n; ++i)
                Check(buffer[i] == BYTE((at + i) * 7 + fork * 13), "worker fork contents");
            at += n;
        }
}
void Wait(const Operation &op) {
    Check(WaitForSingleObject(op.Job()->event(), 30000) == WAIT_OBJECT_0, "copy process timed out");
}
void Pass(const Operation &op) {
    Wait(op);
    if (op.Job()->result().code)
        throw HfsPlus::Error(op.Job()->result().message);
    Check(op.Snapshot().complete, "copy process incomplete");
    Check(CopyProcess::lastProcess && CopyProcess::lastProcess != GetCurrentProcessId(),
          "copy must run in a different process");
}
void Conflict(const Operation &op) {
    DWORD until = GetTickCount() + 10000;
    while (op.Job()->conflict().empty() && !op.Job()->done() && LONG(until - GetTickCount()) > 0)
        Sleep(10);
    if (op.Job()->done() && op.Job()->result().code)
        throw HfsPlus::Error(op.Job()->result().message);
    Check(!op.Job()->conflict().empty(), "worker overwrite prompt missing");
}
void Denied(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    Check(h == INVALID_HANDLE_VALUE, "image exclusivity lost during handoff");
}
std::shared_ptr<Volume> ClassicImage(const std::string &path) {
    LegacyEngine::Scope engine;
    floppy_module_global_init();
    {
        CFile f;
        Check(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite) != FALSE, "classic fixture");
        f.SetLength(4 * 1024 * 1024);
    }
    char label[] = "Classic";
    Check(hfs_format(const_cast<char *>(path.c_str()), 0, label) == 0, "classic format");
    return OpenClassic(path, "Classic:", true);
}
std::shared_ptr<Volume> MfsImageFixture(const std::wstring &path, bool dc42) {
    uint64_t size = 800 * 1024;
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        Check(f != INVALID_HANDLE_VALUE, "MFS fixture");
        LARGE_INTEGER length;
        length.QuadPart = size + (dc42 ? 84 + size / 512 * 12 : 0);
        bool ok = SetFilePointerEx(f, length, nullptr, FILE_BEGIN) && SetEndOfFile(f);
        CloseHandle(f);
        Check(ok, "MFS fixture size");
    }
    {
        MfsImageSource source;
        std::string error;
        Check(source.Open(path.c_str(), error, true), error.c_str());
        Check(MfsImage::Format(source, "MFS", size, dc42, error), error.c_str());
    }
    return OpenImage(path);
}
void Until(std::function<bool()> ready) {
    DWORD end = GetTickCount() + 30000;
    while (!ready()) {
        Check(LONG(end - GetTickCount()) > 0, "process UI wait timed out");
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        Sleep(10);
    }
}
} // namespace
// Intentionally exits without application teardown. Its child must observe the
// parent process handle, cancel cooperatively and finish rollback by itself.
int RunCopyParentExitTest(const char *directory) {
    try {
        Check(CreateDirectoryA(directory, nullptr) != FALSE, "parent-exit fixture directory");
        std::string root = directory;
        auto source = Image(Wide(root + "\\source.dsk")), target = Image(Wide(root + "\\target.dsk"));
        auto id = Put(*source, 2, u"Retained", 2 * 1024 * 1024);
        CopyProcess::enabled = true;
        CopyProcess::fault = 2;
        Request request;
        request.background = true;
        auto from = Native(source, id);
        from.cut = true;
        request.sources = {from};
        request.target = Native(target);
        auto op = Submit(request);
        DWORD end = GetTickCount() + 15000;
        while (!op.Job()->done() && !op.Job()->progress().fileBytes && LONG(end - GetTickCount()) > 0)
            Sleep(10);
        Check(!op.Job()->done() && op.Job()->progress().fileBytes > 0, "parent-exit copy did not start");
        FILE *file = fopen((root + "\\worker.pid").c_str(), "w");
        Check(file != nullptr, "parent-exit PID file");
        fprintf(file, "%lu", CopyProcess::lastProcess.load());
        fclose(file);
        ExitProcess(0);
    } catch (...) {
        return 1;
    }
}
int RunCopyReleaseTest(const char *directory) {
    if (!CreateDirectoryA(directory, nullptr))
        return 2;
    std::string root = directory;
    FILE *report = fopen((root + "\\release-copy-report.txt").c_str(), "w");
    if (!report)
        return 2;
    CopyProcess::enabled = true;
    int result = 0;
    try {
        Check(GetEnvironmentVariableW(L"HFV_COPY_WORKER_TEST_EXECUTABLE", nullptr, 0) > 0,
              "shipping worker executable not configured");
        auto source = Image(Wide(root + "\\source.dsk")),
             target = Image(Wide(root + "\\target.dsk"), HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
        auto id = Put(*source, 2, u"Shipping", 4096);
        HfsPlus::Attributes attrs;
        attrs[u"user.release"] = {7, 8, 9};
        source->SetAttributes(id, attrs);
        Request request;
        request.background = true;
        request.sources = {Native(source, id)};
        request.target = Native(target);
        Pass(Submit(request));
        Entry copied;
        Check(target->FindChild(2, u"Shipping", copied) && target->GetAttributes(copied.id) == attrs,
              "shipping worker metadata");
        Verify(*target, copied.id, 4096);
        auto again = Submit(request);
        Conflict(again);
        again.Job()->resolve(Storage::ConflictChoice::Replace);
        Pass(again);
        again = Submit(request);
        Conflict(again);
        again.Job()->cancel();
        Wait(again);
        Check(again.Job()->result().error == ERROR_CANCELLED, "shipping worker cancellation");
        Verify(*target, copied.id, 4096);
        auto plain = Put(*source, 2, u"Plain", 4096);
        auto classic = ClassicImage(root + "\\classic.dsk");
        request.sources = {Native(source, plain)};
        request.target = Native(classic);
        Pass(Submit(request));
        Check(classic->FindChild(2, u"Plain", copied), "shipping classic output");
        Verify(*classic, copied.id, 4096);
        auto mfs = MfsImageFixture(Wide(root + "\\mfs.img"), false);
        request.target = Native(mfs);
        Pass(Submit(request));
        Check(mfs->FindChild(2, u"Plain", copied), "shipping MFS output");
        Verify(*mfs, copied.id, 4096);
        fprintf(report,
                "PASS: actual Release executable in private headless worker mode; HFS+/HFSX, classic HFS and "
                "MFS writes, both forks/attributes, overwrite, cancellation and parent restoration. "
                "Generated ordinary images only; no discovery, devices or desktop control.\n");
    } catch (const std::exception &error) {
        fprintf(report, "FAIL: %s\n", error.what());
        result = 1;
    }
    CopyProcess::enabled = false;
    fclose(report);
    return result;
}
int RunCopyProcessTest(const char *directory) {
    if (!CreateDirectoryA(directory, nullptr))
        return 2;
    std::string root = directory;
    FILE *report = fopen((root + "\\copy-process-report.txt").c_str(), "w");
    if (!report)
        return 2;
    int result = 0;
    CopyProcess::enabled = true;
    try {
        for (bool create : {false, true}) {
            bool rejected = false;
            try {
                auto invalid = create ? HfsPlus::CreateImage(nullptr, 8 * 1024 * 1024)
                                      : HfsPlus::OpenImage(nullptr, false);
            } catch (const HfsPlus::Error &) {
                rejected = true;
            }
            Check(rejected, "null image path must be rejected before native I/O");
        }

        auto source = Image(Wide(root + "\\source.dsk")),
             target = Image(Wide(root + "\\target.dsk"), HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
        auto folder = source->MakeDirectory(2, u"Folder");
        auto file = Put(*source, folder, u"Both forks");
        HfsPlus::Attributes attrs;
        attrs[u"user.process"] = {1, 2, 3, 4};
        source->SetAttributes(file, attrs);
        Request request;
        request.background = true;
        request.sources = {Native(source, folder)};
        request.target = Native(target);
        Pass(Submit(request));
        Entry copied, child;
        Check(target->FindChild(2, u"Folder", copied) && target->FindChild(copied.id, u"Both forks", child),
              "worker directory traversal");
        Verify(*target, child.id);
        Check(target->GetAttributes(child.id) == attrs, "worker attributes");
        fprintf(report, "PASS: child process, journaled HFS+ to HFSX, recursive forks and attributes, parent "
                        "engines restored\n");
        auto again = Submit(request);
        Conflict(again);
        again.Job()->resolve(Storage::ConflictChoice::Skip);
        Wait(again);
        Check(!again.Job()->result().code && !again.Snapshot().complete &&
                  again.Job()->progress().skipped == 1,
              "worker skip");
        Verify(*target, child.id);
        again = Submit(request);
        Conflict(again);
        again.Job()->resolve(Storage::ConflictChoice::Replace);
        Pass(again);
        again = Submit(request);
        Conflict(again);
        again.Job()->cancel();
        Wait(again);
        Check(again.Job()->result().error == ERROR_CANCELLED, "worker cancel conflict");
        Verify(*target, child.id);
        fprintf(report, "PASS: overwrite Replace/Skip/Cancel and existing destination retained\n");
        CopyProcess::fault = 1;
        auto crash = Submit(request);
        Wait(crash);
        Check(crash.Job()->result().code && !crash.Snapshot().complete, "worker crash must fail");
        CopyProcess::fault = 0;
        Verify(*target, child.id);
        Pass([&] {
            auto op = Submit(request);
            Conflict(op);
            op.Job()->resolve(Storage::ConflictChoice::Replace);
            return op;
        }());
        fprintf(report, "PASS: worker crash detected, writable parent engines recovered, retry succeeds\n");
        CopyProcess::fault = 5;
        auto malformed = Submit(request);
        Wait(malformed);
        Check(malformed.Job()->result().code && !malformed.Snapshot().complete,
              "malformed protocol accepted");
        CopyProcess::fault = 0;
        Verify(*target, child.id);
        fprintf(report, "PASS: malformed worker protocol refused and image ownership restored\n");
        auto host = Wide(root + "\\host");
        Check(CreateDirectoryW(host.c_str(), nullptr) != FALSE, "host fixture directory");
        Request exportRequest;
        exportRequest.background = true;
        exportRequest.sources = {Native(source, file)};
        exportRequest.target.type = LIST_FAT;
        exportRequest.target.windowsPath = host;
        Pass(Submit(exportRequest));
        auto windows = ResolveWindowsFile(host + L"\\Both forks");
        Verify(*windows.first, windows.second);
        Check(windows.first->GetAttributes(windows.second) == attrs, "Windows companion attributes");
        fprintf(report, "PASS: HFS+ to Windows AppleDouble, both forks and attributes\n");
        {
            auto guardVolume = OpenWindows(host);
            auto guarded = Put(*guardVolume, 2, u"Unrelated guard", 4096);
            guardVolume->Read(guarded, 0, 0, nullptr, 0);
            Request independent;
            independent.background = true;
            independent.sources = {Native(guardVolume, guarded)};
            auto parent = target->MakeDirectory(2, u"Cache isolation");
            independent.target = Native(target, parent);
            Pass(Submit(independent));
            Denied(host + L"\\Unrelated guard");
            Verify(*guardVolume, guarded, 4096);
            fprintf(report, "PASS: unrelated Windows cached readers retain their file-version guards across "
                            "a storage handoff\n");
        }
        auto plain = Put(*source, 2, u"Plain", 4096);
        auto classic = ClassicImage(root + "\\classic.dsk");
        Request roundtrip;
        roundtrip.background = true;
        roundtrip.sources = {Native(source, plain)};
        roundtrip.target = Native(classic);
        Pass(Submit(roundtrip));
        Entry classicFile;
        Check(classic->FindChild(2, u"Plain", classicFile), "classic worker output");
        Verify(*classic, classicFile.id, 4096);
        {
            auto links = source->MakeDirectory(2, u"Links");
            auto original = Put(*source, links, u"First", 4096);
            source->MakeHardLink(original, links, u"Second");
            Request copy;
            copy.background = true;
            copy.sources = {Native(source, links)};
            copy.target = Native(target);
            Pass(Submit(copy));
            Entry folderEntry, first, second;
            Check(target->FindChild(2, u"Links", folderEntry) &&
                      target->FindChild(folderEntry.id, u"First", first) &&
                      target->FindChild(folderEntry.id, u"Second", second) &&
                      first.resolvedId == second.resolvedId && first.linkCount == 2,
                  "worker hard-link preservation");
            Verify(*target, second.id, 4096);
            auto directory = source->MakeDirectory(2, u"Directory content");
            Put(*source, directory, u"Inside", 4096);
            auto alias = source->MakeHardLink(directory, 2, u"Directory alias");
            auto inside = source->List(alias).front();
            auto out = target->MakeDirectory(2, u"Alias export");
            copy.sources = {Native(source, inside.id)};
            copy.target = Native(target, out);
            Pass(Submit(copy));
            Check(target->FindChild(out, u"Inside", first), "child selection inside directory hard link");
            Verify(*target, first.id, 4096);
            auto streamed = Put(*source, 2, u"Streamed metadata", 4096);
            HfsPlus::Attributes metadata;
            metadata[u"user.streamed"] = HfsPlus::AttributeData::Snapshot(
                1024 * 1024,
                [](uint64_t at, void *p, size_t n) {
                    for (size_t i = 0; i < n; ++i)
                        static_cast<BYTE *>(p)[i] = BYTE((at + i) * 11);
                },
                true);
            source->SetAttributes(streamed, metadata);
            source->SetCompression(streamed, true, {});
            copy.sources = {Native(source, streamed)};
            copy.target = Native(target);
            Pass(Submit(copy));
            Check(target->FindChild(2, u"Streamed metadata", first) &&
                      target->GetAttributes(first.id) == metadata,
                  "worker streamed attributes/compression decoding");
            Verify(*target, first.id, 4096);
            auto moveId = Put(*source, 2, u"In-place move", 4096),
                 moveFolder = source->MakeDirectory(2, u"Move target");
            auto moveSource = Native(source, moveId);
            moveSource.cut = true;
            copy.sources = {moveSource};
            copy.target = Native(source, moveFolder);
            Pass(Submit(copy));
            Check(source->FindChild(moveFolder, u"In-place move", first) && first.id == moveId,
                  "worker same-volume move identity");
            Verify(*source, first.id, 4096);
            Request fork;
            fork.background = true;
            fork.kind = Kind::ExportFork;
            fork.sources = {Native(source, plain)};
            fork.fork = 1;
            fork.target.type = LIST_FAT;
            fork.target.windowsPath = host + L"\\resource.bin";
            Pass(Submit(fork));
            auto replaceId = Put(*target, 2, u"Replace resource", 4096);
            Request replace;
            replace.background = true;
            replace.kind = Kind::ReplaceResource;
            Location raw;
            raw.type = LIST_FAT;
            raw.windowsPath = fork.target.windowsPath;
            replace.sources = {raw};
            replace.target = Native(target, replaceId);
            Pass(Submit(replace));
            Verify(*target, replaceId, 4096);
            fprintf(report, "PASS: hard links, directory-alias child selection, streamed 1 MiB attributes, "
                            "compressed input decoding, in-place move identity and fork export/import\n");
        }
        auto classic2 = OpenClassic(root + "\\classic.dsk", "Classic:", true);
        Request back;
        back.background = true;
        back.sources = {Native(classic2, classic2->List().front().id)};
        auto classicOut = target->MakeDirectory(2, u"Classic back");
        back.target = Native(target, classicOut);
        Pass(Submit(back));
        Check(target->FindChild(classicOut, u"Plain", child), "classic worker input");
        Verify(*target, child.id, 4096);
        fprintf(report, "PASS: classic HFS read/write handoff, shared classic mount restoration\n");
        for (bool dc42 : {false, true}) {
            auto mfs = MfsImageFixture(Wide(root + (dc42 ? "\\mfs.dc42" : "\\mfs.img")), dc42);
            roundtrip.target = Native(mfs);
            Pass(Submit(roundtrip));
            Entry e;
            Check(mfs->FindChild(2, u"Plain", e), "MFS process output");
            Verify(*mfs, e.id, 4096);
            auto dest = target->MakeDirectory(2, dc42 ? u"DC42 back" : u"MFS back");
            back.sources = {Native(mfs, e.id)};
            back.target = Native(target, dest);
            Pass(Submit(back));
            Check(target->FindChild(dest, u"Plain", e), "MFS process input");
            Verify(*target, e.id, 4096);
        }
        fprintf(report, "PASS: raw MFS and Disk Copy 4.2 read/write with stable browser IDs\n");
        auto extPath = Wide(root + "\\external.dsk"), journal = Wide(root + "\\journal.img");
        auto uuid = HfsPlus::CreateExternalJournalImage(journal.c_str(), 16 * 1024 * 1024);
        {
            auto disk = HfsPlus::CreateImage(extPath.c_str(), 16 * 1024 * 1024, journal.c_str());
            HfsPlus::Volume::Format(*disk, u"External", true);
            HfsPlus::Volume v(*disk, true);
            v.UseExternalJournal(uuid);
        }
        auto external = OpenImage(extPath, journal);
        roundtrip.target = Native(external);
        Pass(Submit(roundtrip));
        Check(external->FindChild(2, u"Plain", child), "external journal worker output");
        Verify(*external, child.id, 4096);
        Denied(journal);
        fprintf(report, "PASS: external journal handle transfer, write and parent replay\n");
        {
            auto originalMappings = TypeMappings::Capture();
            struct RestoreMappings {
                TypeMappings::Snapshot original;
                ~RestoreMappings() { TypeMappings::Apply(original); }
            } restore{originalMappings};
            TypeMappings::Snapshot custom;
            custom.windowsToHfs.push_back({"ZZZ", "TEXTTESTx"});
            custom.hfsToWindows.push_back({"TEXTTEST", "zzz"});
            TypeMappings::Apply(custom);
            auto input = root + "\\Mapped.zzz";
            {
                CFile f;
                Check(f.Open(input.c_str(), CFile::modeCreate | CFile::modeWrite) != FALSE, "mapped input");
                f.Write("DATA", 4);
            }
            Request import;
            import.kind = Kind::LegacyCopy;
            import.background = true;
            import.sourceVolume = root;
            import.sourcePath = input;
            import.targetVolume = root + "\\classic.dsk";
            import.targetPath = "Classic:";
            import.conversion.input = 'r';
            Pass(Submit(import));
            Entry mapped;
            Check(classic->FindChild(2, u"Mapped", mapped) && !memcmp(mapped.finder.data(), "TEXTTEST", 8),
                  "worker custom raw import mapping/extension strip");
            Request exportMapped;
            exportMapped.kind = Kind::LegacyCopy;
            exportMapped.background = true;
            exportMapped.sourceVolume = import.targetVolume;
            exportMapped.sourcePath = "Classic:Mapped";
            exportMapped.targetPath = root + "\\mapped-export";
            exportMapped.conversion.output = 'r';
            Pass(Submit(exportMapped));
            Check(GetFileAttributesA((root + "\\mapped-export.zzz").c_str()) != INVALID_FILE_ATTRIBUTES,
                  "worker raw export extension mapping");
            fprintf(report, "PASS: captured custom type/creator mappings, extension stripping and mapped raw "
                            "export names\n");
        }
        // Legacy conversion continues to use its original converter inside the child.
        Request conversion;
        conversion.kind = Kind::LegacyCopy;
        conversion.background = true;
        conversion.sourceVolume = root + "\\classic.dsk";
        conversion.sourcePath = "Classic:Plain";
        conversion.targetPath = root + "\\converted.bin";
        conversion.conversion.output = 'b';
        Pass(Submit(conversion));
        Check(GetFileAttributesA(conversion.targetPath.c_str()) != INVALID_FILE_ATTRIBUTES,
              "worker MacBinary conversion output");
        fprintf(report, "PASS: legacy MacBinary conversion in child process\n");
        auto slow = Put(*source, 2, u"Slow", 2 * 1024 * 1024);
        Request delayed;
        delayed.background = true;
        delayed.sources = {Native(source, slow)};
        delayed.target = Native(target);
        CopyProcess::fault = 2;
        auto running = Submit(delayed);
        DWORD until = GetTickCount() + 10000;
        while (!running.Job()->done() && running.Job()->progress().phase != "Copying" &&
               LONG(until - GetTickCount()) > 0)
            Sleep(10);
        Check(!running.Job()->done(), "slow worker did not stay active");
        Denied(Wide(root + "\\source.dsk"));
        Denied(Wide(root + "\\target.dsk"));
        auto unrelated = Image(Wide(root + "\\unrelated.dsk"));
        Put(*unrelated, 2, u"Independent", 4096);
        running.Job()->cancel();
        Wait(running);
        Check(running.Job()->result().error == ERROR_CANCELLED, "slow worker cancellation");
        CopyProcess::fault = 0;
        Check(source->Stat(slow).forks[0].size == 2 * 1024 * 1024, "cancel retained source");
        fprintf(report, "PASS: slow-read cancellation, continuous exclusive image locks, unrelated volume "
                        "stays usable\n");
        CopyProcess::fault = 3;
        auto failure = Submit(delayed);
        Wait(failure);
        Check(failure.Job()->result().code &&
                  failure.Job()->result().message.find("Injected worker read failure") != std::string::npos,
              "worker read failure propagation");
        CopyProcess::fault = 0;
        Check(source->Stat(slow).forks[0].size == 2 * 1024 * 1024, "read failure retained source");
        CopyProcess::fault = 4;
        auto interrupted = Submit(delayed);
        Wait(interrupted);
        Check(interrupted.Job()->result().code && !interrupted.Snapshot().complete,
              "mid-write worker crash must fail");
        CopyProcess::fault = 0;
        Check(source->Stat(slow).forks[0].size == 2 * 1024 * 1024, "worker crash retained source");
        Pass(Submit(delayed));
        Check(target->FindChild(2, u"Slow", child), "retry after worker write crash");
        Verify(*target, child.id, 2 * 1024 * 1024);
        fprintf(report, "PASS: read error and worker death during write, journal recovery and safe retry\n");
        {
            auto otherSource = Image(Wide(root + "\\parallel-source.dsk")),
                 otherTarget = Image(Wide(root + "\\parallel-target.dsk"));
            auto otherFile = Put(*otherSource, 2, u"Parallel", 4096);
            auto delayedFolder = target->MakeDirectory(2, u"Parallel delayed");
            Request first = delayed;
            first.target = Native(target, delayedFolder);
            CopyProcess::fault = 2;
            auto blocked = Submit(first);
            Until([&] { return blocked.Job()->progress().fileBytes > 0 || blocked.Job()->done(); });
            Check(!blocked.Job()->done(), "first process did not stay active");
            auto firstPid = CopyProcess::lastProcess.load();
            std::vector<Operation> queued;
            for (unsigned n = 0; n < 7; ++n)
                queued.push_back(Submit(first));
            CopyProcess::fault = 0;
            Request independent;
            independent.background = true;
            independent.sources = {Native(otherSource, otherFile)};
            independent.target = Native(otherTarget);
            Pass(Submit(independent));
            Check(!blocked.Job()->done() && CopyProcess::lastProcess != firstPid,
                  "independent copy process serialized behind reserved volume");
            for (auto &waiting : queued)
                waiting.Job()->cancel();
            for (auto &waiting : queued) {
                Wait(waiting);
                Check(waiting.Job()->result().error == ERROR_CANCELLED, "queued process copy cancellation");
            }
            Pass(blocked);
            Check(otherTarget->FindChild(2, u"Parallel", child), "independent child output");
            Verify(*otherTarget, child.id, 4096);
            fprintf(report, "PASS: simultaneous independent child processes, seven conflicting requests "
                            "queued without worker starvation, queued cancellation\n");
            auto nonjournal = MfsImageFixture(Wide(root + "\\unclean-mfs.img"), false);
            Request unsafe;
            unsafe.background = true;
            unsafe.sources = {Native(source, plain)};
            unsafe.target = Native(nonjournal);
            CopyProcess::fault = 1;
            auto failed = Submit(unsafe);
            Wait(failed);
            CopyProcess::fault = 0;
            Check(failed.Job()->result().code && !nonjournal->Writable() && nonjournal->List().empty(),
                  "unclean non-journaled image remained writable");
            fprintf(report, "PASS: non-journaled MFS reopened read-only after unclean worker exit\n");
        }
        {
            std::string diskPath = root + "\\floppy.img";
            {
                CFile disk;
                Check(disk.Open(diskPath.c_str(), CFile::modeCreate | CFile::modeWrite) != FALSE,
                      "process floppy fixture");
                disk.SetLength(1474560);
            }
            {
                LegacyEngine::Scope engine;
                char label[] = "Floppy";
                Check(hfs_format(&diskPath[0], 0, label) == 0, "process floppy format");
            }
            {
                ProcessFloppy floppy(Wide(diskPath));
                Check(readonly_floppy_attach_api(25, &floppy), "parent process floppy attachment");
                struct Detach {
                    ~Detach() {
                        readonly_floppy_attach_api(25, nullptr);
                        CopyProcess::floppyFixture.clear();
                        CopyProcess::floppyGeneration = 7;
                    }
                } detach;
                CopyProcess::floppyFixture = Wide(diskPath);
                Request physical;
                physical.background = true;
                physical.sources = {Native(source, plain)};
                physical.target.type = LIST_HFS;
                char path[MAX_PATH];
                get_floppy_volume_file_name(25, path);
                physical.target.classicImage = path;
                physical.target.classicPath = "Floppy:";
                Pass(Submit(physical));
                auto overwrite = Submit(physical);
                Conflict(overwrite);
                overwrite.Job()->resolve(Storage::ConflictChoice::Skip);
                Wait(overwrite);
                Check(!overwrite.Job()->result().code && !overwrite.Snapshot().complete,
                      "process floppy skip");
                overwrite = Submit(physical);
                Conflict(overwrite);
                overwrite.Job()->cancel();
                Wait(overwrite);
                Check(overwrite.Job()->result().error == ERROR_CANCELLED, "process floppy cancel");
                overwrite = Submit(physical);
                Conflict(overwrite);
                overwrite.Job()->resolve(Storage::ConflictChoice::Replace);
                Pass(overwrite);
                CopyProcess::floppyGeneration = 8;
                auto changed = Submit(physical);
                Wait(changed);
                Check(changed.Job()->result().error == ERROR_MEDIA_CHANGED,
                      "process floppy media-generation handoff");
            }
            auto disk = OpenClassic(diskPath, "Floppy:", false);
            Entry e;
            Check(disk->FindChild(2, u"Plain", e), "process floppy contents after parent cache reopen");
            Verify(*disk, e.id, 4096);
            fprintf(report,
                    "PASS: file-backed physical floppy worker write/readback, overwrite Skip/Cancel/Replace, "
                    "media-change refusal and cache reopening; no real devices\n");
        }
        {
            CWnd first, second;
            auto previous = AfxGetApp()->m_pMainWnd;
            InitializeStorageUi();
            BackgroundCopies::enabled = true;
            struct Cleanup {
                CWnd &first;
                CWnd &second;
                CWnd *previous;
                ~Cleanup() {
                    BackgroundCopies::Shutdown();
                    ShutdownStorageUi();
                    AfxGetApp()->m_pMainWnd = previous;
                    if (first.GetSafeHwnd())
                        first.DestroyWindow();
                    if (second.GetSafeHwnd())
                        second.DestroyWindow();
                }
            } cleanup{first, second, previous};
            auto cls = AfxRegisterWndClass(0);
            Check(first.CreateEx(0, cls, "Worker first owner", WS_OVERLAPPED, CRect(0, 0, 320, 240), nullptr,
                                 0) &&
                      second.CreateEx(0, cls, "Worker second owner", WS_OVERLAPPED, CRect(0, 0, 320, 240),
                                      nullptr, 0),
                  "hidden process owners");
            AfxGetApp()->m_pMainWnd = &first;
            auto uiFolder = target->MakeDirectory(2, u"Owner transfer");
            Request ui;
            ui.background = true;
            ui.sources = {Native(source, slow)};
            ui.target = Native(target, uiFolder);
            CopyProcess::fault = 2;
            bool completed = false;
            Check(BackgroundCopies::Start({ui}, &first, true, [&](bool ok) { completed = ok; }),
                  "nonmodal process copy start");
            Until([] {
                auto pending = Pending();
                return !pending.empty() && pending.front().Job()->progress().fileBytes > 0;
            });
            Check(first.IsWindowEnabled() && second.IsWindowEnabled(),
                  "copy process disabled browser owners");
            BackgroundCopies::TransferOwner(first.GetSafeHwnd(), second.GetSafeHwnd());
            first.DestroyWindow();
            AfxGetApp()->m_pMainWnd = &second;
            Check(BackgroundCopies::ProgressOwners(second.GetSafeHwnd()) > 0,
                  "process progress owner transfer");
            Until([] { return !BackgroundCopies::Active(); });
            Check(completed && !BackgroundCopies::LastResult().code && second.IsWindowEnabled(),
                  "worker completion after first owner closes");
            CopyProcess::fault = 0;
            Check(target->FindChild(uiFolder, u"Slow", child), "owner transfer copied data");
            Verify(*target, child.id, 2 * 1024 * 1024);
            fprintf(report, "PASS: real subprocess with modeless progress, enabled owners, first owner "
                            "destruction and progress reassignment; hidden windows only\n");
            auto closingFolder = target->MakeDirectory(2, u"Last owner closing");
            ui.target = Native(target, closingFolder);
            CopyProcess::fault = 2;
            Check(BackgroundCopies::Start({ui}, &second, true), "last-owner process start");
            Until([] {
                auto pending = Pending();
                return !pending.empty() && pending.front().Job()->progress().fileBytes > 0;
            });
            auto closing = Pending();
            Check(closing.size() == 1, "last-owner active operation");
            auto closingJob = closing.front().Job();
            BackgroundCopies::CancelAll();
            BackgroundCopies::Shutdown();
            CopyProcess::fault = 0;
            Check(!BackgroundCopies::Active() && closingJob->done() &&
                      closingJob->result().error == ERROR_CANCELLED,
                  "last-owner process shutdown did not drain cancellation");
            Check(!target->FindChild(closingFolder, u"Slow", child),
                  "last-owner cancellation left partial output");
            fprintf(report, "PASS: last-owner shutdown drains child cancellation and destination rollback\n");
        }
        {
            auto dir = root + "\\parent-exit";
            wchar_t executable[32768];
            Check(GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable)) > 0,
                  "parent-exit executable");
            auto command =
                L"\"" + std::wstring(executable) + L"\" --verify-copy-parent-exit \"" + Wide(dir) + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION info{};
            Check(CreateProcessW(executable, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &startup, &info) != FALSE,
                  "start parent-exit helper");
            CloseHandle(info.hThread);
            Check(WaitForSingleObject(info.hProcess, 30000) == WAIT_OBJECT_0, "parent-exit helper timeout");
            DWORD code;
            Check(GetExitCodeProcess(info.hProcess, &code) && !code, "parent-exit helper failed");
            CloseHandle(info.hProcess);
            FILE *file = fopen((dir + "\\worker.pid").c_str(), "r");
            Check(file != nullptr, "parent-exit worker PID");
            DWORD pid = 0;
            Check(fscanf(file, "%lu", &pid) == 1, "parent-exit worker PID parse");
            fclose(file);
            HANDLE childProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (childProcess) {
                Check(WaitForSingleObject(childProcess, 30000) == WAIT_OBJECT_0,
                      "orphan copy worker did not clean up");
                CloseHandle(childProcess);
            }
            auto original = OpenImage(Wide(dir + "\\source.dsk")),
                 destination = OpenImage(Wide(dir + "\\target.dsk"));
            Entry e;
            Check(original->FindChild(2, u"Retained", e), "parent death deleted move source");
            Verify(*original, e.id, 2 * 1024 * 1024);
            Check(!destination->FindChild(2, u"Retained", e),
                  "parent-death rollback left partial destination");
            fprintf(report, "PASS: browser process death cancels worker cooperatively, rollback completes, "
                            "move source retained and handles released\n");
        }
    } catch (const std::exception &error) {
        fprintf(report, "FAIL: %s\n", error.what());
        result = 1;
    } catch (...) {
        fprintf(report, "FAIL: unexpected exception\n");
        result = 1;
    }
    CopyProcess::fault = 0;
    CopyProcess::enabled = false;
    fclose(report);
    return result;
}
#endif
