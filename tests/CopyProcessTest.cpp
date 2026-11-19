// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "CopyProcess.h"
#include "VolumeSession.h"
#include "VolumeSessionProtocol.h"
#include "WorkerTransport.h"
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

    l.SetMounted(std::move(v), id);
    return l;
}
std::shared_ptr<Volume> Image(const std::wstring &path,
                              HfsPlus::Volume::FormatKind kind = HfsPlus::Volume::FormatKind::HfsPlus,
                              bool journaled = true) {
    auto source = HfsPlus::CreateImage(path.c_str(), 16 * 1024 * 1024);
    HfsPlus::Volume::Format(*source, u"Process fixture", journaled, kind);
    source.reset();
    return OpenImage(path);
}
uint32_t Put(Volume &v, uint32_t parent, const std::u16string &name, uint64_t size = 300000, BYTE bias = 0) {
    Entry e;
    e.name = name;
    e.forks[0].size = size;
    e.forks[1].size = 173;
    e.finder[0] = 42;
    return v.Put(
        parent, e,
        [bias](unsigned fork, uint64_t at, void *output, size_t n) {
            auto p = static_cast<BYTE *>(output);
            for (size_t i = 0; i < n; ++i)
                p[i] = BYTE((at + i) * 7 + fork * 13 + bias);
        },
        false);
}
void Verify(Volume &v, uint32_t id, uint64_t size = 300000, BYTE bias = 0) {
    auto e = v.Stat(id);
    Check(e.forks[0].size == size && e.forks[1].size == 173 && e.finder[0] == 42,
          "worker fork/metadata lengths");
    std::vector<BYTE> buffer(65536);
    for (unsigned fork = 0; fork < 2; ++fork)
        for (uint64_t at = 0; at < e.forks[fork].size;) {
            auto n = size_t((std::min)(uint64_t(buffer.size()), e.forks[fork].size - at));
            v.Read(id, fork, at, buffer.data(), n);
            for (size_t i = 0; i < n; ++i)
                Check(buffer[i] == BYTE((at + i) * 7 + fork * 13 + bias), "worker fork contents");
            at += n;
        }
}
// Keep the real source session, but intercept fork reads to exercise failures
// after the destination worker has already begun staging an overwrite.
class InterruptedSource : public Volume, public WorkerEndpoint {
    std::shared_ptr<Volume> source_;
    std::function<void()> interrupt_;
    unsigned reads_ = 0;
public:
    InterruptedSource(std::shared_ptr<Volume> source, std::function<void()> interrupt)
        : source_(std::move(source)), interrupt_(std::move(interrupt)) {}
    WorkerExecution Execution() const override { return ExecutionFor(*source_); }
    std::string ResourceKey() const override { return source_->ResourceKey(); }
    std::u16string Name() const override { return source_->Name(); }
    const char* Kind() const override { return source_->Kind(); }
    bool Writable() const override { return false; }
    bool CaseSensitive() const override { return source_->CaseSensitive(); }
    uint64_t FreeBytes() const override { return source_->FreeBytes(); }
    std::vector<Entry> List(uint32_t parent) override { return source_->List(parent); }
    Entry Stat(uint32_t id) override { return source_->Stat(id); }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void* out, size_t size) override {
        if (size && ++reads_ == 2) interrupt_();
        source_->Read(id, fork, offset, out, size);
    }
    uint32_t MakeDirectory(uint32_t, const std::u16string&) override { throw HfsPlus::Error("Read-only fixture"); }
    uint32_t Put(uint32_t, const Entry&, Reader, bool) override { throw HfsPlus::Error("Read-only fixture"); }
    void Rename(uint32_t, uint32_t, const std::u16string&) override { throw HfsPlus::Error("Read-only fixture"); }
    void Remove(uint32_t) override { throw HfsPlus::Error("Read-only fixture"); }
};
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
void TestWorkerTransport() {
    using namespace WorkerTransport;
    HANDLE read=nullptr,write=nullptr;
    Check(CreatePipe(&read,&write,nullptr,4096)!=FALSE,"create transport fixture pipe");
    Handle input(read),output(write);
    const ImageHandleTransfer::File file{L"fixture.dsk",HANDLE(uintptr_t(17)),true};
    SendImageFile(output.value,file);
    const HfsPlus::Attributes attributes{{u"user.fixture",{1,2,3}}};
    Storage::Job transfer;
    SendAttributes(output.value,0,attributes,transfer);
    Data packet;
    Check(Available(input.value) && Receive(input.value,packet)==CopyWorkerProtocol::File,"shared image frame missing");
    const auto decoded=DecodeImageFile(packet);
    Check(decoded.path==file.path && decoded.handle==file.handle && decoded.writable,"shared image frame changed");
    Check(Receive(input.value,packet)==CopyWorkerProtocol::AttributeSet,"shared attribute frame missing");
    AttributeSets sets;
    ReceiveAttributes(input.value,packet,sets);
    Check(sets[0] && *sets[0]==attributes && !Available(input.value),"shared attribute transport changed or left bytes");
    DWORD oversized[]={DWORD(CopyWorkerProtocol::protocol),DWORD(CopyWorkerProtocol::SessionBytes),CopyWorkerProtocol::maximumFrame+1};
    Exact(output.value,oversized,sizeof(oversized),true);
    bool rejected=false;
    try { Receive(input.value,packet); } catch (const HfsPlus::Error&) { rejected=true; }
    Check(rejected,"shared transport accepted an oversized frame");
}

void TestVolumeSessionProtocol()
{
    namespace Protocol=VolumeSessionProtocol;
    using Protocol::Command;
    using CopyWorkerProtocol::Data;
    const auto roundTrip=[](Command command,const auto& request){
        auto packet=Protocol::EncodeCommand(command,Protocol::Encode(request));
        Check(Protocol::DecodeCommand(packet,false,false)==command,"volume command changed during encoding");
        return packet;
    };
    auto list=roundTrip(Command::List,Protocol::ListRequest{2});
    Check(Protocol::DecodeList(list).parent==2,"volume List parent changed");
    auto stat=roundTrip(Command::Stat,Protocol::StatRequest{UINT32_MAX});
    Check(Protocol::DecodeStat(stat).id==UINT32_MAX,"volume Stat lost a full-width catalog ID");
    auto rename=roundTrip(Command::Rename,Protocol::RenameRequest{42,2,u"Renamed \u03a9"});
    const auto decoded=Protocol::DecodeRename(rename);
    Check(decoded.id==42&&decoded.parent==2&&decoded.name==u"Renamed \u03a9","volume Rename endpoints changed");
    auto read=roundTrip(Command::Read,Protocol::ReadRequest{42,1,UINT64_C(0x100000000),Protocol::ReadLimit});
    const auto range=Protocol::DecodeRead(read);
    Check(range.fork==1&&range.offset==UINT64_C(0x100000000)&&range.count==Protocol::ReadLimit,
          "volume Read lost its fork or wide range");

    const auto reject=[](const auto& decode,const char* message){
        bool rejected=false;
        try{decode();}catch(const HfsPlus::Error&){rejected=true;}
        Check(rejected,message);
    };
    reject([]{Data data;data.put(1000);Protocol::DecodeCommand(data,false,false);},"accepted unknown volume opcode");
    reject([]{auto data=Protocol::EncodeCommand(Command::Put,{});Protocol::DecodeCommand(data,false,false);},
           "accepted missing streamed attributes");
    reject([]{auto data=Protocol::EncodeCommand(Command::List,{});Protocol::DecodeCommand(data,false,true);},
           "accepted unexpected secondary attributes");
    reject([]{Data data;data.put(UINT64_C(0x100000000));Protocol::DecodeStat(data);},"accepted a wider catalog number");
    reject([]{Data data;data.put(2);data.put(3);Protocol::DecodeList(data);},"accepted trailing List bytes");
    reject([]{auto data=Protocol::Encode(Protocol::RenameRequest{42,2,u"Name"});data.bytes.pop_back();Protocol::DecodeRename(data);},
           "accepted truncated UTF-16 rename payload");
    reject([]{auto data=Protocol::Encode(Protocol::ReadRequest{42,2,0,1});Protocol::DecodeRead(data);},
           "accepted invalid read fork");
    reject([]{auto data=Protocol::Encode(Protocol::ReadRequest{42,0,0,Protocol::ReadLimit+1});Protocol::DecodeRead(data);},
           "accepted oversized read request");
    reject([]{auto data=Protocol::Encode(Protocol::Reply{3,"",Storage::Rollback::None});Protocol::DecodeReply(data);},
           "accepted invalid session reply");

    Entry entry;
    entry.id=42;
    entry.name=u"Both forks \u03a9";
    entry.forks[0].size=300000;
    entry.forks[1].size=71;
    auto put=Protocol::EncodePut({2,true,true,42,0},entry);
    const auto decodedPut=Protocol::DecodePut(put);
    Check(decodedPut.header.original==42&&decodedPut.header.replace&&decodedPut.entry.attributesKnown&&
          decodedPut.entry.name==entry.name&&decodedPut.entry.forks[1].size==71,"Put header round trip");
    reject([&]{auto data=Protocol::EncodePut({2,true,true,42,42},entry);Protocol::DecodePut(data);},
           "accepted conflicting Put sources");
    reject([&]{auto data=Protocol::EncodePut({2,false,false,42,0},entry);Protocol::DecodePut(data);},
           "accepted resource replacement with overwrite disabled");
    reject([&]{auto data=Protocol::EncodePut({2,true,false,0,91},entry);Protocol::DecodePut(data);},
           "accepted a Put source that disagrees with the entry ID");
    reject([&]{auto data=Protocol::EncodePut({2,true,false,0,0},entry);data.bytes.pop_back();Protocol::DecodePut(data);},
           "accepted a truncated Put metadata header");
    reject([&]{auto data=Protocol::EncodePut({2,true,false,0,0},entry);data.put(0);Protocol::DecodePut(data);},
           "accepted trailing Put payload data");
    reject([&]{auto data=Protocol::Encode(Protocol::ForkRange{1,70,2});Protocol::DecodeForkRange(data,entry);},
           "accepted a fork range beyond the declared resource fork");
    reject([&]{auto data=Protocol::Encode(Protocol::ForkRange{0,UINT64_MAX,1});Protocol::DecodeForkRange(data,entry);},
           "accepted an overflowing fork range");
    reject([]{Data data;data.bytes.resize(7);Protocol::DecodeForkReply(CopyWorkerProtocol::SessionBytes,data,8,false);},
           "accepted interrupted fork data as a complete frame");
    reject([]{Data data;data.put(0);Protocol::DecodeForkReply(CopyWorkerProtocol::Cancel,data,8,false);},
           "accepted a cancellation frame containing data");
    reject([]{Data data;Protocol::DecodeForkReply(CopyWorkerProtocol::SessionContinue,data,8,false);},
           "accepted a local continuation for an external fork");
    Data empty;
    Check(!Protocol::DecodeForkReply(CopyWorkerProtocol::Cancel,empty,8,false),"empty cancellation changed");
    auto update=Protocol::EncodeDirectoryUpdate({42,true,true},entry,entry);
    update.bytes.pop_back();
    reject([&]{Protocol::DecodeDirectoryUpdate(update);},"accepted truncated expected folder metadata");
    reject([]{Data data;data.put(42);data.put(UINT64_MAX);Protocol::DecodeCompression(data);},
           "accepted an invalid compression method");
}

