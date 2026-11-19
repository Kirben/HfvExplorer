// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "WorkerEndpoint.h"
#include "MediaDiscovery.h"
#include "HfsPlusCd.h"
#include "MacBrowserModel.h"
#include "FileOperations.h"
#include "ReadOnlyCdBackend.h"
#include "StorageHandoff.h"
#include "HfsPlusDisk.h"
#include "CdImage.h"
#include "StorageJobDialog.h"
#include "resource.h"
#include "DeviceWait.h"
#include <cstdio>
using namespace HfsPlus;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw Error(message);
}
template <class F> void reject(F action, const char *message) {
    try {
        action();
    } catch (const Error &) {
        return;
    }
    throw Error(message);
}
struct Memory : Source {
    Bytes data;
    explicit Memory(size_t n) : data(n) {}
    uint64_t Size() const override { return data.size(); }
    bool Writable() const override { return true; }
    void Read(uint64_t o, void *p, size_t n) override {
        check(Disk::Range(o, n, data.size()), "fixture read bounds");
        memcpy(p, data.data() + o, n);
    }
    void Write(uint64_t o, const void *p, size_t n) override {
        check(Disk::Range(o, n, data.size()), "fixture write bounds");
        memcpy(data.data() + o, p, n);
    }
    void Flush() override {}
};
class Disc : public MediaImageSource {
  public:
    Bytes bytes;
    DWORD generation = 7, error = 0, openError = 0, readError = 0, verifyError = 0, delay = 0;
    unsigned opens = 0, reads = 0, checks = 0;
    bool shortRead = false, changeOnRead = false, workerOnly = false, wrongThread = false;
    explicit Disc(Bytes b) : bytes(std::move(b)) {}
    bool Open(int drive, std::string &reason) override {
        ++opens;
        check(drive == 3, "wrong injected CD drive");
        if (openError) {
            error = openError;
            reason = "Injected CD open failure";
            return false;
        }
        return true;
    }
    unsigned long long Size() const override { return bytes.size(); }
    bool Verify(DWORD &value) override {
        ++checks;
        if (workerOnly && GetCurrentThreadId() == StorageUi::state().thread) wrongThread = true;
        value = generation;
        if (verifyError) {
            error = verifyError;
            return false;
        }
        return true;
    }
    bool Read(unsigned long long o, DWORD n, void *p, DWORD &got) override {
        ++reads;
        if (workerOnly && GetCurrentThreadId() == StorageUi::state().thread) wrongThread = true;
        if (delay)
            Sleep(delay);
        got = 0;
        if (readError) {
            error = readError;
            return false;
        }
        if (!Disk::Range(o, n, bytes.size())) {
            error = ERROR_HANDLE_EOF;
            return false;
        }
        got = shortRead ? n - 1 : n;
        memcpy(p, bytes.data() + o, got);
        if (changeOnRead)
            ++generation;
        return true;
    }
    DWORD Error() const override { return error; }
};
void payload(unsigned fork, uint64_t offset, void *p, size_t n) {
    auto b = static_cast<uint8_t *>(p);
    for (size_t i = 0; i < n; ++i)
        b[i] = uint8_t((offset + i) * 37 + fork * 91);
}
Bytes fixture(Volume::FormatKind kind, bool partitioned, bool pending = false) {
    Memory memory(8 * 1024 * 1024);
    Volume::Format(memory, u"Optical tests", true, kind);
    {
        Volume v(memory, true);
        auto dir = v.MakeDirectory(2, u"Folder");
        v.Put(dir, u"Both forks", 190003, 71009, payload);
        if (kind == Volume::FormatKind::HfsXCaseSensitive) {
            v.Put(2, u"Readme", 1, 0, payload);
            v.Put(2, u"README", 2, 0, payload);
        }
        if (pending) {
            v.fault = [](const char *phase) {
                if (!strcmp(phase, "journal-committed"))
                    throw Error("Injected interruption");
            };
            reject([&] { v.MakeDirectory(2, u"Recovered from CD"); }, "journal fixture did not interrupt");
        }
    }
    if (!partitioned)
        return memory.data;
    const size_t base = 65536;
    Bytes bytes(base + memory.data.size());
    memcpy(bytes.data() + base, memory.data.data(), memory.data.size());
    Disk::P16(bytes.data(), 0x4552);
    Disk::P16(bytes.data() + 2, 512);
    Disk::P32(bytes.data() + 4, uint32_t(bytes.size() / 512));
    auto map = bytes.data() + 512;
    Disk::P16(map, 0x504d);
    Disk::P32(map + 4, 2);
    Disk::P32(map + 8, 1);
    Disk::P32(map + 12, 2);
    memcpy(map + 48, "Apple_partition_map", 19);
    auto p = bytes.data() + 1024;
    Disk::P16(p, 0x504d);
    Disk::P32(p + 4, 2);
    Disk::P32(p + 8, uint32_t(base / 512));
    Disk::P32(p + 12, uint32_t(memory.data.size() / 512));
    memcpy(p + 48, kind == Volume::FormatKind::HfsPlus ? "Apple_HFS" : "Apple_HFSX",
           kind == Volume::FormatKind::HfsPlus ? 9 : 10);
    // The Windows side of a hybrid must not displace the Apple partition map.
    bytes[32768] = 1;
    memcpy(bytes.data() + 32769, "CD001", 5);
    return bytes;
}
void content(Volume &v) {
    auto root = v.List();
    uint32_t dir = 0;
    for (auto &e : root)
        if (e.name == u"Folder")
            dir = e.id;
    check(dir != 0, "missing CD folder");
    auto files = v.List(dir);
    check(files.size() == 1 && files[0].name == u"Both forks", "CD directory listing");
    for (unsigned fork = 0; fork < 2; ++fork) {
        uint64_t length = fork ? 71009 : 190003;
        Bytes actual(static_cast<size_t>(length)), expected(actual.size());
        payload(fork, 0, expected.data(), expected.size());
        check(v.Read(files[0].id, fork, 0, actual.data(), actual.size()) == length && actual == expected,
              "CD fork extraction bytes");
        check(v.Read(files[0].id, fork, length - 13, actual.data(), 100) == 13, "CD fork EOF clipping");
    }
}
std::wstring wide(const std::string &s) { return std::wstring(s.begin(), s.end()); }
Bytes load(const std::string &path) {
    CFile file;
    check(file.Open(path.c_str(), CFile::modeRead) != FALSE, "open dumped image");
    Bytes b(size_t(file.GetLength()));
    check(file.Read(b.data(), UINT(b.size())) == b.size(), "read dumped image");
    return b;
}
} // namespace
extern bool TestAutomaticCdSelection(std::function<std::unique_ptr<Source>(int)>, CWnd *, int);
extern bool TestHfsPlusMainCdBrowser(std::unique_ptr<Source>, CWnd *, std::function<void()>);
int RunHfsPlusCdTest(const char *directory) {
    if (!CreateDirectoryA(directory, nullptr))
        return 2;
    std::string root = directory;
    FILE *report = nullptr;
    if (fopen_s(&report, (root + "\\hfsplus-cd-report.txt").c_str(), "w") || !report)
        return 2;
    CWnd owner;
    try {
        for (auto kind : {Volume::FormatKind::HfsPlus, Volume::FormatKind::HfsXCaseSensitive,
                          Volume::FormatKind::HfsXCaseInsensitive})
            for (bool partitioned : {false, true}) {
                auto bytes = fixture(kind, partitioned);
                auto media = std::make_shared<Disc>(bytes);
                auto source = OpenCd(3, media);
                check(!source->Writable() && source->Size() == bytes.size() && Volume::Recognizes(*source),
                      "native-CD adapter recognition");
                Volume v(*source);
                check(v.Journaled() && !v.Writable() && v.IsHfsX() == (kind != Volume::FormatKind::HfsPlus) &&
                          v.CaseSensitive() == (kind == Volume::FormatKind::HfsXCaseSensitive),
                      "CD volume mode");
                content(v);
                if (v.CaseSensitive()) {
                    unsigned names = 0;
                    for (auto &e : v.List())
                        if (e.name == u"Readme" || e.name == u"README")
                            ++names;
                    check(names == 2, "case-distinct CD entries");
                }
                reject([&] { source->Write(0, nullptr, 0); }, "CD source write accepted");
                source->Flush();
                reject([&] { Volume write(*source, true); }, "writable CD mount accepted");
                reject([&] { Volume::Format(*source, u"Unsafe"); }, "CD format accepted");
                reject([&] { v.MakeDirectory(2, u"Unsafe"); }, "CD directory creation accepted");
                uint8_t b[3];
                reject([&] { source->Read(source->Size() - 1, b, 3); }, "CD bounds overflow accepted");
                auto out =
                    root + "\\" + std::to_string(int(kind)) + (partitioned ? "-hybrid.cdr" : "-raw.cdr");
                auto job = Storage::worker().submit(MakeCdImageJob(3, out, media));
                job->wait();
                check(job->result().code == 0 && load(out) == bytes,
                      "HFS+ family dump differs from source data track");
                check(job->progress().processed == bytes.size(), "CD dump progress did not finish");
                auto image = OpenImage(wide(out).c_str(), false);
                Volume reopened(*image);
                content(reopened);
                check(reopened.IsHfsX() == v.IsHfsX() && reopened.CaseSensitive() == v.CaseSensitive(),
                      "dumped image comparison mode changed");
                check(media->bytes == bytes, "reading/dumping modified source CD");
            }
        auto pristine = fixture(Volume::FormatKind::HfsPlus, false);
        for (int mode = 0; mode < 4; ++mode) {
            auto media = std::make_shared<Disc>(pristine);
            auto source = OpenCd(3, media);
            Volume v(*source);
            v.List();
            if (mode == 0)
                ++media->generation;
            if (mode == 1)
                media->verifyError = ERROR_NOT_READY;
            if (mode == 2)
                media->verifyError = ERROR_ACCESS_DENIED;
            if (mode == 3) {
                media->changeOnRead = true;
                uint8_t bytes[512];
                reject([&] { source->Read(0, bytes, sizeof(bytes)); }, "in-flight changed CD read accepted");
            }
            reject([&] { v.List(); }, "stale cached CD directory exposed");
            reject([&] { v.Stat(2); }, "stale cached CD stat exposed");
            reject([&] { v.Read(2, 0, 0, nullptr, 0); }, "stale CD zero-length read accepted");
            media->generation = 7;
            media->verifyError = 0;
            reject([&] { v.List(); }, "invalid CD session silently revalidated");
        }
        for (int mode = 0; mode < 2; ++mode) {
            auto media = std::make_shared<Disc>(pristine);
            auto source = OpenCd(3, media);
            if (mode)
                media->shortRead = true;
            else
                media->readError = ERROR_CRC;
            uint8_t b[512];
            reject([&] { source->Read(0, b, sizeof(b)); }, "read/short-read error hidden");
            media->shortRead = false;
            media->readError = 0;
            reject([&] { source->CheckMedia(); }, "failed CD session revived");
        }
        {
            auto media = std::make_shared<Disc>(pristine);
            media->openError = ERROR_NOT_READY;
            reject([&] { OpenCd(3, media); }, "empty CD open accepted");
        }
        {
            auto media = std::make_shared<Disc>(pristine);
            reject([&] { OpenCd(-1, media); }, "invalid CD drive accepted");
            check(media->opens == 0, "invalid drive opened media");
        }
        {
            auto media = std::make_shared<Disc>(Bytes(123));
            reject([&] { OpenCd(3, media); }, "nonsector CD size accepted");
        }
        {
            auto media = std::make_shared<Disc>(Bytes(65536));
            auto source = OpenCd(3, media);
            check(!Volume::Recognizes(*source), "ISO-only/empty disc recognized as HFS+");
        }
        {
            auto bytes = pristine;
            bytes.resize(bytes.size() - 2048);
            auto media = std::make_shared<Disc>(std::move(bytes));
            auto source = OpenCd(3, media);
            reject([&] { Volume v(*source); }, "truncated HFS+ CD silently synthesized");
        }
        {
            auto bytes = fixture(Volume::FormatKind::HfsXCaseSensitive, true, true);
            auto media = std::make_shared<Disc>(bytes);
            auto source = OpenCd(3, media);
            Volume v(*source);
            bool found = false;
            for (auto &e : v.List())
                found |= e.name == u"Recovered from CD";
            check(found && media->bytes == bytes,
                  "read-only CD journal replay wrote source or lost transaction");
        }
        check(owner.CreateEx(0, AfxRegisterWndClass(0), "Hidden HFS+ CD owner", WS_OVERLAPPED, 0, 0, 100, 100,
                             nullptr, 0) != FALSE,
              "hidden CD owner");
        AfxGetApp()->m_pMainWnd = &owner;
        InitializeStorageUi();
        check(CdDriveFromRoot("D:\\") == 3 && CdDriveFromRoot("d:") == 3 &&
              CdDriveFromRoot("Z:/") == 25, "CD drive-root routing");
        for (auto path : {"", "D:\\folder", "disc.img", "1:\\", "\\\\server\\share"})
            check(CdDriveFromRoot(path) == -1, "non-root path triggered CD detection");
        check(CdDriveFromRoot(nullptr) == -1, "null path triggered CD detection");
        for (auto kind : {Volume::FormatKind::HfsPlus, Volume::FormatKind::HfsXCaseSensitive,
                          Volume::FormatKind::HfsXCaseInsensitive}) {
            for (bool partitioned : {false, true}) {
                auto media = std::make_shared<Disc>(fixture(kind, partitioned, false));
                media->workerOnly = true;
                check(TestAutomaticCdSelection([=](int drive) { return OpenCd(drive, media); }, &owner, 1)
                      && !media->wrongThread, "automatic CD drive selection/browser routing");
            }
        }
        for (int kind = 0; kind < 3; ++kind) {
            Bytes bytes(1024 * 1024);
            if (kind == 1) Disk::P16(bytes.data() + 1024, 0x4244); // classic HFS
            if (kind == 2) { bytes[32768] = 1; memcpy(bytes.data() + 32769, "CD001", 5); }
            auto media = std::make_shared<Disc>(bytes); media->workerOnly = true;
            check(TestAutomaticCdSelection([=](int drive) { return OpenCd(drive, media); }, &owner, 0)
                  && !media->wrongThread && bytes == media->bytes,
                  "non-HFS+ CD did not fall back to normal drive browsing");
        }
        for (bool noMedia : {false, true}) {
            auto media = std::make_shared<Disc>(pristine);
            if (noMedia) media->openError = ERROR_NOT_READY;
            else media->readError = ERROR_CRC;
            check(TestAutomaticCdSelection([=](int drive) { return OpenCd(drive, media); }, &owner, 0),
                  "empty/unreadable disc did not fall back to normal drive browsing");
        }
        {
            auto bytes = pristine;
            Disk::P32(bytes.data() + 1024 + 40, 0); // Invalid allocation-block size after signature detection.
            auto media = std::make_shared<Disc>(std::move(bytes));
            check(TestAutomaticCdSelection([=](int drive) { return OpenCd(drive, media); }, &owner, -1),
                  "recognized damaged HFS+ disc did not report a mount error");
        }
        {
            auto media = std::make_shared<Disc>(pristine);
            media->delay = 30;
            storageUiTest = {};
            auto source = OpenCd(3, media); media->workerOnly = true;
            check(TestHfsPlusMainCdBrowser(std::move(source), &owner, [&] { ++media->generation; }) && !media->wrongThread,
                  "CD browser controls/media-loss invalidation/owner restoration");
        }
        {
            readonly_cd_backend_init();
            auto path=root+"\\classic-cd.dsk";
            {
                auto image=CreateImage(wide(path).c_str(), 8*1024*1024);
            }
            char label[]="Classic CD";
            check(hfs_format(path.data(),0,label)==0,"format classic CD fixture");
            {
                auto original=MacBrowser::OpenClassic(path,":",true);
                const auto folder=original->MakeDirectory(2,u"Classic folder");
                MacBrowser::Entry file;file.name=u"Classic forks";
                file.forks[0].size=8192;file.forks[1].size=4096;
                memcpy(file.finder.data(),"TEXTttxt",8);
                original->Put(folder,file,payload,false);
                original->Close();
            }
            const auto raw=load(path);
            const size_t base=16*2048;
            Bytes image(base+raw.size());
            memcpy(image.data()+base,raw.data(),raw.size());
            Disk::P16(image.data(),0x4552);
            Disk::P16(image.data()+2,512);
            Disk::P32(image.data()+4,uint32_t(image.size()/512));
            auto map=image.data()+512;
            Disk::P16(map,0x504d);
            Disk::P32(map+4,1);
            Disk::P32(map+8,uint32_t(base/512));
            Disk::P32(map+12,uint32_t(raw.size()/512));
            memcpy(map+48,"Apple_HFS",9);
            auto media=std::make_shared<Disc>(image);
            auto cd=MacBrowser::OpenCdRoot("D:\\",[](const char*){return UINT(DRIVE_CDROM);},
                [media](int drive){return MacBrowser::OpenCd(drive,media);});
            check(cd&&!strcmp(cd->Kind(),"HFS")&&!cd->Writable()&&cd->Name()==u"Classic CD",
                  "classic CD was not mounted read-only by the common selector");
            const auto origin=MacBrowser::WorkerEndpoint::Find(*cd)->WorkerDescription();
            check(origin.kind==MacBrowser::WorkerOrigin::Cd&&origin.drive==3&&origin.generation==media->generation&&
                  origin.generationKnown,"classic CD lost the worker media identity");
            MacBrowser::Entry folder,file;
            check(cd->FindChild(2,u"Classic folder",folder)&&cd->FindChild(folder.id,u"Classic forks",file)&&
                  file.forks[0].size==8192&&file.forks[1].size==4096&&!memcmp(file.finder.data(),"TEXTttxt",8),
                  "classic CD listing lost folder, Finder metadata or a fork");
            for(unsigned fork=0;fork<2;++fork){
                Bytes actual(144),expected(144);
                cd->Read(file.id,fork,512,actual.data(),actual.size());
                payload(fork,512,expected.data(),expected.size());
                check(actual==expected,"classic CD returned the wrong fork bytes");
            }
            reject([&]{cd->Rename(file.id,folder.id,u"No write");},"classic CD allowed rename");
            const auto exportFolder=wide(root)+L"\\classic-export";
            check(CreateDirectoryW(exportFolder.c_str(),nullptr)!=FALSE,"create classic CD export directory");
            const auto target=FileOperations::Location::Windows(exportFolder);
            FileOperations::LegacyConversion conversion{
                FileOperations::Location::Physical(3,FileOperations::ClassicMedia::Cd,":"),
                target,false,false,false,{'m','a'}};
            conversion.selection=FileOperations::ConversionSelection{
                FileOperations::Location::Mounted(cd,file.id),target};
            auto copy=FileOperations::Submit(conversion);
            check(WaitForSingleObject(copy.Job()->event(),10000)==WAIT_OBJECT_0 &&
                  !copy.Job()->result().code &&
                  GetFileAttributesW((exportFolder+L"\\Classic forks.bin").c_str())!=INVALID_FILE_ATTRIBUTES,
                  "classic CD bypassed the configured MacBinary copy mode");
            ++media->generation;
            reject([&]{cd->ValidateMedia();},"classic CD accepted a replacement disc");
            cd->ReleaseMedia();
            reject([&]{cd->List(2);},"released classic CD remained readable");
            image.resize(image.size()-2048);
            auto shortened=MacBrowser::OpenCd(3,std::make_shared<Disc>(std::move(image)));
            check(shortened&&shortened->FindChild(2,u"Classic folder",folder)&&
                  shortened->FindChild(folder.id,u"Classic forks",file),
                  "shortened classic CD partition lost its readable catalog");
            Bytes tail(144),expected(144);
            shortened->Read(file.id,1,512,tail.data(),tail.size());
            payload(1,512,expected.data(),expected.size());
            check(tail==expected,"shortened classic CD returned wrong resource bytes");
            for(bool swap:{false,true}){
                const int drive=20;
                MediaDiscovery::Changed(1u<<drive);
                struct Signal {
                    HANDLE handle=CreateEventW(nullptr,TRUE,FALSE,nullptr);
                    ~Signal(){if(handle)CloseHandle(handle);}
                    bool Wait()const{return WaitForSingleObject(handle,5000)==WAIT_OBJECT_0;}
                    void Set()const{SetEvent(handle);}
                } entered,joined,release;
                check(entered.handle&&joined.handle&&release.handle,"create discovery test events");
                std::atomic<int> opens{0};
                std::shared_ptr<MacBrowser::Volume> left,right;
                auto probe=[&]{
                    ++opens;entered.Set();
                    check(release.Wait(),"shared optical test probe was not released");
                    return shortened;
                };
                Storage::Worker workers(2);
                auto first=workers.submit([&](Storage::Job& job){left=MediaDiscovery::Optical(drive,job,probe);return Storage::Result{};});
                const bool started=entered.Wait();
                auto second=workers.submit([&](Storage::Job& job){
                    job.transport([&](const Storage::Progress&){joined.Set();},{});
                    right=MediaDiscovery::Optical(drive,job,probe);return Storage::Result{};
                });
                const bool shared=joined.Wait();
                if(swap)MediaDiscovery::Changed(1u<<drive);
                release.Set();first->wait();second->wait();
                check(started&&shared&&opens==1,"two browser probes reopened one spinning-up CD");
                if(swap)check(!left&&!right&&first->result().error==ERROR_MEDIA_CHANGED&&
                              second->result().error==ERROR_MEDIA_CHANGED,"an old probe survived a media notification");
                else check(left==shortened&&right==left&&!first->result().code&&!second->result().code,
                           "the second browser did not receive the shared optical result");
                MediaDiscovery::Changed(1u<<drive);
            }
            fprintf(report,"PASS: concurrent optical probes share one open and a device notification invalidates both results.\n");
            fprintf(report,"PASS: classic HFS CD selection, read-only shared volume, both forks, Finder metadata and media-change invalidation.\n");
        }
        owner.DestroyWindow();
        AfxGetApp()->m_pMainWnd = nullptr;
        fprintf(report,
                "PASS: raw and Apple-partitioned/hybrid HFS+ and both HFSX modes on read-only CD sources, "
                "both-fork export, exact full-track dump/reopen, mutation rejection, media checks for cached "
                "catalogs and in-flight reads, latched errors, bounds/short/truncated reads, read-only "
                "journal replay, automatic drive-root routing for HFS+ and both HFSX modes, classic/ISO/empty/error "
                "fallback, damaged-volume errors, removed CD chooser, responsive browser controls and owner restoration; simulated CDs only.\n");
    } catch (const std::exception &e) {
        fprintf(report, "FAIL: %s\n", e.what());
        if (owner.m_hWnd)
            owner.DestroyWindow();
        AfxGetApp()->m_pMainWnd = nullptr;
        fclose(report);
        return 1;
    }
    fclose(report);
    return 0;
}
#endif
