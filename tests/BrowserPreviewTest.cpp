// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "BrowserPreview.h"
#include "FileOperations.h"
#include "StorageLocation.h"
#include "VolumeCoordinator.h"
#include <atomic>
#include <cstdio>

namespace {
using BrowserPreview::Request;
using FileOperations::Location;

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Event {
    HANDLE handle = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    Event() { Check(handle != nullptr, "create preview test event"); }
    ~Event() { CloseHandle(handle); }
};

class SlowVolume : public MacBrowser::Volume {
public:
    Event reading, release, returned;
    std::atomic<bool> replaced{false};
    size_t received = 0;

    std::u16string Name() const override { return u"Slow fixture"; }
    const char* Kind() const override { return "Fixture"; }
    bool Writable() const override { return false; }
    bool CaseSensitive() const override { return false; }
    uint64_t FreeBytes() const override { return 0; }
    std::vector<MacBrowser::Entry> List(uint32_t) override { return {Stat(3)}; }
    MacBrowser::Entry Stat(uint32_t id) override {
        Check(id == 3, "preview used wrong catalog ID");
        MacBrowser::Entry entry;
        entry.id = id;
        entry.parent = 2;
        entry.name = u"Slow";
        entry.identity = MacBrowser::EntryIdentity("slow");
        entry.forks[0].size = BrowserPreview::SampleLimit + 100;
        return entry;
    }
    void ValidateMedia() override {
        if (replaced) throw HfsPlus::Error("Media changed");
    }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void* data, size_t size) override {
        Check(id == 3 && fork == 0 && offset == 0, "preview read the wrong fork or offset");
        received = size;
        SetEvent(reading.handle);
        Check(WaitForSingleObject(release.handle, 8000) == WAIT_OBJECT_0, "preview fixture was not released");
        memset(data, 's', size);
        SetEvent(returned.handle);
    }
    uint32_t Put(uint32_t,const MacBrowser::Entry&,MacBrowser::Reader,bool) override {
        throw HfsPlus::Error("preview tried to write");
    }
    uint32_t MakeDirectory(uint32_t,const std::u16string&) override { throw HfsPlus::Error("read-only"); }
    void Rename(uint32_t,uint32_t,const std::u16string&) override { throw HfsPlus::Error("read-only"); }
    void Remove(uint32_t) override { throw HfsPlus::Error("read-only"); }
};

Request::State Await(Request& request, BrowserPreview::Text& text) {
    for (unsigned i = 0; i < 1000; ++i) {
        const auto state = request.Take(text);
        if (state != Request::State::Pending) return state;
        Sleep(5);
    }
    throw std::runtime_error("preview did not finish");
}

void Wait(const Event& event) {
    Check(WaitForSingleObject(event.handle, 5000) == WAIT_OBJECT_0, "waiting for slow preview");
}

void Start(Request& request, const std::shared_ptr<SlowVolume>& volume) {
    request.Start(Location::Mounted(volume, 3), volume->ResourceKey(), true, true);
    Wait(volume->reading);
}
}