void TestMetadataPolicies(const std::shared_ptr<Volume>& volume,uint32_t id,bool temporary)
{
    const auto item=Native(volume,id);
    const auto key=volume->ResourceKey();
    const auto baseline=volume->GetAttributes(id);
    Request inspect=InspectProperties{item};
    inspect.background=true;
    const auto readPlan=InspectPlanForTest(inspect,key);
    Check(readPlan.read&&!readPlan.write&&!readPlan.temporaryWorker,"Properties should use a shared read lease");

    auto desired=baseline;
    desired[u"user.metadata-policy"]={5,7,9};
    Request edit=EditAttributes{item,baseline,desired};
    edit.background=true;
    const auto writePlan=InspectPlanForTest(edit,key);
    Check(!writePlan.read&&writePlan.write&&writePlan.temporaryWorker==temporary,"metadata edit reserved the wrong route or access");
    CopyProcess::lastProcess=0;
    std::optional<Operation> editing;
    {
        VolumeCoord::Lease read({{key,VolumeCoord::Mode::Read}});
        editing=Submit(edit);
        auto reading=Submit(inspect);
        Wait(reading);
        Check(!reading.Job()->result().code&&reading.Snapshot().complete&&
              reading.Snapshot().attributes==baseline&&!editing->Job()->done(),
              "a metadata read should finish while an edit waits for the shared lease");
    }
    Wait(*editing);
    Check(!editing->Job()->result().code&&editing->Snapshot().complete&&
          bool(CopyProcess::lastProcess.load())==temporary&&volume->GetAttributes(id)==desired,
          "metadata edit did not use its prepared route");

    CopyProcess::lastProcess=0;
    auto stale=Submit(edit);
    Wait(stale);
    Check(stale.Job()->result().code&&stale.Snapshot().affected.empty()&&
          bool(CopyProcess::lastProcess.load())==temporary&&volume->GetAttributes(id)==desired,
          "stale metadata edit reached storage or bypassed its expected route");
    auto selection=item;
    selection.expectedName=u"Obsolete name";
    Request staleSelection=EditAttributes{selection,desired,baseline};
    staleSelection.background=true;
    auto rejected=Submit(staleSelection);
    Wait(rejected);
    Check(rejected.Job()->result().code&&rejected.Snapshot().affected.empty()&&volume->GetAttributes(id)==desired,
          "metadata edit rebound a stale selection");
    volume->SetAttributes(id,baseline);
}

