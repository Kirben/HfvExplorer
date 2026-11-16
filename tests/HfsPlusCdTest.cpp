// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "HfsPlusCd.h"
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
extern bool TestHfsPlusCdBrowser(std::unique_ptr<Source>, CWnd *, std::function<void()>);
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
        {
            auto media = std::make_shared<Disc>(pristine);
            media->delay = 30;
            storageUiTest = {};
            auto source = OpenCd(3, media); media->workerOnly = true;
            check(TestHfsPlusCdBrowser(std::move(source), &owner, [&] { ++media->generation; }) && !media->wrongThread,
                  "CD browser controls/media-loss invalidation/owner restoration");
        }
        owner.DestroyWindow();
        AfxGetApp()->m_pMainWnd = nullptr;
        fprintf(report,
                "PASS: raw and Apple-partitioned/hybrid HFS+ and both HFSX modes on read-only CD sources, "
                "both-fork export, exact full-track dump/reopen, mutation rejection, media checks for cached "
                "catalogs and in-flight reads, latched errors, bounds/short/truncated reads, read-only "
                "journal replay, responsive browser controls and owner restoration; simulated CDs only.\n");
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