int RunBrowserPreviewTest(const char* directory) {
    if (!CreateDirectoryA(directory, nullptr)) return 2;
    const std::string root(directory);
    FILE* report = fopen((root + "\\browser-preview-report.txt").c_str(), "w");
    if (!report) return 2;
    int result = 0;
    try {
        using BrowserPreview::Decode;
        const auto mac = Decode({'C','a','f',0x8e,'\r','B','\r','\n','C','\n'}, true, true, false);
        Check(mac.available && mac.encoding == L"MacRoman" && mac.content == L"Caf\u00e9\r\nB\r\nC\r\n",
              "MacRoman or classic line endings decoded incorrectly");
        const auto utf8 = Decode({0xef,0xbb,0xbf,'A',0xe6,0x97,0xa5,'\n'}, true, true, false);
        Check(utf8.available && utf8.encoding == L"UTF-8" && utf8.content == L"A\u65e5\r\n",
              "UTF-8 BOM did not take precedence");
        const auto boundary = Decode({'A',0xe6,0x97}, false, false, true);
        Check(boundary.available && boundary.truncated && boundary.content == L"A", "split UTF-8 at sample boundary");
        Check(!Decode({'A',0,'B'}, true, true, false).available && Decode({},true,true,false).available,
              "binary or empty content classification");

        const auto host = VolumeCoord::Wide(root + "\\fast.txt");
        FILE* file = _wfopen(host.c_str(), L"wb");
        Check(file != nullptr, "create Windows preview fixture");
        const auto written = fwrite("Host text\n", 1, 10, file);
        fclose(file);
        Check(written == 10, "write Windows preview fixture");

        const auto image = VolumeCoord::Wide(root + "\\preview.hfsx");
        {
            auto disk = HfsPlus::CreateImage(image.c_str(), 8 * 1024 * 1024);
            HfsPlus::Volume::Format(*disk, u"Preview", true, HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
        }
        auto mounted = MacBrowser::OpenBrowserImage(image);
        Check(bool(mounted), "open preview image in its browser worker");
        MacBrowser::Entry fileEntry;
        fileEntry.name = u"Read Me";
        memcpy(fileEntry.finder.data(), "TEXTttxt", 8);
        fileEntry.forks[0].size = 5;
        fileEntry.forks[1].size = 12;
        const char contents[] = {'C','a','f',char(0x8e),'\r'};
        const auto id = mounted->Put(2,fileEntry,[&](unsigned fork,uint64_t at,void* out,size_t count){
            if (fork) memset(out, 0, count);
            else memcpy(out, contents+at, count);
        },false);
        Request request;
        request.Start(Location::Mounted(mounted,id), mounted->ResourceKey(), true,true);
        BrowserPreview::Text text;
        Check(Await(request,text) == Request::State::Ready && text.content == L"Caf\u00e9\r\n",
              "read MacRoman data fork from a mounted HFSX volume");

        auto windowsItem=Location::Windows(host);
        const auto serial=StorageLocation::WindowsSerial(host);
        Check(serial.has_value(),"identify Windows preview fixture media");
        windowsItem.BindWindowsMediaSerial(*serial^1u);
        request.Start(windowsItem,VolumeCoord::PathKey(host),false,false);
        Check(Await(request,text)==Request::State::Stale,"preview accepted a replaced Windows volume");

        auto folder=Location::Windows(VolumeCoord::Wide(root));
        folder.BindWindowsMediaSerial(*serial^1u);
        const auto creation=FileOperations::Submit(FileOperations::CreateFolder{folder,u"Wrong medium"});
        Check(WaitForSingleObject(creation.Job()->event(),5000)==WAIT_OBJECT_0 &&
              creation.Job()->result().error==ERROR_MEDIA_CHANGED &&
              GetFileAttributesW((folder.WindowsPath()+L"\\\\Wrong medium").c_str())==INVALID_FILE_ATTRIBUTES,
              "a write used a stale Windows media identity");

        auto slow = std::make_shared<SlowVolume>();
        Start(request, slow);
        request.Start(Location::Windows(host), VolumeCoord::PathKey(host), false, false);
        Check(Await(request,text) == Request::State::Ready && text.content == L"Host text\r\n",
              "a slow preview blocked a newer selection");
        SetEvent(slow->release.handle);
        Wait(slow->returned);
        Check(!request.Pending() && text.content == L"Host text\r\n",
              "a superseded preview remained eligible to publish");
        Check(slow->received == BrowserPreview::SampleLimit, "unbounded preview read");

        auto changed = std::make_shared<SlowVolume>();
        Start(request, changed);
        VolumeCoord::Changed({changed->ResourceKey()});
        SetEvent(changed->release.handle);
        Check(Await(request,text) == Request::State::Stale, "old storage revision was displayed");

        auto swapped = std::make_shared<SlowVolume>();
        Start(request, swapped);
        swapped->replaced = true;
        SetEvent(swapped->release.handle);
        Check(Await(request,text) == Request::State::Failed, "changed media was accepted");

        auto closing = std::make_shared<SlowVolume>();
        {
            Request closingPane;
            Start(closingPane, closing);
        }
        SetEvent(closing->release.handle);
        Wait(closing->returned);
        fprintf(report, "PASS: marked text and MacRoman, HFSX and Windows data reads, bounded samples, superseded selection, revisions, media swap, and closing during a read.\n");
    } catch (const std::exception& error) {
        fprintf(report, "FAIL: %s\n", error.what());
        result = 1;
    }
    fclose(report);
    return result;
}
#endif