void TestMountedForkJobs(const std::shared_ptr<Volume>& image, const std::wstring& folder) {
    Check(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE,"mounted fork fixture directory");
    const auto pid=VolumeSession::VolumeSessionPidForTest(image);
    const auto directory=image->MakeDirectory(2,u"Fork job fixtures");
    const auto id=Put(*image,directory,u"Fork payload");
    HfsPlus::Attributes attributes{{u"user.fork-metadata",{12,34,56}}};
    if(image->SupportsAttributes())image->SetAttributes(id,attributes);
    hfsdirent classic{};
    const bool isClassic=image->GetClassicProperties(id,classic);
    if(isClassic){
        classic.bkdate=1000000000;
        classic.fdflags|=6;
        image->SetClassicProperties(id,classic);
    }
    const auto original=image->Stat(id);
    CopyProcess::lastProcess=0;

    auto verifyBytes=[](Volume& volume,uint32_t file,unsigned fork,uint64_t length,unsigned bias){
        std::vector<BYTE> bytes(65536);
        for(uint64_t at=0;at<length;){
            const auto count=size_t((std::min)(uint64_t(bytes.size()),length-at));
            volume.Read(file,fork,at,bytes.data(),count);
            for(size_t n=0;n<count;++n)
                Check(bytes[n]==BYTE((at+n)*7+bias),"mounted fork payload was changed");
            at+=count;
        }
    };
    for(const auto selector:{ForkSelector::Data,ForkSelector::Resource}){
        const unsigned fork=static_cast<unsigned>(selector);
        const auto path=folder+(fork?L"\\resource.bin":L"\\data.bin");
        auto operation=Submit(ExportFileFork{Native(image,id),Location::Windows(path),selector});
        Wait(operation);
        if(operation.Job()->result().code)throw HfsPlus::Error(operation.Job()->result().message);
        auto output=ResolveWindowsFile(path);
        const auto entry=output.first->Stat(output.second);
        Check(operation.Snapshot().complete && entry.forks[0].size==original.forks[fork].size &&
                  !entry.forks[1].size,"mounted export used the wrong fork");
        verifyBytes(*output.first,output.second,0,entry.forks[0].size,fork*13);
    }

    const auto cancelledPath=folder+L"\\cancelled.bin";
    Storage::Job exportJob;
    bool exportStarted=false;
    exportJob.transport([&](const Storage::Progress& progress){
        if(progress.fileBytes){exportStarted=true;exportJob.cancel();}
    },{});
    Completion completion;
    const auto cancelledExport=ExecuteInWorker(
        ExportFileFork{Native(image,id),Location::Windows(cancelledPath),ForkSelector::Data},exportJob,completion);
    Check(exportStarted && cancelledExport.outcome==Storage::Outcome::Cancelled &&
              GetFileAttributesW(cancelledPath.c_str())==INVALID_FILE_ATTRIBUTES,
          "cancelled mounted fork export left a partial file");

    const auto replacement=folder+L"\\replacement.bin";
    {
        auto windows=OpenWindows(folder);
        Put(*windows,2,u"replacement.bin",300000,71);
    }
    auto rejected=Submit(ImportResourceFork{Location::Windows(folder),Native(image,id)});
    Wait(rejected);
    Check(rejected.Job()->result().code && image->Stat(id).forks[1].size==173,
          "resource replacement accepted a directory as fork input");
    const ImportResourceFork request{Location::Windows(replacement),Native(image,id)};
    Storage::Job importJob;
    bool stagedResource=false;
    importJob.transport([&](const Storage::Progress& progress){
        if(progress.fileBytes>original.forks[0].size){stagedResource=true;importJob.cancel();}
    },{});
    Completion importCompletion;
    const auto cancelledImport=ExecuteInWorker(request,importJob,importCompletion);
    Check(stagedResource && cancelledImport.outcome==Storage::Outcome::Cancelled &&
              !cancelledImport.partial && image->Stat(id).forks[1].size==173,
          "cancelled resource replacement modified the original file");
    verifyBytes(*image,id,0,300000,0);
    verifyBytes(*image,id,1,173,13);

    auto operation=Submit(request);
    Wait(operation);
    if(operation.Job()->result().code)throw HfsPlus::Error(operation.Job()->result().message);
    Entry updated;
    Check(operation.Snapshot().complete && image->FindChild(directory,u"Fork payload",updated) &&
              updated.forks[0].size==300000 && updated.forks[1].size==300000 &&
              updated.finder==original.finder && updated.created==original.created &&
              (!image->SupportsAttributes() || image->GetAttributes(updated.id)==attributes),
          "resource replacement failed to preserve the data fork or metadata");
    if(isClassic){
        hfsdirent properties{};
        Check(image->GetClassicProperties(updated.id,properties) && properties.bkdate==classic.bkdate,
              "classic resource replacement lost its backup date");
    }
    verifyBytes(*image,updated.id,0,300000,0);
    verifyBytes(*image,updated.id,1,300000,71);
    Check(!CopyProcess::lastProcess && VolumeSession::VolumeSessionPidForTest(image)==pid,
          "fork operations restarted the persistent worker");
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
        CopyProcess::ioGate = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        Check(CopyProcess::ioGate.load() != nullptr, "parent-exit gate");
        CopyProcess::fault = 7;
        Request request;
        request.background = true;
        auto from = Native(source, id);
        from.cut = true;
        request.Copy().sources = {from};
        request.Copy().destination = Native(target);
        auto op = Submit(request);
        DWORD end = GetTickCount() + 15000;
        while (!op.Job()->done() && op.Job()->progress().phase != "Worker test gate" &&
               LONG(end - GetTickCount()) > 0)
            Sleep(10);
        Check(!op.Job()->done() && op.Job()->progress().phase == "Worker test gate",
              "parent-exit copy did not start");
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
        {
            const auto path=Wide(root+"\\session.dsk");
            { auto seed=Image(path); Put(*seed,2,u"Mounted read",4096); }
            auto mounted=OpenBrowserImage(path);
            Entry entry;
            const auto pid=VolumeSession::VolumeSessionPidForTest(mounted);
            Check(pid && mounted->FindChild(2,u"Mounted read",entry),"shipping volume session did not mount");
            Verify(*mounted,entry.id,4096);
            const auto written=Put(*mounted,2,u"Mounted write",8192);
            Verify(*mounted,written,8192);
            Check(VolumeSession::VolumeSessionPidForTest(mounted)==pid,"shipping mount restarted during read/write");
            fprintf(report,"PASS: shipping persistent worker launches and runs read/write through the shared transport\n");
        }
        auto source = Image(Wide(root + "\\source.dsk")),
             target = Image(Wide(root + "\\target.dsk"), HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
        auto id = Put(*source, 2, u"Shipping", 4096);
        HfsPlus::Attributes attrs;
        attrs[u"user.release"] = {7, 8, 9};
        source->SetAttributes(id, attrs);
        Request request;
        request.background = true;
        request.Copy().sources = {Native(source, id)};
        request.Copy().destination = Native(target);
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
        request.Copy().sources = {Native(source, plain)};
        request.Copy().destination = Native(classic);
        Pass(Submit(request));
        Check(classic->FindChild(2, u"Plain", copied), "shipping classic output");
        Verify(*classic, copied.id, 4096);
        auto mfs = MfsImageFixture(Wide(root + "\\mfs.img"), false);
        request.Copy().destination = Native(mfs);
        Pass(Submit(request));
        Check(mfs->FindChild(2, u"Plain", copied), "shipping MFS output");
        Verify(*mfs, copied.id, 4096);
        auto execute = [&](Request mutation) {
            CopyProcess::lastProcess = 0;
            mutation.background = true;
            auto op = Submit(std::move(mutation));
            Pass(op);
            Check(!op.Snapshot().affected.empty(), "Worker mutation omitted affected storage");
        };
        execute(CreateFolder{Native(target), u"Worker folder"});
        Entry folder, renamed;
        Check(target->FindChild(2, u"Worker folder", folder) && folder.directory, "Worker folder missing");
        Entry original;
        Check(target->FindChild(2, u"Shipping", original), "Missing rename fixture");
        execute(RenameEntry{Native(target, original.id), u"Renamed by worker", Native(target, folder.id)});
        Check(target->FindChild(folder.id, u"Renamed by worker", renamed) && renamed.id == original.id,
              "Worker rename did not preserve the file");
        auto linkSource = Put(*target, folder.id, u"Worker link source", 4096);
        execute(FileOperations::CreateHardLink{Native(target, linkSource), Native(target, folder.id), u"Worker link"});
        Entry link;
        Check(target->FindChild(folder.id, u"Worker link", link), "Worker hard link missing");
        execute(DeleteEntries{{Native(target, link.id)}});
        Check(!target->FindChild(folder.id, u"Worker link", link), "Worker delete did not remove the link");

        auto staleAttributes = target->GetAttributes(renamed.id);
        auto attributes = staleAttributes;
        attributes[u"user.worker"] = HfsPlus::AttributeData::Snapshot(1024 * 1024 + 7,
            [](uint64_t offset, void* bytes, size_t count) {
                memset(bytes, offset % 2 ? 7 : 11, count);
            });
        execute(EditAttributes{Native(target, renamed.id), staleAttributes, attributes});
        Check(target->GetAttributes(renamed.id) == attributes, "Worker streamed attributes changed");
        CopyProcess::lastProcess = 0;
        auto stale = Submit(EditAttributes{Native(target, renamed.id), staleAttributes, {}});
        Wait(stale);
        Check(CopyProcess::lastProcess && stale.Job()->result().code && stale.Snapshot().affected.empty() &&
                  target->GetAttributes(renamed.id) == attributes, "Worker accepted stale attribute edit");
        execute(ChangeCompression{Native(target, renamed.id), target->Stat(renamed.id), attributes,
                                  HfsPlus::Volume::CompressionMode::Zlib});
        Check(target->Stat(renamed.id).compressed, "Worker did not apply compression");

        auto mfsOriginal = mfs->Stat(copied.id);
        auto mfsDesired = mfsOriginal;
        mfsDesired.version = 7;
        execute(EditBasicMetadata{Native(mfs, copied.id), mfsOriginal, mfsDesired});
        Check(mfs->Stat(copied.id).version == 7, "Worker lost MFS version change");
        auto classicItem = Location::Classic(root + "\\classic.dsk", "Classic:Plain");
        classic->Close();
        auto inspect = Submit(InspectProperties{classicItem});
        Wait(inspect);
        Check(!inspect.Job()->result().code && inspect.Snapshot().classicProperties, "Classic Properties fixture");
        auto before = inspect.Snapshot().properties;
        auto after = before;
        after.fdlocation.h = 25;
        classic = OpenClassic(root + "\\classic.dsk", "Classic:", true);
        execute(EditClassicProperties{classicItem, ClassicPropertyEdit{before, after}});
        Entry classicResult;
        Check(classic->FindChild(2, u"Plain", classicResult) && classicResult.finder[12] == 0 &&
                  classicResult.finder[13] == 25,
              "Worker lost classic Properties edit");

        auto partialId = Put(*target, 2, u"Partial worker delete", 4096);
        CopyProcess::lastProcess = 0;
        auto partial = Submit(DeleteEntries{{Native(target, partialId), Native(target)}});
        Wait(partial);
        Entry remaining;
        Check(CopyProcess::lastProcess && partial.Job()->result().code &&
                  partial.Job()->result().outcome == Storage::Outcome::Failed && partial.Job()->result().partial &&
                  partial.Job()->result().completed == 1 && partial.Snapshot().completed == 1 &&
                  !partial.Snapshot().affected.empty() &&
                  !target->FindChild(2, u"Partial worker delete", remaining) && target->Stat(2).directory,
              "Worker delete lost partial completion or attempted a volume root");
        fprintf(report, "PASS: shipping worker folder/rename/delete/hard links, partial delete notifications, "
                        "classic and MFS Properties, streamed attributes, stale edits and compression\n");
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
        TestWorkerTransport();
        TestVolumeSessionProtocol();
        fprintf(report,"PASS: typed persistent volume command round trips and rejected malformed packets\n");
        fprintf(report,"PASS: shared pipe framing, inherited image codec, streamed attributes and oversized-frame rejection\n");
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
        request.Copy().sources = {Native(source, folder)};
        request.Copy().destination = Native(target);
        Pass(Submit(request));
        Entry copied, child;
        Check(target->FindChild(2, u"Folder", copied) && target->FindChild(copied.id, u"Both forks", child),
              "worker directory traversal");
        Verify(*target, child.id);
        Check(target->GetAttributes(child.id) == attrs, "worker attributes");
        TestMetadataPolicies(target,child.id,true);
        {
            const auto path=Wide(root+"\\interrupted-stream.dsk");
            {
                auto image=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
                HfsPlus::Volume::Format(*image,u"Interrupted stream",true);
            }
            auto mounted=OpenBrowserImage(path);
            const auto id=Put(*mounted,2,u"Both forks");
            mounted->SetAttributes(id,attrs);
            TestMetadataPolicies(mounted,id,false);
            fprintf(report,"PASS: Properties shares a read lease, edits wait for write access, and both metadata routes reject stale edits and selections.\n");
            const auto pid=VolumeSession::VolumeSessionPidForTest(mounted);
            Check(pid!=0,"interrupted Put fixture requires a persistent worker");
            const auto original=mounted->Stat(id);
            const auto free=mounted->FreeBytes();
            bool sentPrefix=false,interrupted=false;
            try{
                mounted->Put(2,original,[&](unsigned fork,uint64_t offset,void* bytes,size_t count){
                    if(fork||offset)throw HfsPlus::Error("Injected source interruption");
                    sentPrefix=true;
                    memset(bytes,0xe4,count);
                },true);
            }catch(const HfsPlus::Error& error){
                interrupted=std::string(error.what())=="Injected source interruption";
            }
            Entry retained;
            Check(sentPrefix&&interrupted&&mounted->FindChild(2,original.name,retained)&&
                  retained.id==original.id&&mounted->FreeBytes()==free&&
                  VolumeSession::VolumeSessionPidForTest(mounted)==pid,
                  "interrupted Put lost its destination or restarted the mounted worker");
            Verify(*mounted,retained.id);
            Check(mounted->GetAttributes(retained.id)==attrs,"interrupted Put changed destination attributes");
            fprintf(report,"PASS: interrupted streamed replacement preserves both forks, attributes, free space and the mounted worker.\n");
        }
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
        {
            const auto fromPath = Wide(root + "\\persistent-source.dsk");
            const auto toPath = Wide(root + "\\persistent-target.dsk");
            { auto seed = Image(fromPath); Put(*seed,2,u"Persistent payload"); }
            {
                auto seed = Image(toPath, HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
                Put(*seed,2,u"Persistent payload",4096,93);
            }
            auto from = OpenBrowserImage(fromPath), to = OpenBrowserImage(toPath);
            Entry original, copied;
            Check(ExecutionFor(*from).Persistent() && ExecutionFor(*to).Persistent() &&
                      from->FindChild(2,u"Persistent payload",original) &&
                      to->FindChild(2,u"Persistent payload",copied),
                  "persistent copy fixture could not browse");
            const auto existing = copied;
            const auto freeBeforeFailure = to->FreeBytes();
            const auto fromPid = VolumeSession::VolumeSessionPidForTest(from);
            const auto toPid = VolumeSession::VolumeSessionPidForTest(to);
            auto verifySessions = [&] {
                Check(from->Stat(original.id).identity == original.identity &&
                          VolumeSession::VolumeSessionPidForTest(from) == fromPid &&
                          VolumeSession::VolumeSessionPidForTest(to) == toPid,
                      "cross-volume copy restarted a persistent session");
            };
            struct Gate {
                HANDLE entered = CreateEvent(nullptr, TRUE, FALSE, nullptr);
                HANDLE release = CreateEvent(nullptr, TRUE, FALSE, nullptr);
                ~Gate() { if (entered) CloseHandle(entered); if (release) CloseHandle(release); }
            };
            auto gate = std::make_shared<Gate>();
            Check(gate->entered && gate->release, "persistent cancellation gate");
            auto delayed = std::make_shared<InterruptedSource>(from, [gate] {
                SetEvent(gate->entered);
                Check(WaitForSingleObject(gate->release, 10000) == WAIT_OBJECT_0, "persistent read gate timed out");
            });
            auto cancel = Submit(OrdinaryCopy{{Native(delayed,original.id)},Native(to)});
            Conflict(cancel);
            cancel.Job()->resolve(Storage::ConflictChoice::Replace);
            const auto entered = WaitForSingleObject(gate->entered, 10000);
            cancel.Job()->cancel();
            SetEvent(gate->release);
            Wait(cancel);
            Check(entered == WAIT_OBJECT_0 && cancel.Job()->result().error == ERROR_CANCELLED,
                  "persistent overwrite did not cancel during fork transfer");
            Verify(*to,existing.id,4096,93);
            Check(to->FreeBytes() == freeBeforeFailure, "aborted overwrite leaked allocation blocks");
            verifySessions();

            auto faulty = std::make_shared<InterruptedSource>(from, [] {
                throw HfsPlus::Error("Injected persistent source read failure");
            });
            auto failed = Submit(OrdinaryCopy{{Native(faulty,original.id)},Native(to)});
            Conflict(failed);
            failed.Job()->resolve(Storage::ConflictChoice::Replace);
            Wait(failed);
            Check(failed.Job()->result().code &&
                      failed.Job()->result().message.find("Injected persistent source read failure") != std::string::npos &&
                      to->Stat(existing.id).identity == existing.identity,
                  "failed persistent overwrite did not preserve the original entry");
            Verify(*to,existing.id,4096,93);
            Check(to->FreeBytes() == freeBeforeFailure, "aborted overwrite leaked allocation blocks");
            verifySessions();

            auto selected = Native(from,original.id);
            selected.expectedIdentity = original.identity;
            Request transfer = OrdinaryCopy{{selected},Native(to)};
            transfer.background = true;
            auto copy = Submit(std::move(transfer));
            Conflict(copy);
            copy.Job()->resolve(Storage::ConflictChoice::Replace);
            Wait(copy);
            Check(!copy.Job()->result().code && copy.Snapshot().complete &&
                      to->FindChild(2,u"Persistent payload",copied),
                  "persistent cross-volume copy failed");
            verifySessions();
            Verify(*to,copied.id);

            const auto folder = from->MakeDirectory(2,u"Persistent folder");
            Put(*from,folder,u"Nested forks",4096);
            const HfsPlus::Attributes folderAttributes{{u"user.folder",{5,6,7}}};
            from->SetAttributes(folder,folderAttributes);
            const auto originalFolder = from->Stat(folder);
            auto recursive = Submit(OrdinaryCopy{{Native(from,folder)},Native(to)});
            Wait(recursive);
            Entry copiedFolder, nested;
            Check(!recursive.Job()->result().code && recursive.Snapshot().complete &&
                      to->FindChild(2,u"Persistent folder",copiedFolder) &&
                      to->FindChild(copiedFolder.id,u"Nested forks",nested) &&
                      copiedFolder.modified == originalFolder.modified &&
                      to->GetAttributes(copiedFolder.id) == folderAttributes,
                  "persistent recursive copy did not preserve folder metadata");
            Verify(*to,nested.id,4096);
            verifySessions();

            for (const auto& image : {from,to}) {
                const auto pid = VolumeSession::VolumeSessionPidForTest(image);
                const auto sourceFolder = image->MakeDirectory(2,u"Same-image source");
                const auto targetFolder = image->MakeDirectory(2,u"Same-image target");
                const auto descendant = image->MakeDirectory(sourceFolder,u"Descendant");
                const auto payload = Put(*image,sourceFolder,u"Both forks");
                const HfsPlus::Attributes attributes{{u"user.recursive",{1,2,3}}};
                image->SetAttributes(sourceFolder,attributes);
                Request same = OrdinaryCopy{{Native(image,sourceFolder)},Native(image,targetFolder)};
                same.background = true;
                auto recursiveSame = Submit(same);
                Wait(recursiveSame);
                Entry copiedTree, copiedFile, copiedDescendant;
                Check(!recursiveSame.Job()->result().code && recursiveSame.Snapshot().complete &&
                          image->FindChild(targetFolder,u"Same-image source",copiedTree) &&
                          image->FindChild(copiedTree.id,u"Both forks",copiedFile) &&
                          image->FindChild(copiedTree.id,u"Descendant",copiedDescendant) &&
                          image->GetAttributes(copiedTree.id) == attributes,
                      "same-image recursive copy failed");
                Verify(*image,copiedFile.id);

                for (const auto invalidParent : {sourceFolder,descendant}) {
                    auto refused = Submit(OrdinaryCopy{{Native(image,sourceFolder)},Native(image,invalidParent)});
                    Wait(refused);
                    Entry unexpected;
                    Check(refused.Job()->result().code &&
                              !image->FindChild(invalidParent,u"Same-image source",unexpected),
                          "same-image copy accepted a folder as its own ancestor");
                }

                const auto oldFile = Put(*image,targetFolder,u"Both forks",4096,71);
                const auto freeBefore = image->FreeBytes();
                Storage::Job interrupted;
                bool staged = false;
                interrupted.transport([&](const Storage::Progress& progress) {
                    // The data fork has been staged before the resource fork is read.
                    if (progress.fileBytes > 300000 && !progress.files) {
                        staged = true;
                        interrupted.cancel();
                    }
                }, [](const std::string&) { return Storage::ConflictChoice::Replace; });
                Completion interruptedCompletion;
                const auto overwrite = OrdinaryCopy{{Native(image,payload)},Native(image,targetFolder)};
                const auto aborted = ExecuteInWorker(overwrite,interrupted,interruptedCompletion);
                Check(staged && aborted.outcome == Storage::Outcome::Cancelled &&
                          !aborted.partial && !interruptedCompletion.complete,
                      "same-image overwrite did not cancel after staging the data fork");
                Verify(*image,oldFile,4096,71);
                Verify(*image,payload);
                Check(image->FreeBytes() == freeBefore, "same-image rollback leaked space");
                auto retry = Submit(overwrite);
                Conflict(retry);
                retry.Job()->resolve(Storage::ConflictChoice::Replace);
                Wait(retry);
                Entry replacement;
                Check(!retry.Job()->result().code && image->FindChild(targetFolder,u"Both forks",replacement) &&
                          VolumeSession::VolumeSessionPidForTest(image) == pid,
                      "same-image replacement restarted its worker or failed");
                Verify(*image,replacement.id);
            }
            fprintf(report,"PASS: HFS+ and HFSX same-image recursive copy, self-copy refusal and overwrite cancellation without worker restart\n");
            const auto hostFolder=Wide(root+"\\persistent-windows");
            Check(CreateDirectoryW(hostFolder.c_str(),nullptr)!=FALSE,"persistent Windows copy folder");
            const auto hostTarget=Location::Windows(hostFolder);
            const auto hostFile=Location::Windows(hostFolder+L"\\Persistent payload");
            {
                auto host=OpenWindows(hostFolder);
                const auto execution=ExecutionFor(*host);
                Check(execution.windowsPaths&&!execution.Persistent()&&!execution.stableCatalogIds,
                      "Windows endpoints advertised a catalog or a persistent worker");
                Put(*host,2,u"Persistent payload",4096,71);
            }
            const auto uploadFolder=to->MakeDirectory(2,u"Windows imports");
            const auto uploadTarget=Native(to,uploadFolder);
            const auto uploadOld=Put(*to,uploadFolder,u"Persistent payload",4096,89);
            auto cancelOverwrite=[](const OrdinaryCopy& copy){
                Storage::Job job;
                bool staged=false;
                job.transport([&](const Storage::Progress& progress){
                    if(progress.fileBytes>300000 && !progress.files){
                        staged=true;
                        job.cancel();
                    }
                },[](const std::string&){return Storage::ConflictChoice::Replace;});
                Completion completion;
                auto result=ExecuteInWorker(copy,job,completion);
                Check(staged && result.outcome==Storage::Outcome::Cancelled && !result.partial,
                      "Windows transfer did not cancel during overwrite staging");
            };
            const auto download=OrdinaryCopy{{Native(from,original.id)},hostTarget};
            {
                Request planned=download;planned.background=true;
                const auto mountedExecution=ExecutionFor(*from);
                const auto localExecution=ExecutionFor(*source);
                Check(mountedExecution.Persistent()&&mountedExecution.stableCatalogIds&&
                      !mountedExecution.classicConversion&&!localExecution.Persistent()&&localExecution.stableCatalogIds,
                      "HFS+ catalog identity was tied to worker placement");
                auto exportPlan=InspectPlanForTest(planned,from->ResourceKey());
                Check(exportPlan.read && !exportPlan.write && !exportPlan.temporaryWorker && !exportPlan.sameMount,
                      "background mounted export plan did not share source reads");
                planned.Copy().sources.front().cut=true;
                auto cutPlan=InspectPlanForTest(planned,from->ResourceKey());
                Check(cutPlan.write && !cutPlan.read && !cutPlan.temporaryWorker,
                      "mounted cut export plan failed to reserve the source for mutation");
                const auto same=InspectPlanForTest(OrdinaryCopy{{Native(from,original.id)},Native(from)},from->ResourceKey());
                Check(same.sameMount && same.write && !same.temporaryWorker,
                      "same-image plan did not choose its mounted worker and exclusive access");
                Request handoff=OrdinaryCopy{{Native(source,file)},hostTarget};
                const auto temporary=InspectPlanForTest(handoff,source->ResourceKey());
                const auto childPlan=InspectPlanForTest(handoff,source->ResourceKey(),true);
                Check(temporary.temporaryWorker && temporary.write && !temporary.read &&
                          !childPlan.temporaryWorker && childPlan.read && !childPlan.write,
                      "temporary worker planning did not reserve the handoff or prevent nested routing");
                const auto mountedFile=Native(from,original.id);
                const auto mountedRoot=Native(from);
                const auto otherFile=Native(source,file);
                const auto key=from->ResourceKey();
                const auto folderKey=VolumeCoord::PathKey(hostFolder);
                const auto oldPath=hostFolder+L"\\old.txt";
                const auto newPath=hostFolder+L"\\new.txt";
                const Request rename=RenameEntry{Location::Windows(oldPath),u"new.txt"};
                for(const auto& path:{oldPath,newPath,hostFolder+L"\\%old.txt",hostFolder+L"\\%new.txt"}){
                    const auto policy=InspectPlanForTest(rename,VolumeCoord::PathKey(path));
                    const auto worker=InspectPlanForTest(rename,VolumeCoord::PathKey(path),true);
                    Check(policy.temporaryWorker&&policy.write&&!policy.read&&
                          !worker.temporaryWorker&&worker.write,
                          "Windows rename did not reserve both names and their companions");
                }
                const auto mountedRename=InspectPlanForTest(RenameEntry{mountedFile,u"New name",mountedRoot},key);
                Check(mountedRename.write&&!mountedRename.temporaryWorker,
                      "mounted rename did not keep its persistent worker and write lease");
                const Request deletion=DeleteEntries{{mountedFile,otherFile,Location::Windows(oldPath)}};
                for(const auto& resource:{key,source->ResourceKey(),VolumeCoord::PathKey(oldPath),
                                          VolumeCoord::PathKey(hostFolder+L"\\%old.txt")}){
                    const auto policy=InspectPlanForTest(deletion,resource);
                    Check(policy.temporaryWorker&&policy.write&&!policy.read,
                          "mixed delete did not cover every selection and companion");
                }
                const auto mountedDelete=InspectPlanForTest(DeleteEntries{{mountedFile}},key);
                Check(mountedDelete.write&&!mountedDelete.temporaryWorker,
                      "mounted delete selected a temporary worker");
                const Request folderRequest=CreateFolder{hostTarget,u"subfolder"};
                const auto child=InspectPlanForTest(folderRequest,VolumeCoord::PathKey(hostFolder+L"\\subfolder"));
                const auto parent=InspectPlanForTest(folderRequest,folderKey);
                const auto companion=InspectPlanForTest(folderRequest,VolumeCoord::PathKey(hostFolder+L"\\%subfolder"));
                const auto mountedFolder=InspectPlanForTest(CreateFolder{mountedRoot,u"subfolder"},key);
                Check(child.temporaryWorker&&child.write&&!parent.write&&!companion.write&&
                      mountedFolder.write&&!mountedFolder.temporaryWorker,
                      "folder creation did not reserve only the Windows child or the mounted volume");
                const auto link=InspectPlanForTest(FileOperations::CreateHardLink{mountedFile,mountedRoot,u"Link"},key);
                const auto unmountedLink=InspectPlanForTest(FileOperations::CreateHardLink{otherFile,Native(source),u"Link"},
                                                           source->ResourceKey());
                Check(link.write&&!link.read&&!link.temporaryWorker&&unmountedLink.write&&unmountedLink.temporaryWorker,
                      "hard link did not reserve its source for mutation or choose the expected worker");
                fprintf(report,"PASS: typed rename, delete, folder and hard-link plans choose the expected worker and leases\n");
                Request fork=ExportFileFork{Native(from,original.id),
                    Location::Windows(hostFolder+L"\\plan-fork.bin"),ForkSelector::Resource};
                fork.background=true;
                const auto forkPlan=InspectPlanForTest(fork,from->ResourceKey());
                Check(forkPlan.read && !forkPlan.write && !forkPlan.temporaryWorker,
                      "mounted fork export plan did not share source reads");
                fprintf(report,"PASS: one execution plan pairs mounted export, cut, same-image copy and worker handoff with expected leases\n");
            }
            {
                const auto mixedFolder=from->MakeDirectory(2,u"Mixed sources");
                const auto within=Put(*from,2,u"Already mounted",1024,12);
                const auto remote=Put(*to,2,u"Other image",2048,23);
                const auto oldRemote=Put(*from,mixedFolder,u"Other image",512,79);
                Put(*from,mixedFolder,u"Windows file",512,80);
                {
                    auto host=OpenWindows(hostFolder);
                    Put(*host,2,u"Windows file",4096,34);
                }
                auto cut=Native(from,within);
                cut.cut=true;
                Request mixed=OrdinaryCopy{{cut,Native(to,remote),Location::Windows(hostFolder+L"\\Windows file")},
                    Native(from,mixedFolder)};
                mixed.background=true;
                auto plan=InspectPlanForTest(mixed,from->ResourceKey());
                Check(plan.workerSources==1&&plan.streamedSources==2&&plan.write&&!plan.temporaryWorker,
                      "mixed plan did not assign each source to the destination pair");
                const auto fromPid=VolumeSession::VolumeSessionPidForTest(from);
                const auto toPid=VolumeSession::VolumeSessionPidForTest(to);
                CopyProcess::lastProcess=0;
                auto stopped=Submit(mixed);
                Conflict(stopped);
                auto progress=stopped.Job()->progress();
                const auto total=1024+2048+4096+3*173;
                Check(progress.totalFiles==3&&progress.files==1&&progress.totalBytes==total&&
                      progress.processed==1024+173&&stopped.Job()->conflict()=="Other image",
                      "mixed progress did not include the whole ordered selection");
                stopped.Job()->resolve(Storage::ConflictChoice::Cancel);
                Wait(stopped);
                const auto cancelled=stopped.Job()->result();
                Check(cancelled.outcome==Storage::Outcome::Cancelled&&cancelled.completed==1&&cancelled.partial&&
                      !stopped.Snapshot().complete,"mixed cancellation lost the partial completion");
                Verify(*from,within,1024,12);
                Verify(*from,oldRemote,512,79);

                auto retry=Submit(mixed);
                Conflict(retry);
                Check(retry.Job()->conflict()=="Already mounted","mixed retry changed source ordering");
                retry.Job()->resolve(Storage::ConflictChoice::ReplaceAll);
                Wait(retry);
                const auto result=retry.Job()->result();
                const auto finalProgress=retry.Job()->progress();
                Check(!result.code,result.message.c_str());
                Check(result.moved&&retry.Snapshot().complete&&result.completed==3,
                      "mixed Cut did not report all copied files and completed source cleanup");
                Check(finalProgress.files==3&&finalProgress.processed==total,
                      "mixed Cut cleanup reset the combined progress");
                Check(!CopyProcess::lastProcess&&VolumeSession::VolumeSessionPidForTest(from)==fromPid&&
                      VolumeSession::VolumeSessionPidForTest(to)==toPid,
                      "mixed replacement restarted a mounted worker or launched a temporary worker");
                Entry copied;
                Check(!from->FindChild(2,u"Already mounted",copied),"completed mixed Cut retained the moved source");
                Check(from->FindChild(mixedFolder,u"Already mounted",copied),"same-image result missing");
                Verify(*from,copied.id,1024,12);
                Check(from->FindChild(mixedFolder,u"Other image",copied),"other image result missing");
                Verify(*from,copied.id,2048,23);
                Check(from->FindChild(mixedFolder,u"Windows file",copied),"Windows result missing");
                Verify(*from,copied.id,4096,34);
                Verify(*to,remote,2048,23);
                fprintf(report,"PASS: mixed same-image, other-image, and Windows sources share progress, Replace All, and partial Cut cancellation without a worker handoff.\n");
            }
            cancelOverwrite(download);
            {
                auto host=ResolveWindowsFile(hostFile.WindowsPath());
                Verify(*host.first,host.second,4096,71);
            }
            CopyProcess::lastProcess=0;
            auto exportOp=Submit(download);
            Conflict(exportOp); exportOp.Job()->resolve(Storage::ConflictChoice::Replace); Wait(exportOp);
            Check(!exportOp.Job()->result().code && exportOp.Snapshot().complete && !CopyProcess::lastProcess,
                  "Windows export used handoff or failed");
            {
                auto host=ResolveWindowsFile(hostFile.WindowsPath());
                Verify(*host.first,host.second);
            }
            const auto upload=OrdinaryCopy{{hostFile},uploadTarget};
            cancelOverwrite(upload);
            Verify(*to,uploadOld,4096,89);
            auto importOp=Submit(upload);
            Conflict(importOp); importOp.Job()->resolve(Storage::ConflictChoice::Replace); Wait(importOp);
            Entry uploaded;
            Check(!importOp.Job()->result().code && importOp.Snapshot().complete && !CopyProcess::lastProcess &&
                      to->FindChild(uploadFolder,u"Persistent payload",uploaded),
                  "Windows import used handoff or failed");
            Verify(*to,uploaded.id);
            verifySessions();
            fprintf(report,"PASS: Windows both-fork transfer and mid-overwrite cancellation in both directions keep persistent image workers mounted\n");

            TestMountedForkJobs(from,Wide(root+"\\fork-hfsplus"));
            TestMountedForkJobs(to,Wide(root+"\\fork-hfsx"));

            auto stablePid=VolumeSession::VolumeSessionPidForTest(to);
            HfsPlus::Attributes edited{{u"user.session",{1,2,3,4}}};
            auto attrs=Submit(EditAttributes{Native(to,copied.id),{},edited});
            Wait(attrs);
            Check(!attrs.Job()->result().code&&to->GetAttributes(copied.id)==edited,
                  "persistent metadata write did not update the mounted volume");
            const auto compressible = Put(*to,2,u"Compression fixture",4096);
            auto compression=Submit(ChangeCompression{Native(to,compressible),to->Stat(compressible),{},
                HfsPlus::Volume::CompressionMode::Zlib});
            Wait(compression);
            Check(!compression.Job()->result().code&&to->Stat(compressible).compressed&&
                      VolumeSession::VolumeSessionPidForTest(to)==stablePid,
                  "metadata writes restarted the volume session");
            fprintf(report,"PASS: persistent HFS+/HFSX sessions stay mounted through copy, cancellation, and aborted overwrite\n");
        }
        {
            const auto path = root + "\\classic-session.dsk";
            {
                auto seed = ClassicImage(path);
                Put(*seed,2,u"Crash sentinel",4096);
            }
            auto classic = OpenBrowserImage(Wide(path));
            auto alias = OpenBrowserImage(Wide(path));
            Check(classic && classic == alias && ExecutionFor(*classic).Persistent() &&
                      ExecutionFor(*classic).classicConversion && !ExecutionFor(*classic).stableCatalogIds && classic->Writable() &&
                      classic->Name() == u"Classic" && !classic->SupportsAttributes(),
                  "classic image did not share a writable persistent mount");
            Entry sentinel;
            {
                auto pid = VolumeSession::VolumeSessionPidForTest(classic);
                Check(alias->FindChild(2,u"Crash sentinel",sentinel),"classic persistent root listing");
                Verify(*alias,sentinel.id,4096);

                const auto sourceFolder=classic->MakeDirectory(2,u"Copy source");
                const auto targetFolder=classic->MakeDirectory(2,u"Copy target");
                const auto sourceFile=Put(*classic,sourceFolder,u"Payload");
                const auto oldFile=Put(*classic,targetFolder,u"Payload",4096,71);
                auto edit=Submit(EditClassicProperties{Native(classic,sourceFile),FinderLabel{3}});
                Wait(edit);
                hfsdirent properties{};
                Check(!edit.Job()->result().code && alias->GetClassicProperties(sourceFile,properties) &&
                          (properties.fdflags&14)==6,
                      "classic Finder label edit did not reach the shared worker");

                const auto freeBefore=classic->FreeBytes();
                Storage::Job job;
                bool staged=false;
                job.transport([&](const Storage::Progress& progress) {
                    if (progress.fileBytes>300000 && !progress.files) {
                        staged=true;
                        job.cancel();
                    }
                }, [](const std::string&) { return Storage::ConflictChoice::Replace; });
                Completion completion;
                const auto overwrite=OrdinaryCopy{{Native(classic,sourceFile)},Native(alias,targetFolder)};
                const auto aborted=ExecuteInWorker(overwrite,job,completion);
                Check(staged && aborted.outcome==Storage::Outcome::Cancelled &&
                          aborted.rollback==Storage::Rollback::Restored && !aborted.partial,
                      "classic overwrite did not roll back on cancellation");
                Verify(*classic,oldFile,4096,71);
                Check(classic->FreeBytes()==freeBefore,"classic cancelled overwrite leaked space");
                auto rename=Submit(RenameEntry{Native(classic,sourceFile),u"Renamed payload"});
                Wait(rename);
                Entry renamed;
                Check(!rename.Job()->result().code && alias->FindChild(sourceFolder,u"Renamed payload",renamed) &&
                          VolumeSession::VolumeSessionPidForTest(classic)==pid,
                      "classic rename did not refresh the shared mount");
                Verify(*alias,renamed.id);

                const auto exportDir=Wide(root + "\\classic-session-export");
                Check(CreateDirectoryW(exportDir.c_str(),nullptr)!=FALSE,"classic export fixture folder");
                Entry lines;
                lines.name=u"Text lines";
                lines.forks[0].size=4;
                const auto textId=classic->Put(sourceFolder,lines,[](unsigned, uint64_t at, void* output, size_t size){
                    memcpy(output,"a\rb\r"+at,size);
                },false);
                for (const char mode : {'m','5','b','a','r','R','t'}) {
                    const auto folder=exportDir+L"\\"+std::wstring(1,wchar_t(mode));
                    // Windows folder names are case-insensitive.
                    const auto output=mode=='R'?folder+L"-resource":folder;
                    Check(CreateDirectoryW(output.c_str(),nullptr)!=FALSE,"classic export mode fixture folder");
                    LegacyConversion conversion{Native(classic,mode=='t'?textId:renamed.id),Location::Windows(output)};
                    conversion.options.output=mode;
                    conversion.target=ConversionTarget::Folder;
                    CopyProcess::lastProcess=0;
                    auto exported=Submit(conversion);
                    Wait(exported);
                    if(exported.Job()->result().code)throw HfsPlus::Error(exported.Job()->result().message);
                    Check(exported.Snapshot().complete && !CopyProcess::lastProcess &&
                              VolumeSession::VolumeSessionPidForTest(classic)==pid,
                          "classic conversion export replaced its mounted worker");
                    WIN32_FIND_DATAW file{};
                    const auto mask=mode=='t'?L"\\Text lines.txt":L"\\Renamed payload*";
                    auto search=FindFirstFileW((output+mask).c_str(),&file);
                    Check(search!=INVALID_HANDLE_VALUE,"classic conversion did not produce a Windows file");
                    FindClose(search);
                    if(mode=='r' || mode=='R'){
                        const DWORD expected=mode=='r'?300000:173;
                        Check(file.nFileSizeLow==expected,"classic raw export wrote the wrong fork length");
                    }
                    const auto exportedFile=Location::Windows(output+L"\\"+file.cFileName);
                    const auto importFolder=classic->MakeDirectory(targetFolder,u"Import "+std::u16string(1,char16_t(mode))+(mode=='R'?u" resource":u""));
                    LegacyConversion import{exportedFile,Native(classic,importFolder)};
                    import.options.input=mode;
                    import.target=ConversionTarget::Folder;
                    auto imported=Submit(import);
                    Wait(imported);
                    if(imported.Job()->result().code)throw HfsPlus::Error(imported.Job()->result().message);
                    auto entries=alias->List(importFolder);
                    Check(imported.Snapshot().complete && entries.size()==1 && !CopyProcess::lastProcess &&
                              VolumeSession::VolumeSessionPidForTest(classic)==pid,
                          "classic conversion import replaced its mounted worker");
                    if(mode=='r' || mode=='R'){
                        const unsigned fork=mode=='R'?1:0;
                        const auto expected=fork?173u:300000u;
                        Check(entries[0].forks[fork].size==expected && !entries[0].forks[1-fork].size,
                              "classic raw import wrote to the wrong fork");
                        BYTE sample[16];
                        alias->Read(entries[0].id,fork,0,sample,sizeof(sample));
                        for(unsigned i=0;i<sizeof(sample);++i)
                            Check(sample[i]==BYTE(i*7+fork*13),"classic raw fork bytes changed");
                    }else if(mode=='t'){
                        char text[4];
                        Check(entries[0].forks[0].size==sizeof(text) && !entries[0].forks[1].size,
                              "classic text round trip produced the wrong fork lengths");
                        alias->Read(entries[0].id,0,0,text,sizeof(text));
                        Check(!memcmp(text,"a\rb\r",sizeof(text)),"classic text round trip changed line endings");
                    }else Verify(*alias,entries[0].id);
                }
                struct RestoreMappings {
                    TypeMappings::Snapshot previous=TypeMappings::Capture();
                    ~RestoreMappings(){TypeMappings::Apply(previous);}
                } restoreMappings;
                TypeMappings::Snapshot mappings;
                mappings.hfsToWindows={{"TEXTTEST","hfvx"}};
                mappings.windowsToHfs={{"HFVX","TEXTTESTx"}};
                TypeMappings::Apply(mappings);
                lines.name=u"Mapped raw";
                memcpy(lines.finder.data(),"TEXTTEST",8);
                const auto mappedId=classic->Put(sourceFolder,lines,[](unsigned, uint64_t at, void* output, size_t size){
                    memcpy(output,"data"+at,size);
                },false);
                const auto exportRoot=Location::Windows(exportDir);
                LegacyConversion mappedExport{Native(classic,mappedId),exportRoot};
                mappedExport.options.output='r';
                Request converted=mappedExport;
                converted.background=true;
                const auto sharedConversion=InspectPlanForTest(converted,classic->ResourceKey());
                Check(sharedConversion.read&&!sharedConversion.write&&!sharedConversion.temporaryWorker,
                      "converted mounted export did not keep a shared read lease");
                converted.Legacy().move=true;
                const auto cutConversion=InspectPlanForTest(converted,classic->ResourceKey());
                Check(cutConversion.write&&!cutConversion.read&&!cutConversion.temporaryWorker,
                      "converted Cut did not retain its source write lease");
                mappedExport.target=ConversionTarget::Folder;
                auto mappingExport=Submit(mappedExport);Wait(mappingExport);
                const auto mappedPath=Location::Windows(exportDir+L"\\Mapped raw.hfvx");
                Check(!mappingExport.Job()->result().code &&
                          GetFileAttributesW(mappedPath.WindowsPath().c_str())!=INVALID_FILE_ATTRIBUTES,
                      "mounted raw export did not apply the mapped extension");
                LegacyConversion mappedImport{mappedPath,Native(classic,targetFolder)};
                mappedImport.options.input='r';
                mappedImport.target=ConversionTarget::Folder;
                auto mappingImport=Submit(mappedImport);Wait(mappingImport);
                Entry mapped;
                Check(!mappingImport.Job()->result().code && classic->FindChild(targetFolder,u"Mapped raw",mapped) &&
                          !memcmp(mapped.finder.data(),"TEXTTEST",8) && !CopyProcess::lastProcess &&
                          VolumeSession::VolumeSessionPidForTest(classic)==pid,
                      "mounted raw import did not apply the type mapping and strip policy");
            }
            TestMountedForkJobs(classic,Wide(root+"\\fork-classic"));
            fprintf(report,"PASS: mounted HFS, HFS+ and HFSX fork selectors and cancellation preserve original forks, Finder metadata and worker PID\n");
            WorkerEndpoint::Find(*classic)->SuspendWorker();
            std::weak_ptr<Volume> previousMount=classic;
            alias.reset();
            classic.reset();
            Until([&]{return previousMount.expired();});
            {
                // A Ctrl-Z in the reserved tail of the MDB is ordinary binary
                // data and must not stop the format probe prematurely.
                CFile raw(path.c_str(),CFile::modeReadWrite | CFile::shareExclusive);
                raw.Seek(1024+400,CFile::begin);
                const BYTE controlZ=0x1a;
                raw.Write(&controlZ,1);
                raw.Flush();
            }
            classic=OpenBrowserImage(Wide(path));
            Check(bool(classic),"classic image detection failed after a clean unmount");
            Check(classic->Writable(),"clean classic image did not reopen writable");

            const auto beforeCrash=VolumeSession::VolumeSessionPidForTest(classic);
            VolumeSession::CrashVolumeSessionDuringPutForTest(classic);
            bool lost=false;
            try { Put(*classic,2,u"Interrupted write"); }
            catch (const HfsPlus::Error&) { lost=true; }
            Check(lost && WorkerTransport::lastExitCode==80 && !classic->Writable(),
                  "classic worker did not fail at the injected point or the image remained writable");
            classic->Reload();
            Check(!classic->Writable() && VolumeSession::VolumeSessionPidForTest(classic)!=beforeCrash &&
                      classic->FindChild(2,u"Crash sentinel",sentinel),
                  "classic worker did not recover its image read-only");
            Verify(*classic,sentinel.id,4096);
            classic.reset();
            classic=OpenBrowserImage(Wide(path));
            Check(classic && !classic->Writable() && classic->FindChild(2,u"Crash sentinel",sentinel),
                  "fresh classic mount ignored the on-disk dirty flag");
            Verify(*classic,sentinel.id,4096);
            bool refused=false;
            try { Put(*classic,2,u"Disallowed recovery write",4096); }
            catch (const HfsPlus::Error&) { refused=true; }
            Entry unexpected;
            Check(refused && !classic->FindChild(2,u"Disallowed recovery write",unexpected),
                  "read-only recovery accepted a new write");
            fprintf(report,"PASS: classic HFS shared mount, Finder edit, rename refresh, conversions, overwrite rollback and durable read-only crash recovery\n");
        }
        {
            const auto inputPath = Wide(root + "\\launch-source.dsk");
            const auto outputPath = Wide(root + "\\launch-target.dsk");
            {
                auto input = Image(inputPath, HfsPlus::Volume::FormatKind::HfsPlus, false);
                auto output = Image(outputPath, HfsPlus::Volume::FormatKind::HfsPlus, false);
                const auto from = Put(*input, 2, u"Launch source", 4096);
                const auto retained = Put(*output, 2, u"Existing file", 4096);
                const Request copy = OrdinaryCopy{{Native(input, from)}, Native(output)};
                CopyProcess::lastProcess = 0;
                CopyProcess::fault = CopyProcess::failWorkerLaunch;
                auto failed = Submit(copy);
                Wait(failed);
                CopyProcess::fault = 0;
                const auto result = failed.Job()->result();
                Check(result.code && result.message.find("Cannot start copy worker") != std::string::npos &&
                          !failed.Snapshot().complete && !CopyProcess::lastProcess,
                      "Launch failure did not reach CreateProcess after suspension");
                Entry file;
                Check(input->Writable() && output->Writable() &&
                          input->FindChild(2, u"Launch source", file) && file.id == from &&
                          output->FindChild(2, u"Existing file", file) && file.id == retained &&
                          !output->FindChild(2, u"Launch source", file),
                      "Browser mounts were not restored unchanged after failed launch");
                Verify(*input, from, 4096);
                Verify(*output, retained, 4096);
                // Non-journaled images must retain write access when no worker launched.
                Put(*output, 2, u"Browser write after failed launch", 4096);
            }
            auto reopenedInput = OpenImage(inputPath);
            auto reopenedOutput = OpenImage(outputPath);
            Entry file;
            Check(reopenedInput->Writable() && reopenedOutput->Writable() &&
                      reopenedInput->FindChild(2, u"Launch source", file) &&
                      reopenedOutput->FindChild(2, u"Browser write after failed launch", file),
                  "Browser could not reopen images after failed worker launch");
            fprintf(report, "PASS: real launch failure after suspension restores browser mounts and "
                            "non-journaled write access; both images reopen without leaked handles\n");
        }
        for(const bool dc42:{false,true}) {
            const auto path=Wide(root+(dc42?"\\persistent-mfs.dc42":"\\persistent-mfs.img"));
            { auto seed=MfsImageFixture(path,dc42); Put(*seed,2,u"Mounted payload",4096,71); }
            auto volume=OpenBrowserImage(path);
            Check(volume && ExecutionFor(*volume).Persistent() && volume->Writable() &&
                      !volume->Supports().createFolders && volume->SupportsBasicMetadata(),
                  "MFS writable persistent mount capabilities");
            const auto pid=VolumeSession::VolumeSessionPidForTest(volume);
            {
                auto shared=OpenBrowserImage(path);
                Entry original;
                Check(shared==volume && pid && shared->FindChild(2,u"Mounted payload",original),
                      "browser opens did not share an MFS worker");
                const auto host=Wide(root+(dc42?"\\mfs-dc42-source":"\\mfs-raw-source"));
                Check(CreateDirectoryW(host.c_str(),nullptr)!=FALSE,"MFS copy source folder");
                { auto source=OpenWindows(host); Put(*source,2,u"Mounted payload",8192); }
                const OrdinaryCopy copy{{Location::Windows(host+L"\\Mounted payload")},Native(volume)};
                Storage::Job job;
                bool staged=false;
                job.transport([&](const Storage::Progress& progress){
                    if(progress.fileBytes>8192){staged=true;job.cancel();}
                },[](const std::string&){return Storage::ConflictChoice::Replace;});
                Completion completion;
                const auto aborted=ExecuteInWorker(copy,job,completion);
                Check(staged && aborted.outcome==Storage::Outcome::Cancelled && !aborted.partial,
                      "mounted MFS overwrite was not cancelled");
                Verify(*shared,original.id,4096,71);

                auto replaced=Submit(copy);
                Conflict(replaced);replaced.Job()->resolve(Storage::ConflictChoice::Replace);Wait(replaced);
                if(replaced.Job()->result().code)throw HfsPlus::Error(replaced.Job()->result().message);
                Entry updated;
                Check(replaced.Snapshot().complete && shared->FindChild(2,u"Mounted payload",updated),
                      "mounted MFS replacement did not refresh the shared mount");
                Verify(*shared,updated.id,8192);
                auto desired=updated;
                desired.name=u"Renamed payload";desired.version=7;
                auto edited=Submit(EditBasicMetadata{Native(volume,updated.id),updated,desired});
                Wait(edited);
                Check(!edited.Job()->result().code && shared->FindTransferChild(2,desired,updated) &&
                          updated.version==7 && VolumeSession::VolumeSessionPidForTest(shared)==pid,
                      "mounted MFS metadata or version lookup restarted the worker");
            }
            WorkerEndpoint::Find(*volume)->SuspendWorker();
            std::weak_ptr<Volume> previous=volume;
            volume.reset();
            Until([&]{return previous.expired();});
            volume=OpenBrowserImage(path);
            Check(volume && volume->Writable(),"clean MFS image did not reopen writable");
            const auto beforeCrash=VolumeSession::VolumeSessionPidForTest(volume);
            VolumeSession::CrashVolumeSessionDuringPutForTest(volume);
            bool failed=false;
            try{Put(*volume,2,u"Interrupted write",4096);}
            catch(const HfsPlus::Error&){failed=true;}
            Check(failed && WorkerTransport::lastExitCode==81 && !volume->Writable(),
                  "MFS worker did not stop after a flushed sector write");
            volume->Reload();
            Check(!volume->Writable() && VolumeSession::VolumeSessionPidForTest(volume)!=beforeCrash,
                  "MFS crash refresh did not restore read-only browsing");
            volume.reset();
            volume=OpenBrowserImage(path);
            Entry retained;
            Check(volume && !volume->Writable() && volume->FindChild(2,u"Renamed payload",retained),
                  "fresh MFS mount did not enforce the durable write lock");
            Verify(*volume,retained.id,8192);
            bool denied=false;
            try{Put(*volume,2,u"Rejected",4096);}
            catch(const HfsPlus::Error&){denied=true;}
            Check(denied,"MFS crash recovery allowed a new write");
            fprintf(report,"PASS: %s MFS shared read/write worker, version metadata, fork-safe cancellation and durable read-only crash recovery\n",dc42?"Disk Copy 4.2":"raw");
        }
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
        Request exportPacket = ExportFileFork{Native(source, file),
            Location::Windows(host + L"\\malformed-output.bin"), ForkSelector::Data};
        exportPacket.background = true;
        struct Rejection { unsigned fault; const char* expected; };
        const Rejection invalidRequests[] = {
            {8, "Copy requires a source"},
            {9, "Invalid fork selector"},
            {10, "Fork jobs require exactly one source"},
            {11, "Truncated copy-worker"},
            {12, "Invalid copy-worker string length"},
            {13, "Trailing copy-worker packet data"},
        };
        for (const auto& invalid : invalidRequests) {
            CopyProcess::fault = invalid.fault;
            auto refused = Submit(invalid.fault == 8 ? request : exportPacket);
            Wait(refused);
            CopyProcess::fault = 0;
            const auto result = refused.Job()->result();
            Check(result.code && result.message.find(invalid.expected) != std::string::npos &&
                  !refused.Snapshot().complete && refused.Snapshot().completed == 0,
                  result.message.c_str());
            Check(GetFileAttributesW((host + L"\\malformed-output.bin").c_str()) == INVALID_FILE_ATTRIBUTES,
                  "Rejected request created a fork output");
            Verify(*source, file);
            Verify(*target, child.id);
        }
        fprintf(report, "PASS: worker rejects missing and extra endpoints, invalid fork selectors, "
                        "truncated data and trailing endpoint data before writing output\n");
        Request exportRequest;
        exportRequest.background = true;
        exportRequest.Copy().sources = {Native(source, file)};

        exportRequest.Copy().destination.SetWindows(host);
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
            independent.Copy().sources = {Native(guardVolume, guarded)};
            auto parent = target->MakeDirectory(2, u"Cache isolation");
            independent.Copy().destination = Native(target, parent);
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
        roundtrip.Copy().sources = {Native(source, plain)};
        roundtrip.Copy().destination = Native(classic);
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
            copy.Copy().sources = {Native(source, links)};
            copy.Copy().destination = Native(target);
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
            copy.Copy().sources = {Native(source, inside.id)};
            copy.Copy().destination = Native(target, out);
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
            copy.Copy().sources = {Native(source, streamed)};
            copy.Copy().destination = Native(target);
            Pass(Submit(copy));
            Check(target->FindChild(2, u"Streamed metadata", first) &&
                      target->GetAttributes(first.id) == metadata,
                  "worker streamed attributes/compression decoding");
            Verify(*target, first.id, 4096);
            auto moveId = Put(*source, 2, u"In-place move", 4096),
                 moveFolder = source->MakeDirectory(2, u"Move target");
            auto moveSource = Native(source, moveId);
            moveSource.cut = true;
            copy.Copy().sources = {moveSource};
            copy.Copy().destination = Native(source, moveFolder);
            Pass(Submit(copy));
            Check(source->FindChild(moveFolder, u"In-place move", first) && first.id == moveId,
                  "worker same-volume move identity");
            Verify(*source, first.id, 4096);
            Request fork = ExportFileFork{};
            fork.background = true;
            fork.Get<ExportFileFork>()->source = Native(source, plain);
            fork.Get<ExportFileFork>()->fork = ForkSelector::Resource;

            fork.Get<ExportFileFork>()->output.SetWindows(host + L"\\resource.bin");
            Pass(Submit(fork));
            auto replaceId = Put(*target, 2, u"Replace resource", 4096);
            Request replace = ImportResourceFork{};
            replace.background = true;
            Location raw;

            raw.SetWindows(fork.Get<ExportFileFork>()->output.WindowsPath());
            replace.Get<ImportResourceFork>()->input = raw;
            replace.Get<ImportResourceFork>()->file = Native(target, replaceId);
            Pass(Submit(replace));
            Verify(*target, replaceId, 4096);
            fprintf(report, "PASS: hard links, directory-alias child selection, streamed 1 MiB attributes, "
                            "compressed input decoding, in-place move identity and fork export/import\n");
        }
        auto classic2 = OpenClassic(root + "\\classic.dsk", "Classic:", true);
        Request back;
        back.background = true;
        back.Copy().sources = {Native(classic2, classic2->List().front().id)};
        auto classicOut = target->MakeDirectory(2, u"Classic back");
        back.Copy().destination = Native(target, classicOut);
        Pass(Submit(back));
        Check(target->FindChild(classicOut, u"Plain", child), "classic worker input");
        Verify(*target, child.id, 4096);
        fprintf(report, "PASS: classic HFS read/write handoff, shared classic mount restoration\n");
        for (bool dc42 : {false, true}) {
            auto mfs = MfsImageFixture(Wide(root + (dc42 ? "\\mfs.dc42" : "\\mfs.img")), dc42);
            roundtrip.Copy().destination = Native(mfs);
            Pass(Submit(roundtrip));
            Entry e;
            Check(mfs->FindChild(2, u"Plain", e), "MFS process output");
            Verify(*mfs, e.id, 4096);
            auto dest = target->MakeDirectory(2, dc42 ? u"DC42 back" : u"MFS back");
            back.Copy().sources = {Native(mfs, e.id)};
            back.Copy().destination = Native(target, dest);
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
        auto external = OpenBrowserImage(extPath, journal);
        Check(ExecutionFor(*external).Persistent(), "External journal image did not get a worker session");
        roundtrip.Copy().destination = Native(external);
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
            import = FileOperations::LegacyConversion{};
            import.background = true;

            import.Legacy().source = FileOperations::Location::FromLegacy(root, input);

            import.Legacy().destination = FileOperations::Location::FromLegacy(root + "\\classic.dsk", "Classic:");
            import.Legacy().options.input = 'r';
            Pass(Submit(import));
            Entry mapped;
            Check(classic->FindChild(2, u"Mapped", mapped) && !memcmp(mapped.finder.data(), "TEXTTEST", 8),
                  "worker custom raw import mapping/extension strip");
            Request exportMapped;
            exportMapped = FileOperations::LegacyConversion{};
            exportMapped.background = true;

            exportMapped.Legacy().source =
                FileOperations::Location::FromLegacy(import.Legacy().destination.LegacyVolume(), "Classic:Mapped");
            exportMapped.SetLegacyTargetPath(root + "\\mapped-export");
            exportMapped.Legacy().options.output = 'r';
            Pass(Submit(exportMapped));
            Check(GetFileAttributesA((root + "\\mapped-export.zzz").c_str()) != INVALID_FILE_ATTRIBUTES,
                  "worker raw export extension mapping");
            fprintf(report, "PASS: captured custom type/creator mappings, extension stripping and mapped raw "
                            "export names\n");
        }
        const auto classicPath = root + "\\classic.dsk";
        auto conversion = [&](Location from, Location to, char mode) {
            LegacyConversion options;
            options.source = std::move(from);
            options.destination = std::move(to);
            options.options.input = options.options.output = mode;
            Request request = std::move(options);
            request.background = true;
            return request;
        };
        auto classicLocation = [&](const std::string& name) { return Location::Classic(classicPath, "Classic:" + name); };
        for (char mode : {'m', '5', '3', 'b'}) {
            std::string suffix = mode == 'b' ? ".hqx" : ".bin";
            auto container = Location::Windows(Wide(root + "\\container-" + mode + suffix));
            auto exportFile = conversion(classicLocation("Plain"), container, mode);
            Pass(Submit(exportFile));
            Check(GetFileAttributesW(container.WindowsPath().c_str()) != INVALID_FILE_ATTRIBUTES,
                  "Converted output missing");
            if(mode=='3'){
                CFile file(container.LegacyPath().c_str(),CFile::modeRead);
                unsigned char header[128]{};
                Check(file.Read(header,sizeof(header))==sizeof(header)&&!memcmp(header+102,"mBIN",4)&&
                      header[122]==130&&header[123]==129,"MacBinary III signature or versions differ");
            }
            auto duplicate = Submit(exportFile);
            Conflict(duplicate);
            duplicate.Job()->resolve(Storage::ConflictChoice::Skip);
            Wait(duplicate);
            Check(!duplicate.Job()->result().code && !duplicate.Snapshot().complete &&
                      duplicate.Job()->progress().skipped == 1, "Converted export did not use common overwrite handling");
            auto name = std::string("Imported ") + mode;
            Pass(Submit(conversion(container, classicLocation(name), mode)));
            Entry record;
            Check(classic->FindChild(2, U16(Wide(name)), record), "Decoded file missing");
            Verify(*classic, record.id, 4096);
            if (mode == 'b' || mode == '3') {
                CFile file(container.LegacyPath().c_str(), CFile::modeReadWrite | CFile::shareExclusive);
                if(mode=='b')file.SetLength(file.GetLength() / 2);
                else {
                    file.Seek(102,CFile::begin);
                    file.Write("x",1);
                }
                file.Close();
                auto damaged = Submit(conversion(container, classicLocation(name), mode));
                Wait(damaged);
                Check(damaged.Job()->result().code && !damaged.Snapshot().completed,
                      "Invalid BinHex or MacBinary III unexpectedly published");
                Verify(*classic, record.id, 4096);
            }
        }
        Entry text;
        text.name = u"Lines";
        text.forks[0].size = 4;
        memcpy(text.finder.data(), "TEXTUNIX", 8);
        classic->Put(2, text, [](unsigned, uint64_t at, void* p, size_t n) {
            memcpy(p, "a\rb\r" + at, n);
        }, false);
        auto textPath = Location::Windows(Wide(root + "\\lines.txt"));
        Pass(Submit(conversion(classicLocation("Lines"), textPath, 't')));
        {
            CFile file(textPath.LegacyPath().c_str(), CFile::modeRead);
            char contents[6]{};
            Check(file.Read(contents, sizeof(contents)) == sizeof(contents) &&
                      !memcmp(contents, "a\r\nb\r\n", sizeof(contents)), "Windows text line endings");
        }
        Pass(Submit(conversion(textPath, classicLocation("Imported text"), 't')));
        Check(classic->FindChild(2, u"Imported text", text) && text.forks[0].size == 4,
              "Imported text length");
        char resultText[4]{};
        classic->Read(text.id, 0, 0, resultText, sizeof(resultText));
        Check(!memcmp(resultText, "a\rb\r", sizeof(resultText)), "Macintosh text line endings");
        auto mixed = Wide(root + "\\mixed");
        Check(CreateDirectoryW(mixed.c_str(), nullptr) &&
                  CopyFileW(textPath.WindowsPath().c_str(), (mixed + L"\\Mixed.txt").c_str(), TRUE) &&
                  CopyFileW(Wide(root + "\\container-m.bin").c_str(), (mixed + L"\\Wrapped.bin").c_str(), TRUE),
              "Create mixed automatic conversion fixture");
        {
            CFile raw((root + "\\mixed\\Raw.dat").c_str(), CFile::modeCreate | CFile::modeWrite);
            raw.Write("raw", 3);
        }
        Pass(Submit(conversion(Location::Windows(mixed), classicLocation(""), 'a')));
        Entry mixedFolder, wrapped, raw, lines;
        Check(classic->FindChild(2, u"mixed", mixedFolder) && mixedFolder.directory &&
                  classic->FindChild(mixedFolder.id, u"Plain", wrapped) &&
                  classic->FindChild(mixedFolder.id, u"Raw.dat", raw) && raw.forks[0].size == 3 &&
                  classic->FindChild(mixedFolder.id, u"Mixed", lines) && lines.forks[0].size == 4,
              "Automatic folder transfer did not select an adapter for each file");
        Verify(*classic, wrapped.id, 4096);
        Check(GetFileAttributesW(Wide(root + "\\%container-m.bin").c_str()) == INVALID_FILE_ATTRIBUTES,
              "Portable container unexpectedly needs a sidecar");
        fprintf(report, "PASS: automatic folder transfer mixes text, wrapped forks and raw files\n");
        fprintf(report, "PASS: common pipeline MacBinary I/II/III and BinHex fork round trips, overwrite Skip, "
                        "malformed header rollback and text line endings\n");
        auto slow = Put(*source, 2, u"Slow", 2 * 1024 * 1024);
        Request delayed;
        delayed.background = true;
        delayed.Copy().sources = {Native(source, slow)};
        delayed.Copy().destination = Native(target);
        {
            struct CancellationGate {
                HANDLE event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
                CancellationGate() {
                    Check(event != nullptr, "create cancellation test gate");
                    CopyProcess::ioGate = event;
                }
                ~CancellationGate() {
                    SetEvent(event);
                    CopyProcess::ioGate = nullptr;
                    CloseHandle(event);
                }
            } gate;
            CopyProcess::fault = 7;
            auto running = Submit(delayed);
            Until([&] {
                return running.Job()->done() || running.Job()->progress().phase == "Worker test gate";
            });
            Check(!running.Job()->done(), "worker did not reach the cancellation gate");
            Denied(Wide(root + "\\source.dsk"));
            Denied(Wide(root + "\\target.dsk"));
            auto unrelated = Image(Wide(root + "\\unrelated.dsk"));
            Put(*unrelated, 2, u"Independent", 4096);
            running.Job()->cancel();
            Wait(running);
            Check(running.Job()->result().outcome == Storage::Outcome::Cancelled &&
                      !running.Job()->result().partial, "slow worker cancellation");
            CopyProcess::fault = 0;
        }
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
        WorkerTransport::lastExitCode = 0;
        CopyProcess::fault = 4;
        auto interrupted = Submit(delayed);
        Wait(interrupted);
        Check(interrupted.Job()->result().code && !interrupted.Snapshot().complete &&
                  WorkerTransport::lastExitCode == 74 && interrupted.Job()->result().rollback == Storage::Rollback::Unverified,
              "Expected the worker to exit inside its write hook");
        CopyProcess::fault = 0;
        Check(VolumeCoord::Available({{source->ResourceKey(),VolumeCoord::Mode::Write},
                                      {target->ResourceKey(),VolumeCoord::Mode::Write}}),
              "completion after a worker crash retained a volume reservation");
        Check(source->Stat(slow).forks[0].size == 2 * 1024 * 1024, "worker crash retained source");
        Check(source->Writable() && target->Writable() &&
                  !source->List(2).empty() && !target->List(2).empty(),
              "Browser could not list the recovered journaled mounts");
        Verify(*source, slow, 2 * 1024 * 1024);
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
            first.Copy().destination = Native(target, delayedFolder);
            struct Gate {
                HANDLE event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
                Gate() {
                    Check(event != nullptr, "parallel process gate");
                    CopyProcess::ioGate = event;
                }
                ~Gate() {
                    SetEvent(event);
                    CopyProcess::ioGate = nullptr;
                    CloseHandle(event);
                }
            } gate;
            CopyProcess::fault = 7;
            auto blocked = Submit(first);
            Until([&] {
                return blocked.Job()->progress().phase == "Worker test gate" || blocked.Job()->done();
            });
            Check(!blocked.Job()->done(), "first process did not stay active");
            auto firstPid = CopyProcess::lastProcess.load();
            std::vector<Operation> queued;
            for (unsigned n = 0; n < 7; ++n)
                queued.push_back(Submit(first));
            CopyProcess::fault = 0;
            Request independent;
            independent.background = true;
            independent.Copy().sources = {Native(otherSource, otherFile)};
            independent.Copy().destination = Native(otherTarget);
            Pass(Submit(independent));
            Check(!blocked.Job()->done() && CopyProcess::lastProcess != firstPid,
                  "independent copy process serialized behind reserved volume");
            for (auto &waiting : queued)
                waiting.Job()->cancel();
            for (auto &waiting : queued) {
                Wait(waiting);
                Check(waiting.Job()->result().error == ERROR_CANCELLED, "queued process copy cancellation");
            }
            SetEvent(gate.event);
            Pass(blocked);
            Check(otherTarget->FindChild(2, u"Parallel", child), "independent child output");
            Verify(*otherTarget, child.id, 4096);
            fprintf(report, "PASS: simultaneous independent child processes, seven conflicting requests "
                            "queued without worker starvation, queued cancellation\n");
            auto nonjournal = MfsImageFixture(Wide(root + "\\unclean-mfs.img"), false);
            Request unsafe;
            unsafe.background = true;
            unsafe.Copy().sources = {Native(source, plain)};
            unsafe.Copy().destination = Native(nonjournal);
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
                physical.Copy().sources = {Native(source, plain)};

                char path[MAX_PATH];
                get_floppy_volume_file_name(25, path);
                physical.Copy().destination.SetClassic(path, "Floppy:");
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
            ui.Copy().sources = {Native(source, slow)};
            ui.Copy().destination = Native(target, uiFolder);
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
            ui.Copy().destination = Native(target, closingFolder);
            struct ShutdownGate {
                HANDLE event=CreateEvent(nullptr,TRUE,FALSE,nullptr);
                ShutdownGate() {
                    Check(event!=nullptr,"create last-owner cancellation gate");
                    CopyProcess::ioGate=event;
                    CopyProcess::fault=7;
                }
                ~ShutdownGate() {
                    SetEvent(event);
                    CopyProcess::ioGate=nullptr;
                    CopyProcess::fault=0;
                    CloseHandle(event);
                }
            } shutdownGate;
            Check(BackgroundCopies::Start({ui}, &second, true), "last-owner process start");
            Until([] {
                auto pending = Pending();
                return !pending.empty() && pending.front().Job()->progress().phase == "Worker test gate";
            });
            auto closing = Pending();
            Check(closing.size() == 1, "last-owner active operation");
            auto closingJob = closing.front().Job();
            BackgroundCopies::CancelAll();
            BackgroundCopies::Shutdown();
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
