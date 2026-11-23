// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include <future>
#include "ClassicCopyArchive.h"
#include "BrowserPresentation.h"
#include "HfsPlusCompression.h"
#include "CopyProcess.h"
#include "ApfsFormat.h"
#include "ApfsContainer.h"
#include "ApfsTree.h"
#include "ApfsWriteDisk.h"
#include "ApfsVolume.h"
#include "HfsPlusDisk.h"
#include "HfsPlusTime.h"
#include "HfsPlusCompression.h"
#include "VolumeSession.h"
#include "VolumeSessionProtocol.h"
#include "StorageLocation.h"
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
HfsPlus::Attributes UserCopyAttributes(Volume& volume,uint32_t id)
{
    auto attributes=volume.GetAttributes(id);
    attributes.erase(MacBrowser::ApfsTimesAttribute);
    return attributes;
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
    Capabilities caps;
    caps.writable = caps.rename = caps.attributes = caps.compression = caps.hardLinks = true;
    caps.createFolders = false;
    Data capabilityPacket;
    Protocol::EncodeCapabilities(capabilityPacket,caps);
    const auto decodedCaps=Protocol::DecodeCapabilities(capabilityPacket);
    capabilityPacket.end();
    Check(decodedCaps.rename && decodedCaps.CanCreateHardLinks() && decodedCaps.CanEditCompression() &&
          decodedCaps.CanEditAttributes() && !decodedCaps.createFolders && !decodedCaps.basicMetadata,
          "worker capability description changed in transit");
    reject([&]{
        auto malformed=caps;
        malformed.writable=false;
        Data packet;
        Protocol::EncodeCapabilities(packet,malformed);
        Protocol::DecodeCapabilities(packet);
    },"accepted a read-only capability description with rename enabled");
    reject([]{Data data;data.put(1000);Protocol::DecodeCommand(data,false,false);},"accepted unknown volume opcode");
    reject([]{auto data=Protocol::EncodeCommand(Command::Put,{});Protocol::DecodeCommand(data,false,false);},
           "accepted missing streamed attributes");
    reject([]{auto data=Protocol::EncodeCommand(Command::MakeAttributedDirectory,{});Protocol::DecodeCommand(data,false,false);},
           "accepted an attributed folder request without attributes");
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
    auto reply=Protocol::Encode(Protocol::Reply{1,"Cancelled",Storage::Rollback::Failed,ERROR_CANCELLED,true});
    const auto decodedReply=Protocol::DecodeReply(reply);
    Check(decodedReply.error==ERROR_CANCELLED&&decodedReply.retained&&decodedReply.rollback==Storage::Rollback::Failed,
          "Volume reply lost cancellation, retained changes or rollback evidence");
    reject([]{auto data=Protocol::Encode(Protocol::Reply{0,"",Storage::Rollback::None,ERROR_MEDIA_CHANGED});
              Protocol::DecodeReply(data);},"accepted a successful session result with an error");
    reject([]{auto data=Protocol::Encode(Protocol::Reply{1,"Missing error",Storage::Rollback::None});
              Protocol::DecodeReply(data);},"accepted a failed session result without an error code");

    Entry link;
    link.name=u"Shortcut";link.symbolicLink=true;link.expectedDestinationIdentity=EntryIdentity("42");
    const HfsPlus::Bytes path{'.','.','/','a',0};
    auto linkPacket=Protocol::EncodeSymbolicLink(2,link,path);
    const auto linkRequest=Protocol::DecodeSymbolicLink(linkPacket);
    Check(linkRequest.parent==2&&linkRequest.target==path&&linkRequest.entry.symbolicLink&&
          linkRequest.entry.expectedDestinationIdentity==link.expectedDestinationIdentity,
          "Symbolic link request lost its target or approved destination");
    reject([&]{
        auto data=Protocol::EncodeSymbolicLink(2,link,path);
        data.bytes.back()='b';
        Protocol::DecodeSymbolicLink(data);
    },"accepted a symbolic link without a terminator");
    reject([&]{
        auto data=Protocol::EncodeSymbolicLink(2,link,path);
        data.bytes.push_back(0);
        Protocol::DecodeSymbolicLink(data);
    },"accepted trailing symbolic link request data");

    Entry entry;
    entry.id=42;
    entry.name=u"Both forks \u03a9";
    entry.forks[0].size=300000;
    entry.forks[1].size=71;
    auto linked=entry;
    linked.hardLink=true;
    linked.linkCount=2;
    using Scope=HfsPlus::Volume::OverwriteScope;
    auto transfer=Protocol::EncodePut({2,true,true,0,0,Scope::SelectedName},linked);
    const auto decodedTransfer=Protocol::DecodePut(transfer);
    Check(decodedTransfer.entry.hfsOverwriteScope==Scope::SelectedName,
          "Hard-link name replacement intent did not survive the wire");
    for(auto scope:{Scope::SelectedName,Scope::SymbolicLink}){
        auto ordinaryTransfer=Protocol::EncodePut({2,true,true,0,0,scope},entry);
        const auto decodedOrdinary=Protocol::DecodePut(ordinaryTransfer);
        Check(decodedOrdinary.entry.hfsOverwriteScope==scope&&!decodedOrdinary.entry.hardLink,
              "Ordinary file name replacement intent did not survive the wire");
        reject([&]{auto data=Protocol::EncodePut({2,false,true,0,0,scope},entry);Protocol::DecodePut(data);},
               "accepted name replacement without overwrite approval");
        reject([&]{auto data=Protocol::EncodePut({2,true,true,42,0,scope},entry);Protocol::DecodePut(data);},
               "accepted name replacement during a resource fork edit");
    }
    reject([&]{auto data=Protocol::EncodePut({2,true,true,0,0,Scope(3)},entry);Protocol::DecodePut(data);},
           "accepted an unknown HFS+ overwrite scope");
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
    if(image->Supports().attributes)image->SetAttributes(id,attributes);
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
              (!image->Supports().attributes || image->GetAttributes(updated.id)==attributes),
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
void Conflict(const Operation &op,uint64_t after=0) {
    const auto pending=[&]{const auto prompt=op.Job()->conflictState();return prompt.id>after&&!prompt.path.empty();};
    DWORD until = GetTickCount() + 10000;
    while (!pending() && !op.Job()->done() && LONG(until - GetTickCount()) > 0)
        Sleep(10);
    if (op.Job()->done() && op.Job()->result().code)
        throw HfsPlus::Error(op.Job()->result().message);
    Check(pending(), "worker copy prompt missing");
}
void Denied(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    Check(h == INVALID_HANDLE_VALUE, "image exclusivity lost during handoff");
}
std::shared_ptr<Volume> ClassicImage(const std::string &path,uint64_t size=4*1024*1024) {
    LegacyEngine::Scope engine;
    floppy_module_global_init();
    {
        CFile f;
        Check(f.Open(path.c_str(), CFile::modeCreate | CFile::modeWrite) != FALSE, "classic fixture");
        f.SetLength(size);
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
        auto classicCopy=Submit(request);
        Conflict(classicCopy);
        Check(classicCopy.Job()->conflictState().kind==Storage::CopyPrompt::ClassicMetadataLoss,"Missing shipping classic copy warning");
        classicCopy.Job()->resolve(Storage::ConflictChoice::Replace);
        Pass(classicCopy);
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
        const auto revisionBeforeDelete = VolumeCoord::Revision(target->ResourceKey());
        CopyProcess::lastProcess = 0;
        auto partial = Submit(DeleteEntries{{Native(target, partialId), Native(target)}});
        Wait(partial);
        Entry remaining;
        Check(CopyProcess::lastProcess && partial.Job()->result().code &&
                  partial.Job()->result().outcome == Storage::Outcome::Failed && partial.Job()->result().partial &&
                  partial.Job()->result().completed == 1 && partial.Snapshot().completed == 1 &&
                  !partial.Snapshot().affected.empty() &&
                  VolumeCoord::Revision(target->ResourceKey()) == revisionBeforeDelete + 1 &&
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

namespace {
// Patch BSD metadata in the single leaf of a small, disposable HFS fixture.
struct HfsFixtureMode {
    uint32_t id;
    uint32_t uid, gid;
    uint16_t mode;
};
void SetHfsFixtureModes(HfsPlus::Source& disk,const std::vector<HfsFixtureMode>& entries)
{
    using HfsPlus::Disk::Read;
    using HfsPlus::Disk::U16;
    using HfsPlus::Disk::U32;
    using HfsPlus::Disk::P16;
    using HfsPlus::Disk::P32;
    const auto header=Read(disk,1024,512);
    const auto blockSize=U32(header.data()+40);
    const auto base=uint64_t(U32(header.data()+288))*blockSize;
    const auto length=uint64_t(U32(header.data()+292))*blockSize;
    const auto tree=Read(disk,base,64);
    const auto nodeSize=U16(tree.data()+32);
    const auto nodeNumber=U32(tree.data()+24);
    const auto offset=uint64_t(nodeSize)*nodeNumber;
    Check(nodeSize>=512&&nodeNumber&&offset+nodeSize<=length,"Expected a bounded fixture catalog leaf");
    auto node=Read(disk,base+offset,nodeSize);
    Check(node[8]==0xff&&!U32(node.data()),"Expected a single fixture catalog leaf");
    size_t found=0;
    for(unsigned i=0;i<U16(node.data()+10);++i){
        Check((i+2)*2<node.size(),"Invalid fixture catalog offsets");
        const auto start=U16(node.data()+node.size()-(i+1)*2);
        const auto end=U16(node.data()+node.size()-(i+2)*2);
        Check(start+2u<=end&&end<=node.size(),"Invalid fixture catalog record");
        const auto value=start+2u+U16(node.data()+start);
        Check(value+2<=end,"Truncated fixture catalog record");
        auto record=node.data()+value;
        const auto type=U16(record);
        if(type!=1&&type!=2)continue;
        Check(value+44<=end,"Truncated fixture BSD metadata");
        for(const auto& info:entries){
            if(U32(record+8)!=info.id)continue;
            P32(record+32,info.uid);
            P32(record+36,info.gid);
            P16(record+42,info.mode);
            if ((info.mode & 0170000) == 0120000) memcpy(record+48,"slnkrhap",8);
            ++found;
            break;
        }
    }
    Check(found==entries.size(),"Missing fixture catalog permissions");
    disk.Write(base+offset,node.data(),node.size());
    disk.Flush();
}

void TestHfsToApfsPosix(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const PosixMetadata folderMode{501,80,02750};
    const PosixMetadata fileMode{0,20,04751};
    const PosixMetadata existingMode{502,81,0700};
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive}){
        const char* format=kind==Kind::HfsPlus?"hfsplus":"hfsx";
        const auto sourcePath=Wide(root+"\\"+format+"-posix.dsk");
        const auto targetPath=Wide(root+"\\"+format+"-posix-apfs.img");
        {
            auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"POSIX source",true,kind);
            uint32_t folder,file;
            {
                HfsPlus::Volume hfs(*disk,true);
                folder=hfs.MakeDirectory(2,u"System");
                file=hfs.Put(folder,u"launchd",4096,0,[](unsigned,uint64_t,void* out,size_t size){
                    memset(out,0x5b,size);
                });
            }
            SetHfsFixtureModes(*disk,{
                {folder,folderMode.uid,folderMode.gid,uint16_t(0040000|folderMode.mode)},
                {file,fileMode.uid,fileMode.gid,uint16_t(0100000|fileMode.mode)}});
        }
        const auto external=kind==Kind::HfsXCaseSensitive?_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE"):nullptr;
        if(external){
            Check(CopyFileW(external,targetPath.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
        }else{
            auto disk=HfsPlus::CreateImage(targetPath.c_str(),32*1024*1024);
            Storage::Job job;
            Apfs::Format(*disk,u"POSIX destination",true,job);
        }
        auto source=OpenBrowserImage(sourcePath);
        auto target=OpenBrowserImage(targetPath);
        Entry sourceFolder,sourceFile;
        Check(source&&VolumeSession::VolumeSessionPidForTest(source)&&target&&target->CaseSensitive()&&
              source->FindChild(2,u"System",sourceFolder)&&sourceFolder.posix==folderMode&&
              source->FindChild(sourceFolder.id,u"launchd",sourceFile)&&sourceFile.posix==fileMode,
              "The mounted HFS worker did not return source ownership and permissions");
        const auto copy=[&](uint32_t parent,bool overwrite){
            Storage::Job job;
            job.transport({},[&](const auto&){
                return overwrite?Storage::ConflictChoice::Replace:Storage::ConflictChoice::Cancel;
            });
            Completion completion;
            const auto result=ExecuteInWorker(
                OrdinaryCopy{{Native(source,sourceFolder.id)},Native(target,parent)},job,completion);
            if(result.code)throw HfsPlus::Error(result.message);
            Check(completion.complete,"Incomplete HFS to APFS POSIX copy");
        };
        const auto checkCopy=[&](uint32_t parent,const PosixMetadata& expectedFolder){
            Entry folder,file;
            Check(target->FindChild(parent,u"System",folder)&&folder.directory&&folder.posix==expectedFolder&&
                  target->FindChild(folder.id,u"launchd",file)&&!file.directory&&file.posix==fileMode&&
                  file.forks[0].size==4096,"APFS copy lost source ownership or permission bits");
            std::array<uint8_t,4096> bytes{};
            target->Read(file.id,0,0,bytes.data(),bytes.size());
            Check(std::all_of(bytes.begin(),bytes.end(),[](auto b){return b==0x5b;}),
                  "APFS copy changed executable bytes");
        };
        copy(2,false);
        checkCopy(2,folderMode);
        const auto mergeParent=target->MakeDirectory(2,u"Merge");
        Entry existingFolder;existingFolder.name=u"System";existingFolder.directory=true;existingFolder.posix=existingMode;
        const auto folder=target->MakeDirectoryWithAttributes(mergeParent,existingFolder);
        Entry prior;prior.name=u"launchd";prior.posix=existingMode;prior.forks[0].size=512;
        target->Put(folder,prior,[](unsigned,uint64_t,void* out,size_t size){memset(out,0,size);},false);
        copy(mergeParent,true);
        checkCopy(mergeParent,existingMode);
        target.reset();
        target=OpenBrowserImage(targetPath);
        Entry merge;
        Check(target&&target->FindChild(2,u"Merge",merge),"Missing reopened APFS transfer fixture");
        checkCopy(2,folderMode);
        checkCopy(merge.id,existingMode);
        if(external)fprintf(report,"PASS: ownership and permissions persisted on a copy of the shared macOS APFS fixture.\n");
    }
    fprintf(report,"PASS: HFS+/HFSX worker copies preserve APFS UID/GID, executable and special mode bits, and nested folder permissions after reopening. Overwrites use source permissions and folder merges retain destination permissions.\n");
}

void TestCopyPosixIntoHfs(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const PosixMetadata folderMode{503,81,02750};
    const PosixMetadata executableMode{0,20,04751};
    const PosixMetadata noAccessMode{0xf0000123,0x80000056,0000};
    const PosixMetadata retainedMode{504,82,0710};
    const size_t forkSizes[]={5003,719};
    const auto payload=[](unsigned fork,uint64_t at,void* out,size_t size){
        auto bytes=static_cast<uint8_t*>(out);
        for(size_t i=0;i<size;++i)bytes[i]=uint8_t((at+i)*17+fork*29);
    };
    const auto formatHfs=[](const std::wstring& path,Kind kind){
        auto disk=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
        HfsPlus::Volume::Format(*disk,u"Unix modes",true,kind);
        return disk;
    };
    const auto apfsPath=Wide(root+"\\unix-source-apfs.img");
    const auto fixture=_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE");
    if(fixture){
        Check(CopyFileW(fixture,apfsPath.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
    }else{
        auto disk=HfsPlus::CreateImage(apfsPath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Unix source",true,job);
    }
    {
        auto source=OpenBrowserImage(apfsPath);
        Check(source&&source->Supports().create,"Cannot seed the APFS POSIX source");
        Entry folder;folder.directory=true;folder.name=u"Unix metadata";folder.posix=folderMode;
        const auto id=source->MakeDirectoryWithAttributes(2,folder);
        Entry executable;executable.name=u"Executable";executable.posix=executableMode;
        for(unsigned fork=0;fork<2;++fork)executable.forks[fork].size=forkSizes[fork];
        source->Put(id,executable,payload,false);
        Entry noAccess;noAccess.name=u"No access";noAccess.posix=noAccessMode;
        source->Put(id,noAccess,payload,false);
    }
    for(unsigned from=0;from<3;++from){
        const auto sourcePath=from==0?apfsPath:Wide(root+"\\unix-source-"+std::to_string(from)+".dsk");
        if(from){
            auto disk=formatHfs(sourcePath,from==1?Kind::HfsPlus:Kind::HfsXCaseSensitive);
            uint32_t folder,file,noAccess;
            {
                HfsPlus::Volume hfs(*disk,true);
                folder=hfs.MakeDirectory(2,u"Unix metadata");
                file=hfs.Put(folder,u"Executable",forkSizes[0],forkSizes[1],payload);
                noAccess=hfs.Put(folder,u"No access",0,0,payload);
                Check(hfs.Stat(folder).posix==PosixMetadata{0,0,0755}&&
                      hfs.Stat(file).posix==PosixMetadata{0,0,0644},
                      "Fresh HFS entries did not expose their written default permissions");
            }
            SetHfsFixtureModes(*disk,{
                {folder,folderMode.uid,folderMode.gid,uint16_t(0040000|folderMode.mode)},
                {file,executableMode.uid,executableMode.gid,uint16_t(0100000|executableMode.mode)},
                {noAccess,noAccessMode.uid,noAccessMode.gid,uint16_t(0100000|noAccessMode.mode)}});
        }

        for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive}){
            const auto targetPath=Wide(root+"\\unix-target-"+std::to_string(from)+
                (kind==Kind::HfsPlus?"-plus.dsk":"-x.dsk"));
            {
                auto disk=formatHfs(targetPath,kind);
                uint32_t folder,file;
                {
                    HfsPlus::Volume hfs(*disk,true);
                    const auto merge=hfs.MakeDirectory(2,u"Merge");
                    folder=hfs.MakeDirectory(merge,u"Unix metadata");
                    file=hfs.Put(folder,u"Executable",512,0,payload);
                }
                SetHfsFixtureModes(*disk,{
                    {folder,retainedMode.uid,retainedMode.gid,uint16_t(0040000|retainedMode.mode)},
                    {file,retainedMode.uid,retainedMode.gid,uint16_t(0100000|retainedMode.mode)}});
            }
            const auto verify=[&](Volume& volume,uint32_t parent,const PosixMetadata& expectedFolder){
                Entry folder,file,noAccess;
                Check(volume.FindChild(parent,u"Unix metadata",folder)&&folder.directory&&folder.posix==expectedFolder&&
                      volume.FindChild(folder.id,u"Executable",file)&&!file.directory&&file.posix==executableMode&&
                      volume.FindChild(folder.id,u"No access",noAccess)&&noAccess.posix==noAccessMode,
                      "Copied ownership, execute bits or folder permissions differ");
                for(unsigned fork=0;fork<2;++fork){
                    Check(file.forks[fork].size==forkSizes[fork],"Executable copy lost a fork");
                    HfsPlus::Bytes actual(forkSizes[fork]),expected(forkSizes[fork]);
                    volume.Read(file.id,fork,0,actual.data(),actual.size());
                    payload(fork,0,expected.data(),expected.size());
                    Check(actual==expected,"Executable copy changed fork bytes");
                }
            };
            {
                auto source=OpenBrowserImage(sourcePath);
                auto target=OpenBrowserImage(targetPath);
                Entry folder,merge;
                Check(source&&target&&VolumeSession::VolumeSessionPidForTest(target)&&
                      (!from||VolumeSession::VolumeSessionPidForTest(source))&&
                      source->FindChild(2,u"Unix metadata",folder)&&target->FindChild(2,u"Merge",merge),
                      "Could not mount Unix metadata copy fixtures");
                verify(*source,2,folderMode);
                const auto copy=[&](uint32_t parent,bool overwrite){
                    bool prompted=false;
                    Storage::Job job;
                    job.transport({},[&](const auto&){
                        prompted=true;
                        return overwrite?Storage::ConflictChoice::Replace:Storage::ConflictChoice::Cancel;
                    });
                    Completion completion;
                    const auto result=ExecuteInWorker(OrdinaryCopy{{Native(source,folder.id)},Native(target,parent)},job,completion);
                    if(result.code)throw HfsPlus::Error(result.message);
                    Check(completion.complete&&prompted==overwrite,"Unexpected HFS copy completion or overwrite prompt");
                };
                copy(2,false);
                copy(merge.id,true);
                verify(*target,2,folderMode);
                verify(*target,merge.id,retainedMode);
            }
            auto target=OpenBrowserImage(targetPath);
            Entry merge;
            Check(target&&target->FindChild(2,u"Merge",merge),"Missing reopened HFS merge fixture");
            verify(*target,2,folderMode);
            verify(*target,merge.id,retainedMode);
        }
    }
    if(fixture)fprintf(report,"PASS: Unix ownership and permissions copied from the shared macOS APFS fixture into HFS+ and HFSX.\n");
    fprintf(report,"PASS: APFS, HFS+ and HFSX copies into HFS+ and HFSX preserve file and folder UID/GID, executable and special bits, zero permissions and both forks after reopen. Folder merges retain destination permissions.\n");
}

void TestSymbolicLinkCopies(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    constexpr auto relativeName=u"relativ\u00e9";
    const auto hfsRelativeName=HfsPlus::Normalize(relativeName);
    struct LinkFixture {
        std::u16string name;
        std::string target;
        PosixMetadata posix;
    };
    const std::array<LinkFixture,3> links{{
        {hfsRelativeName,"../target",{501,80,0751}},
        {u"absolute",u8"/System/Library/\u00e9",{0xf0000123,0x80000056,0000}},
        {u"loop","loop",{502,20,04777}}
    }};
    const PosixMetadata oldPosix{22,45,0700};
    const HfsPlus::Bytes oldTarget{'/','o','l','d',0};
    const auto verifyLinks=[&](MacBrowser::Volume& volume,const Entry& folder){
        const auto children=volume.List(folder.id);
        Check(children.size()==links.size(),"Unexpected files in copied link folder");
        for(const auto& [name,path,posix]:links){
            Entry link;
            HfsPlus::Bytes expected(path.begin(),path.end());expected.push_back(0);
            const auto lookup=std::string_view(volume.Kind())=="APFS"&&name==hfsRelativeName?relativeName:name;
            Check(volume.FindChild(folder.id,lookup,link)&&link.symbolicLink&&!link.directory&&
                  !link.forks[0].size&&!link.forks[1].size&&link.posix==posix&&
                  volume.SymbolicLinkTarget(link.id)==expected,
                  "Symbolic link type, ownership, permissions or target bytes changed");
        }
    };
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const char* format=kind==Kind::HfsPlus?"hfsplus":kind==Kind::HfsXCaseSensitive?"hfsx":"hfsx-folded";
        const auto sourcePath=Wide(root+"\\"+format+"-symlinks.dsk");
        const auto targetPath=Wide(root+"\\"+format+"-symlinks-apfs.img");
        {
            auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Link source",true,kind);
            std::vector<HfsFixtureMode> modes;
            {
                HfsPlus::Volume hfs(*disk,true);
                const auto folder=hfs.MakeDirectory(2,u"Links");
                hfs.Put(2,u"target",1024,0,[](unsigned,uint64_t,void* out,size_t size){
                    memset(out,0x37,size);
                });
                for(const auto& [name,path,posix]:links){
                    const auto id=hfs.Put(folder,name,path.size(),0,[&](unsigned,uint64_t at,void* out,size_t size){
                        memcpy(out,path.data()+at,size);
                    });
                    modes.push_back({id,posix.uid,posix.gid,uint16_t(0120000|posix.mode)});
                }
                constexpr char badTarget[]={'a',0,'b'};
                const auto bad=hfs.Put(2,u"invalid",sizeof(badTarget),0,
                    [&](unsigned,uint64_t at,void* out,size_t size){memcpy(out,badTarget+at,size);});
                modes.push_back({bad,501,80,0120777});
            }
            SetHfsFixtureModes(*disk,modes);
        }
        const auto external=kind==Kind::HfsXCaseSensitive?_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE"):nullptr;
        if(external){
            Check(CopyFileW(external,targetPath.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
        }else{
            auto disk=HfsPlus::CreateImage(targetPath.c_str(),32*1024*1024);
            Storage::Job job;
            Apfs::Format(*disk,u"Link destination",true,job);
        }
        {
            auto source=OpenBrowserImage(sourcePath);
            auto target=OpenBrowserImage(targetPath);
            Entry folder;
            Check(source&&VolumeSession::VolumeSessionPidForTest(source)&&target&&
                  source->FindChild(2,u"Links",folder),"Unable to mount symlink copy fixtures");
            verifyLinks(*source,folder);
            Entry regular;
            Check(source->FindChild(2,u"target",regular)&&!source->SymbolicLinkTarget(regular.id),
                  "An ordinary file was classified as a symbolic link");
            Entry bad,unexpected;
            Check(source->FindChild(2,u"invalid",bad)&&bad.symbolicLink,"Missing invalid symlink fixture");
            Storage::Job rejected;
            Completion refusal;
            const auto failed=ExecuteInWorker(OrdinaryCopy{{Native(source,bad.id)},Native(target,2)},rejected,refusal);
            Check(failed.code&&failed.message.find("Invalid HFS+ symbolic link target")!=std::string::npos&&
                  !target->FindChild(2,u"invalid",unexpected),"Malformed HFS+ symlink was copied");
            const auto destination=target->MakeDirectory(2,u"Links");
            Entry old;
            old.name=relativeName;
            old.symbolicLink=true;
            old.posix=oldPosix;
            Storage::Job seed;
            target->PutSymbolicLink(destination,old,oldTarget,seed);
            Entry seeded;
            Check(target->FindChild(destination,old.name,seeded)&&seeded.posix==oldPosix&&
                  target->SymbolicLinkTarget(seeded.id)==oldTarget,"Could not seed distinct APFS symlink metadata");
            unsigned prompts=0;
            Storage::Job job;
            job.transport({},[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
            Completion completion;
            const auto result=ExecuteInWorker(OrdinaryCopy{{Native(source,folder.id)},Native(target,2)},job,completion);
            if(result.code)throw HfsPlus::Error(result.message);
            Check(completion.complete&&prompts==1,"Incomplete HFS to APFS symlink copy or overwrite");
        }
        auto target=OpenBrowserImage(targetPath);
        Entry folder,unexpected;
        Check(target&&target->FindChild(2,u"Links",folder)&&!target->FindChild(2,u"target",unexpected),
              "The copied folder is missing or the relative link target was followed");
        verifyLinks(*target,folder);
        if(external)fprintf(report,"PASS: HFSX symbolic links reopen on the shared macOS APFS fixture.\n");
    }
    fprintf(report,"PASS: HFS+/HFSX worker copies preserve symbolic-link UID, GID, mode, and relative, absolute UTF-8, and self-referential targets on APFS.\n");

    const char* images[]={"hfsplus-symlinks.dsk","hfsx-symlinks.dsk","hfsx-folded-symlinks.dsk","hfsx-symlinks-apfs.img"};
    for(size_t from=0;from<ARRAYSIZE(images);++from){
        const auto sourcePath=Wide(root+"\\"+images[from]);
        for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
            const char* suffix=kind==Kind::HfsPlus?"-plus.dsk":kind==Kind::HfsXCaseSensitive?"-x.dsk":"-folded.dsk";
            const auto targetPath=Wide(root+"\\symlinks-to-"+std::to_string(from)+suffix);
            {
                auto disk=HfsPlus::CreateImage(targetPath.c_str(),16*1024*1024);
                HfsPlus::Volume::Format(*disk,u"Link destination",true,kind);
            }
            {
                auto source=OpenBrowserImage(sourcePath);
                auto target=OpenBrowserImage(targetPath);
                Entry folder;
                Check(source&&target&&source->FindChild(2,u"Links",folder)&&VolumeSession::VolumeSessionPidForTest(target),
                      "Could not mount symlink copy fixtures");
                const auto referent=Put(*target,2,u"target",1024);
                const auto destination=target->MakeDirectory(2,u"Links");
                const bool fromApfs=std::string_view(source->Kind())=="APFS";
                Entry relative;
                Check(source->FindChild(folder.id,fromApfs?relativeName:hfsRelativeName,relative),
                      "Missing relative source link");
                Check(!fromApfs||relative.name==relativeName,"APFS fixture lost the composed source spelling");
                const auto copyRelative=[&](uint32_t parent,Storage::Job& job,Completion& completion){
                    return ExecuteInWorker(OrdinaryCopy{{Native(source,relative.id)},Native(target,parent)},job,completion);
                };
                const HfsPlus::Attributes originalAttrs{{u"user.original",{0x32,0,0x41}}};
                uint32_t original=0;
                if(fromApfs){
                    original=Put(*target,destination,hfsRelativeName,4099,11);
                    target->SetAttributes(original,originalAttrs);

                    const auto group=target->MakeDirectory(2,u"Hard links");
                    const auto shared=Put(*target,group,hfsRelativeName,3011,13);
                    const auto firstAlias=target->MakeHardLink(shared,2,u"First retained alias");
                    const auto secondAlias=target->MakeHardLink(firstAlias,2,u"Second retained alias");
                    const auto verifyGroup=[&](bool replaced){
                        Entry selected;
                        const auto first=target->Stat(firstAlias);
                        const auto second=target->Stat(secondAlias);
                        Check(first.hardLink&&second.hardLink&&first.resolvedId==shared&&second.resolvedId==shared&&
                              first.linkCount==(replaced?2u:3u)&&second.linkCount==first.linkCount,
                              "The surviving HFS aliases changed inode or link count");
                        Verify(*target,first.id,3011,13);
                        Verify(*target,second.id,3011,13);
                        Check(target->FindChild(group,hfsRelativeName,selected),"The selected HFS name disappeared");
                        if(replaced){
                            Check(selected.symbolicLink&&!selected.hardLink&&selected.resolvedId!=shared&&
                                  selected.posix==relative.posix&&
                                  target->SymbolicLinkTarget(selected.id)==source->SymbolicLinkTarget(relative.id),
                                  "Replacing a hard-link name did not publish the APFS symlink");
                        }else{
                            Check(selected.hardLink&&selected.resolvedId==shared&&selected.linkCount==3,
                                  "Cancellation detached the selected hard-link name");
                            Verify(*target,selected.id,3011,13);
                        }
                        Verify(*target,referent,1024);
                    };
                    bool approvedGroup=false,atWrite=false;
                    Storage::Job cancelGroup;
                    cancelGroup.transport([&](const Storage::Progress& progress){
                        if(approvedGroup&&!progress.scanning&&progress.path==u8"relativ\u00e9"){
                            atWrite=true;
                            cancelGroup.cancel();
                        }
                    },[&](const auto&){
                        approvedGroup=true;
                        return Storage::ConflictChoice::Replace;
                    });
                    Completion cancelledGroup;
                    const auto stopped=copyRelative(group,cancelGroup,cancelledGroup);
                    Check(atWrite&&stopped.outcome==Storage::Outcome::Cancelled&&!stopped.partial&&
                          !cancelledGroup.complete,"Expected cancellation before publishing the replacement symlink");
                    target.reset();
                    target=OpenBrowserImage(targetPath);
                    Check(bool(target),"Could not reopen the cancelled hard-link destination");
                    verifyGroup(false);

                    unsigned groupPrompts=0;
                    Storage::Job replaceGroup;
                    replaceGroup.transport({},[&](const auto&){
                        ++groupPrompts;
                        return Storage::ConflictChoice::Replace;
                    });
                    Completion replacedGroup;
                    const auto replaced=copyRelative(group,replaceGroup,replacedGroup);
                    if(replaced.code)throw HfsPlus::Error(replaced.message);
                    Check(replacedGroup.complete&&groupPrompts==1,"Missing hard-link overwrite approval");
                    target.reset();
                    target=OpenBrowserImage(targetPath);
                    Check(bool(target),"Could not reopen the replaced hard-link destination");
                    verifyGroup(true);
                }else{
                    Entry existing;existing.name=hfsRelativeName;existing.symbolicLink=true;
                    existing.posix=oldPosix;
                    Storage::Job seed;
                    target->PutSymbolicLink(destination,existing,oldTarget,seed);
                }
                Storage::Job cancelled;
                bool approved=false;
                bool reachedWrite=false;
                cancelled.transport([&](const Storage::Progress& progress){
                    if(fromApfs&&approved&&progress.path==u8"relativ\u00e9"&&!progress.scanning){
                        reachedWrite=true;
                        cancelled.cancel();
                    }
                },[&](const auto&){
                    approved=true;
                    return fromApfs?Storage::ConflictChoice::Replace:Storage::ConflictChoice::Cancel;
                });
                Completion aborted;
                const auto stopped=copyRelative(destination,cancelled,aborted);
                Entry retained;
                Check(stopped.outcome==Storage::Outcome::Cancelled&&!stopped.partial&&approved&&
                      target->FindChild(destination,hfsRelativeName,retained),"Expected a cancelled symlink overwrite");
                if(fromApfs){
                    Check(reachedWrite&&retained.id==original&&!retained.symbolicLink&&
                          target->GetAttributes(retained.id)==originalAttrs,
                          "Cancelling a symlink overwrite changed the original file or its attributes");
                    Verify(*target,retained.id,4099,11);
                }else{
                    Check(retained.posix==oldPosix&&target->SymbolicLinkTarget(retained.id)==oldTarget,
                          "Cancelled overwrite changed the old symlink or its POSIX metadata");
                }

                Storage::Job job;
                bool prompted=false;
                job.transport({},[&](const auto&){prompted=true;return Storage::ConflictChoice::Replace;});
                Completion completion;
                const auto result=ExecuteInWorker(OrdinaryCopy{{Native(source,folder.id)},Native(target,2)},job,completion);
                if(result.code)throw HfsPlus::Error(result.message);
                Check(completion.complete&&prompted,"Incomplete HFS symlink copy or missing overwrite prompt");
            }
            {
                auto disk=HfsPlus::OpenImage(targetPath.c_str(),false);
                HfsPlus::Volume hfs(*disk);
                HfsPlus::Entry folder,link;
                Check(hfs.FindChild(2,u"Links",folder),"Missing HFS links folder after reopen");
                for(const auto& [name,path,posix]:links){
                    Check(hfs.FindChild(folder.id,name,link)&&link.symbolicLink&&
                          link.forks[0].size==path.size()&&!link.forks[1].size&&link.posix==posix&&
                          !memcmp(link.finder.data(),"slnkrhap",8),"HFS on-disk symlink type, fork length or POSIX fields changed");
                    Check(!hfs.GetAttributes(link.id).count(u"user.original"),
                          "The replacement symlink retained the original file's attributes");
                }
            }
            auto target=OpenBrowserImage(targetPath);
            Entry folder,link,ordinary;
            Check(target&&target->FindChild(2,u"Links",folder)&&target->FindChild(2,u"target",ordinary)&&
                  target->FindChild(folder.id,hfsRelativeName,link),"Missing copied HFS links after reopen");
            verifyLinks(*target,folder);
            Verify(*target,ordinary.id,1024);
            bool refused=false;
            try{target->Read(link.id,0,0,nullptr,0);}
            catch(const HfsPlus::Error&){refused=true;}
            Check(refused,"HFS read treated a symlink as an ordinary file");
            target->Rename(link.id,folder.id,u"renamed link");
            Check(target->FindChild(folder.id,u"renamed link",link)&&link.symbolicLink,
                  "HFS renamed the wrong object");
            target->Remove(link.id);
            Check(!target->FindChild(folder.id,u"renamed link",link),"HFS did not remove the symlink");
            Verify(*target,ordinary.id,1024);
        }
    }
    fprintf(report,"PASS: APFS symlinks replace regular files on HFS+ and both HFSX variants without following targets. Cancellation preserves the original forks and attributes.\n");
    fprintf(report,"PASS: APFS symlink overwrite changes only the selected HFS+ or HFSX hard-link name. After reopening, the other aliases share their original inode and retain both forks. Cancellation preserves all three names.\n");
    fprintf(report,"PASS: APFS, HFS+ and HFSX symlinks reopen on HFS+ and both HFSX variants with unchanged UID, GID, mode, and target. Cancel, rename and delete preserve link metadata or leave the target intact.\n");
}

void TestApfsRegularOverwritesHfsSymlink(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    constexpr uint64_t fileSize=8199,referentSize=4111;
    constexpr BYTE fileBias=17,referentBias=71;
    const PosixMetadata linkPosix{417,52,0751};
    const HfsPlus::Attributes linkAttributes{{u"user.old-link",{9,0,4}}};
    const HfsPlus::Bytes linkTarget{'.','.','/','t','a','r','g','e','t',0};
    const auto sourcePath=Wide(root+"\\apfs-ordinary-overwrite.img");
    const auto fixture=_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE");
    if(fixture){
        Check(CopyFileW(fixture,sourcePath.c_str(),TRUE)!=FALSE,"Could not copy the macOS APFS fixture");
    }else{
        auto disk=HfsPlus::CreateImage(sourcePath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"APFS regular overwrite",true,job);
    }
    auto source=OpenBrowserImage(sourcePath);
    Check(source&&source->Supports().create,"Cannot seed the APFS overwrite fixture");
    const auto file=Put(*source,2,u"Replacement file",fileSize,fileBias);
    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const auto path=Wide(root+"\\hfs-link-to-regular-"+std::to_string(index++)+".dsk");
        {
            auto disk=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"HFS overwrite destination",true,kind);
        }
        auto target=OpenBrowserImage(path);
        Check(target&&VolumeSession::VolumeSessionPidForTest(target),"Cannot mount the HFS overwrite fixture");
        const auto referent=Put(*target,2,u"target",referentSize,referentBias);
        const auto parent=target->MakeDirectory(2,u"Links");
        Entry original;
        original.name=u"Replacement file";
        original.symbolicLink=true;
        original.posix=linkPosix;
        original.attributes=linkAttributes;
        Storage::Job seed;
        target->PutSymbolicLink(parent,original,linkTarget,seed);
        const auto copy=[&](Storage::Job& job,Completion& completion){
            return ExecuteInWorker(OrdinaryCopy{{Native(source,file)},Native(target,parent)},job,completion);
        };
        bool readResource=false;
        unsigned prompts=0;
        Storage::Job cancel;
        cancel.transport([&](const Storage::Progress& progress){
            if(progress.fileBytes>fileSize){readResource=true;cancel.cancel();}
        },[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
        Completion aborted;
        const auto cancelled=copy(cancel,aborted);
        Check(readResource&&prompts==1&&cancelled.outcome==Storage::Outcome::Cancelled&&
              !cancelled.partial&&!aborted.complete,"Regular overwrite did not cancel during the resource fork");
        target.reset();
        target=OpenBrowserImage(path);
        Entry link;
        Check(target&&target->FindChild(parent,original.name,link)&&link.symbolicLink&&link.posix==linkPosix&&
              target->SymbolicLinkTarget(link.id)==linkTarget&&target->GetAttributes(link.id)==linkAttributes,
              "Cancellation changed the original symlink, target bytes or metadata on disk");
        Verify(*target,referent,referentSize,referentBias);

        Storage::Job replace;
        replace.transport({},[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
        Completion done;
        const auto completed=copy(replace,done);
        if(completed.code)throw HfsPlus::Error(completed.message);
        Check(done.complete&&prompts==2,"Missing regular-file overwrite approval");
        target.reset();
        target=OpenBrowserImage(path);
        Entry replacement;
        Check(target&&target->FindChild(parent,original.name,replacement)&&!replacement.symbolicLink&&
              !target->SymbolicLinkTarget(replacement.id)&&UserCopyAttributes(*target,replacement.id).empty(),
              "HFS overwrite did not store a regular file in place of the symbolic link");
        Verify(*target,replacement.id,fileSize,fileBias);
        Verify(*target,referent,referentSize,referentBias);
    }
    fprintf(report,"PASS: Regular APFS files replace symlinks on HFS+ and both HFSX variants. Both forks and the untouched target survive reopening. Cancellation preserves the link and its metadata.\n");
}

void TestHfsToApfsHardLinks(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const size_t sizes[]={5003,7321};
    constexpr auto ordinaryName=u"Ordinary replacement";
    constexpr uint64_t ordinarySize=8599,oldSize=3499;
    const std::u16string survivorNames[][2]={{u"Saved group original",u"Saved group alias"},
                                             {u"Saved ordinary original",u"Saved ordinary alias"}};
    const auto verifySurvivors=[&](Volume& volume,bool replaced){
        for(unsigned i=0;i<2;++i){
            Entry first,second;
            Check(volume.FindChild(2,survivorNames[i][0],first)&&volume.FindChild(2,survivorNames[i][1],second)&&
                  first.hardLink&&second.hardLink&&first.resolvedId==second.resolvedId&&
                  first.linkCount==(replaced?2u:4u-i)&&second.linkCount==first.linkCount,
                  "The surviving APFS aliases did not keep their original shared inode");
            Verify(volume,first.id,oldSize+i,BYTE(41+i));
            Verify(volume,second.id,oldSize+i,BYTE(41+i));
        }
    };
    const auto pattern=[](unsigned group,unsigned fork,uint64_t offset){
        return uint8_t(group*73+fork*31+offset*17+offset/256);
    };
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const char* format=kind==Kind::HfsPlus?"hfsplus":kind==Kind::HfsXCaseSensitive?"hfsx":"hfsx-folded";
        const auto sourcePath=Wide(root+"\\"+format+"-hard-links.dsk");
        const auto targetPath=Wide(root+"\\"+format+"-hard-links-apfs.img");
        const std::u16string names[]={u"First file",u"Second file"};
        {
            auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Hard-link source",true,kind);
            HfsPlus::Volume hfs(*disk,true);
            hfs.Put(2,ordinaryName,ordinarySize,173,[](unsigned fork,uint64_t offset,void* out,size_t count){
                auto bytes=static_cast<uint8_t*>(out);
                for(size_t i=0;i<count;++i)bytes[i]=BYTE((offset+i)*7+fork*13+31);
            });
            const auto folder=hfs.MakeDirectory(2,u"Hard-link transfer");
            const auto nested=hfs.MakeDirectory(folder,u"Aliases");
            for(unsigned group=0;group<2;++group){
                const auto id=hfs.Put(folder,names[group],sizes[0],sizes[1],
                    [&](unsigned fork,uint64_t offset,void* out,size_t count){
                        auto bytes=static_cast<uint8_t*>(out);
                        for(size_t i=0;i<count;++i)bytes[i]=pattern(group,fork,offset+i);
                    });
                hfs.MakeHardLink(id,nested,names[group]);
            }
        }
        const auto external=kind==Kind::HfsXCaseSensitive?_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE"):nullptr;
        if(external){
            Check(CopyFileW(external,targetPath.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
        }else{
            auto disk=HfsPlus::CreateImage(targetPath.c_str(),32*1024*1024);
            Storage::Job job;
            Apfs::Format(*disk,u"Hard-link destination",true,job);
        }
        {
            auto source=OpenBrowserImage(sourcePath);
            auto target=OpenBrowserImage(targetPath);
            Entry folder;
            Check(source&&VolumeSession::VolumeSessionPidForTest(source)&&target&&
                  source->FindChild(2,u"Hard-link transfer",folder),"Unable to mount hard-link copy fixtures");
            const auto existingFolder=target->MakeDirectory(2,u"Hard-link transfer");
            const auto existingNested=target->MakeDirectory(existingFolder,u"Aliases");
            const auto oldGroup=Put(*target,existingNested,names[0],oldSize,41);
            target->MakeHardLink(oldGroup,existingFolder,names[0]);
            for(const auto& name:survivorNames[0])target->MakeHardLink(oldGroup,2,name);
            const auto oldOrdinary=Put(*target,2,survivorNames[1][0],oldSize+1,42);
            target->MakeHardLink(oldOrdinary,2,survivorNames[1][1]);
            target->MakeHardLink(oldOrdinary,existingFolder,ordinaryName);

            Entry ordinary,sourceNested,groupMember;
            Check(source->FindChild(2,ordinaryName,ordinary)&&
                  source->FindChild(folder.id,u"Aliases",sourceNested)&&
                  source->FindChild(sourceNested.id,names[0],groupMember)&&groupMember.hardLink,
                  "Missing HFS overwrite sources");
            const auto cancelOverwrite=[&](const Entry& from,uint32_t parent){
                bool resourceStarted=false;
                unsigned prompts=0;
                Storage::Job cancel;
                cancel.transport([&](const Storage::Progress& progress){
                    if(progress.fileBytes>from.forks[0].size){
                        resourceStarted=true;
                        cancel.cancel();
                    }
                },[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
                Completion aborted;
                const auto result=ExecuteInWorker(OrdinaryCopy{{Native(source,from.id)},Native(target,parent)},cancel,aborted);
                Check(resourceStarted&&prompts==1&&result.outcome==Storage::Outcome::Cancelled&&
                      !result.partial&&!aborted.complete,"Expected cancellation before replacing an APFS alias");
            };
            cancelOverwrite(ordinary,existingFolder);
            cancelOverwrite(groupMember,existingNested);
            target.reset();
            target=OpenBrowserImage(targetPath);
            Check(bool(target),"Could not reopen the APFS image after cancellation");
            Entry retainedFolder,retainedNested,retainedOrdinary,retainedMember,retainedAlias;
            Check(target->FindChild(2,u"Hard-link transfer",retainedFolder)&&
                  target->FindChild(retainedFolder.id,u"Aliases",retainedNested)&&
                  target->FindChild(retainedFolder.id,ordinaryName,retainedOrdinary)&&retainedOrdinary.linkCount==3&&
                  target->FindChild(retainedNested.id,names[0],retainedMember)&&retainedMember.linkCount==4&&
                  target->FindChild(retainedFolder.id,names[0],retainedAlias)&&retainedAlias.resolvedId==retainedMember.resolvedId,
                  "Cancellation detached an APFS destination alias");
            Verify(*target,retainedOrdinary.id,oldSize+1,42);
            Verify(*target,retainedMember.id,oldSize,41);
            Verify(*target,retainedAlias.id,oldSize,41);
            verifySurvivors(*target,false);

            // Use the reopened folder IDs. APFS row IDs only live for one mount.
            unsigned ordinaryPrompts=0;
            Storage::Job replace;
            replace.transport({},[&](const auto&){++ordinaryPrompts;return Storage::ConflictChoice::Replace;});
            Completion overwritten;
            const auto result=ExecuteInWorker(OrdinaryCopy{{Native(source,ordinary.id)},Native(target,retainedFolder.id)},
                                               replace,overwritten);
            if(result.code)throw HfsPlus::Error(result.message);
            Check(overwritten.complete&&ordinaryPrompts==1,"Incomplete HFS regular-file overwrite");

            unsigned approved=0;
            Storage::Job job;
            job.transport({},[&](const auto&){
                ++approved;
                return Storage::ConflictChoice::Replace;
            });
            Completion completion;
            const auto copied=ExecuteInWorker(OrdinaryCopy{{Native(source,folder.id)},Native(target,2)},job,completion);
            if(copied.code)throw HfsPlus::Error(copied.message);
            Check(completion.complete&&approved==2,"Incomplete HFS to APFS hard-link copy or missing overwrite prompt");
        }
        auto target=OpenBrowserImage(targetPath);
        Entry folder,nested;
        Check(target&&target->FindChild(2,u"Hard-link transfer",folder)&&target->FindChild(folder.id,u"Aliases",nested),
              "Missing copied hard-link folders");
        Check(external||target->Writable(),"Selected-name replacement lost native APFS write access");
        verifySurvivors(*target,true);
        Entry ordinary,survivor;
        Check(target->FindChild(folder.id,ordinaryName,ordinary)&&!ordinary.hardLink&&
              target->FindChild(2,survivorNames[1][0],survivor)&&ordinary.resolvedId!=survivor.resolvedId,
              "The regular file did not replace only the selected APFS name");
        Check(ordinary.forks[0].size==ordinarySize&&ordinary.forks[1].size==173,
              "The regular replacement lost a fork");
        for(unsigned fork=0;fork<2;++fork){
            HfsPlus::Bytes bytes(size_t(ordinary.forks[fork].size));
            target->Read(ordinary.id,fork,0,bytes.data(),bytes.size());
            for(size_t i=0;i<bytes.size();++i)Check(bytes[i]==BYTE(i*7+fork*13+31),"Regular replacement fork changed");
        }
        std::array<uint32_t,2> inodes{};
        for(unsigned group=0;group<2;++group){
            Entry original,alias;
            Check(target->FindChild(folder.id,names[group],original)&&target->FindChild(nested.id,names[group],alias)&&
                  original.id!=alias.id&&original.hardLink&&alias.hardLink&&original.linkCount==2&&alias.linkCount==2&&
                  original.resolvedId&&original.resolvedId==alias.resolvedId,"Copied APFS names did not share one inode");
            inodes[group]=original.resolvedId;
            for(const auto* entry:{&original,&alias}){
                for(unsigned fork=0;fork<2;++fork){
                    Check(entry->forks[fork].size==sizes[fork],"Copied APFS hard link lost a fork");
                    HfsPlus::Bytes actual(sizes[fork]),expected(sizes[fork]);
                    target->Read(entry->id,fork,0,actual.data(),actual.size());
                    for(size_t i=0;i<expected.size();++i)expected[i]=pattern(group,fork,i);
                    Check(actual==expected,"Copied APFS hard-link fork bytes differ");
                }
            }
        }
        Check(inodes[0]!=inodes[1],"Independent HFS hard-link groups merged on APFS");
        if(kind==Kind::HfsPlus){
            Entry original,approved,current;
            Check(target->FindChild(folder.id,names[0],original),"Missing APFS hard-link source");
            Put(*target,folder.id,u"Victim",512);
            Check(target->FindChild(folder.id,u"Victim",approved),"Missing approved APFS link destination");
            target->Remove(approved.id);
            Put(*target,folder.id,u"Victim",1024);
            bool rejected=false;
            Storage::Job job;
            try{
                target->MakeCopiedHardLinkForTransfer(original,folder.id,u"Victim",&approved,job);
            }catch(const HfsPlus::Error& error){
                rejected=std::string(error.what()).find("changed after overwrite approval")!=std::string::npos;
            }
            Check(rejected&&target->FindChild(folder.id,u"Victim",current)&&current.forks[0].size==1024&&
                  current.linkCount==1&&current.resolvedId!=original.resolvedId,
                  "A stale hard-link overwrite changed the new destination");
            Verify(*target,current.id,1024);
        }
        if(external)fprintf(report,"PASS: HFSX hard-link groups and forks reopen on the shared macOS APFS fixture.\n");
    }
    fprintf(report,"PASS: HFS+ and both HFSX variants copy separate hard-link groups to APFS, sharing one inode per group and preserving both forks across folders and reopen.\n");
    fprintf(report,"PASS: HFS+/HFSX regular files and hard-link groups replace only selected APFS names. Cancellation preserves the old group and both forks of surviving aliases stay intact after reopening.\n");
}

void TestHfsToApfsOverwriteTypes(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const std::u16string symlinks[]={u"Link over file",u"Link over alias",u"Link over link"};
    const std::u16string names[]={u"Group first",u"Group second",u"Group third"};
    constexpr auto ordinary=u"File over link";
    constexpr auto referent=u"Untouched target";
    constexpr auto survivor=u"Surviving destination alias";
    constexpr uint64_t regularSize=5309,groupSize=7313;
    const PosixMetadata linkMode{451,81,0711};
    const std::string freshPath="/Untouched target",oldPath="Untouched target";
    const HfsPlus::Bytes freshTarget(freshPath.c_str(),freshPath.c_str()+freshPath.size()+1);
    const HfsPlus::Bytes oldTarget(oldPath.c_str(),oldPath.c_str()+oldPath.size()+1);
    const auto makeSymlink=[&](Volume& volume,const std::u16string& name,bool incoming){
        Entry link;
        link.name=name;
        link.symbolicLink=true;
        if(incoming)link.posix=linkMode;
        Storage::Job job;
        volume.PutSymbolicLink(2,link,incoming?freshTarget:oldTarget,job);
    };
    const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const auto base=root+"\\apfs-overwrite-matrix-"+std::to_string(index++);
        const auto sourcePath=Wide(base+".dsk");
        {
            auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Overwrite sources",true,kind);
        }
        auto source=OpenBrowserImage(sourcePath);
        Check(source&&VolumeSession::VolumeSessionPidForTest(source),"Could not mount HFS overwrite source");
        for(const auto& name:symlinks)makeSymlink(*source,name,true);
        const auto file=Put(*source,2,ordinary,regularSize,17);
        const auto group=Put(*source,2,names[0],groupSize,22);
        const auto firstAlias=source->MakeHardLink(group,2,names[1]);
        source->MakeHardLink(firstAlias,2,names[2]);
        const auto input=[&](const std::u16string& name){
            Entry entry;
            Check(source->FindChild(2,name,entry),"Missing HFS overwrite input");
            return Native(source,entry.id);
        };
        std::vector<Location> inputs;
        for(const auto& name:symlinks)inputs.push_back(input(name));
        inputs.push_back(Native(source,file));
        for(const auto& name:names)inputs.push_back(input(name));

        for(const bool external:{false,true}){
            if(external&&!fixture)continue;
            const auto path=Wide(base+(external?"-macos.img":"-native.img"));
            if(external){
                Check(CopyFileW(fixture,path.c_str(),TRUE)!=FALSE,"Could not copy the macOS APFS fixture");
            }else{
                auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
                Storage::Job job;
                Apfs::Format(*disk,u"Overwrite destination",false,job);
            }
            auto target=OpenBrowserImage(path);
            Check(target&&target->Supports().create&&target->Writable()!=external,"Wrong APFS writer for overwrite test");
            Put(*target,2,referent,3203,97);
            Put(*target,2,symlinks[0],909,31);
            const auto linked=Put(*target,2,symlinks[1],1109,77);
            target->MakeHardLink(linked,2,survivor);
            for(const auto name:{symlinks[2],std::u16string(ordinary),names[0],names[1]})
                makeSymlink(*target,name,false);
            Put(*target,2,names[2],1801,41);
            const auto copy=[&](const std::vector<Location>& files,Storage::Job& job,Completion& completion){
                return ExecuteInWorker(OrdinaryCopy{files,Native(target,2)},job,completion);
            };
            unsigned prompts=0;
            bool startedResource=false;
            Storage::Job cancelFile;
            cancelFile.transport([&](const Storage::Progress& progress){
                if(progress.fileBytes>regularSize){
                    startedResource=true;
                    cancelFile.cancel();
                }
            },[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
            Completion aborted;
            const auto cancelled=copy({Native(source,file)},cancelFile,aborted);
            Check(startedResource&&prompts==1&&cancelled.outcome==Storage::Outcome::Cancelled&&
                  !cancelled.partial,"File over symlink did not cancel during its resource fork");
            Storage::Job cancelLink;
            cancelLink.transport({},[&](const auto&){++prompts;return Storage::ConflictChoice::Cancel;});
            Completion refused;
            Check(copy({inputs[1]},cancelLink,refused).outcome==Storage::Outcome::Cancelled&&prompts==2,
                  "Symlink over a hard-link name did not honor Cancel");
            target.reset();
            target=OpenBrowserImage(path);
            Entry originalLink,originalAlias,oldPeer;
            Check(target&&target->FindChild(2,ordinary,originalLink)&&
                  target->SymbolicLinkTarget(originalLink.id)==oldTarget&&
                  target->FindChild(2,symlinks[1],originalAlias)&&target->FindChild(2,survivor,oldPeer)&&
                  originalAlias.hardLink&&originalAlias.linkCount==2&&originalAlias.resolvedId==oldPeer.resolvedId,
                  "Cancellation changed an APFS symlink or hard-link group");
            Verify(*target,originalAlias.id,1109,77);

            prompts=0;
            Storage::Job replace;
            replace.transport({},[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
            Completion done;
            const auto result=copy(inputs,replace,done);
            if(result.code)throw HfsPlus::Error(result.message);
            Check(done.complete&&prompts==inputs.size(),"Incomplete APFS overwrite matrix");
            target.reset();
            target=OpenBrowserImage(path);
            Check(target&&target->Supports().create&&target->Writable()!=external,
                  "Overwrites changed APFS writer eligibility after reopening");
            for(const auto& name:symlinks){
                Entry link;
                Check(target->FindChild(2,name,link)&&link.symbolicLink&&!link.hardLink&&link.posix==linkMode&&
                      target->SymbolicLinkTarget(link.id)==freshTarget,"APFS overwrite changed the incoming symlink");
            }
            Entry ordinaryCopy,untouched,peer;
            Check(target->FindChild(2,ordinary,ordinaryCopy)&&!ordinaryCopy.symbolicLink&&!ordinaryCopy.hardLink&&
                  target->FindChild(2,referent,untouched)&&target->FindChild(2,survivor,peer)&&!peer.hardLink,
                  "An APFS overwrite lost an ordinary file or an unrelated name");
            Verify(*target,ordinaryCopy.id,regularSize,17);
            Verify(*target,untouched.id,3203,97);
            Verify(*target,peer.id,1109,77);
            uint32_t inode=0;
            for(const auto& name:names){
                Entry alias;
                Check(target->FindChild(2,name,alias)&&alias.hardLink&&!alias.symbolicLink&&
                      alias.linkCount==3&&(!inode||alias.resolvedId==inode),
                      "The APFS overwrite did not recreate one shared hard-link group");
                inode=alias.resolvedId;
                Verify(*target,alias.id,groupSize,22);
            }
        }
    }
    fprintf(report,"PASS: HFS+, case-sensitive HFSX and case-insensitive HFSX exercise regular-file, symlink and hard-link overwrites on both APFS writers when the shared macOS fixture is available. Cancellation and reopening preserve both forks, symlink targets and surviving aliases.\n");
}

void TestApfsToHfsHardLinks(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const auto sourcePath=Wide(root+"\\apfs-hard-link-source.img");
    const auto fixture=_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE");
    if(fixture){
        Check(CopyFileW(fixture,sourcePath.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
    }else{
        auto disk=HfsPlus::CreateImage(sourcePath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Hard-link source",true,job);
    }
    constexpr auto overwriteName=u"Ordinary overwrite";
    constexpr uint64_t replacementSize=4099,originalSize=3203;
    constexpr BYTE replacementBias=93,originalBias=117;
    const auto verifyOverwrite=[&](Volume& volume,bool replaced){
        Entry folder,selected,firstAlias,secondAlias;
        Check(volume.FindChild(2,u"Copied groups",folder)&&
              volume.FindChild(folder.id,overwriteName,selected)&&
              volume.FindChild(2,u"Retained first alias",firstAlias)&&
              volume.FindChild(2,u"Retained second alias",secondAlias),
              "Missing selected HFS name or its surviving aliases");
        Check(firstAlias.hardLink&&secondAlias.hardLink&&firstAlias.resolvedId==secondAlias.resolvedId&&
              firstAlias.linkCount==(replaced?2u:3u)&&secondAlias.linkCount==firstAlias.linkCount,
              "HFS overwrite changed the inode shared by surviving names");
        Check(replaced?(!selected.hardLink&&selected.resolvedId!=firstAlias.resolvedId):
                       (selected.hardLink&&selected.resolvedId==firstAlias.resolvedId&&selected.linkCount==3),
              "HFS overwrite did not isolate the selected name");
        Verify(volume,firstAlias.id,originalSize,originalBias);
        Verify(volume,secondAlias.id,originalSize,originalBias);
        Verify(volume,selected.id,replaced?replacementSize:originalSize,replaced?replacementBias:originalBias);
    };
    const size_t sizes[]={5003,7321};
    const std::u16string names[][3]={{u"Alpha",u"Beta",u"Gamma"},{u"Delta",u"\u00c9psilon",u"Zeta"}};
    const std::array<std::u16string,3> referentNames{u"Target A",u"Target B",u"Target C"};
    const std::array<std::string,3> paths{"../Target A","/Target B","../Target C"};
    const auto linkTarget=[&](size_t index){
        HfsPlus::Bytes bytes(paths[index].begin(),paths[index].end());
        bytes.push_back(0);
        return bytes;
    };
    const auto verifyReferents=[&](Volume& volume){
        for(size_t index=0;index<referentNames.size();++index){
            Entry referent;
            Check(volume.FindChild(2,referentNames[index],referent)&&!referent.symbolicLink,
                  "An HFS symlink referent was removed or replaced");
            Verify(volume,referent.id,5111+index,BYTE(61+index));
        }
    };
    const auto pattern=[](unsigned group,unsigned fork,uint64_t at){
        return uint8_t(group*53+fork*71+at*17+(at>>8));
    };
    {
        auto volume=OpenBrowserImage(sourcePath);
        Check(volume&&volume->Supports().CanCreateHardLinks(),"Could not seed the APFS link fixture");
        Put(*volume,2,overwriteName,replacementSize,replacementBias);
        const auto a=volume->MakeDirectory(2,u"APFS Originals");
        const auto b=volume->MakeDirectory(2,u"APFS Aliases");
        for(unsigned group=0;group<2;++group){
            Entry file;file.name=names[group][0];file.forks[0].size=sizes[0];file.forks[1].size=sizes[1];
            const auto id=volume->Put(a,file,[&](unsigned fork,uint64_t at,void* out,size_t size){
                auto bytes=static_cast<uint8_t*>(out);
                for(size_t i=0;i<size;++i)bytes[i]=pattern(group,fork,at+i);
            },false);
            volume->MakeHardLink(id,b,names[group][1]);
            volume->MakeHardLink(id,b,names[group][2]);
        }
    }
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const char* format=kind==Kind::HfsPlus?"plus":kind==Kind::HfsXCaseSensitive?"x":"folded-x";
        const auto targetPath=Wide(root+"\\apfs-hard-links-to-"+format+".dsk");
        {
            auto disk=HfsPlus::CreateImage(targetPath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Hard-link destination",true,kind);
        }
        {
            auto source=OpenBrowserImage(sourcePath);
            auto target=OpenBrowserImage(targetPath);
            Entry a,b;
            Check(source&&target&&VolumeSession::VolumeSessionPidForTest(target)&&
                  source->FindChild(2,u"APFS Originals",a)&&source->FindChild(2,u"APFS Aliases",b),
                  "Missing mounted hard-link copy fixtures");
            const auto folder=target->MakeDirectory(2,u"Copied groups");
            const auto victim=Put(*target,folder,names[0][0],513,145);
            target->MakeHardLink(victim,2,u"Unrelated alias");
            for(size_t index=0;index<referentNames.size();++index){
                Put(*target,2,referentNames[index],5111+index,BYTE(61+index));
                Entry link;
                link.name=names[1][index];
                link.symbolicLink=true;
                Storage::Job seed;
                target->PutSymbolicLink(folder,link,linkTarget(index),seed);
            }
            const auto original=Put(*target,folder,overwriteName,originalSize,originalBias);
            const auto firstAlias=target->MakeHardLink(original,2,u"Retained first alias");
            target->MakeHardLink(firstAlias,2,u"Retained second alias");
            Entry replacement;
            Check(source->FindChild(2,overwriteName,replacement)&&!replacement.hardLink,
                  "Missing ordinary APFS replacement file");
            const auto overwrite=[&](Storage::Job& job,Completion& completion){
                return ExecuteInWorker(OrdinaryCopy{{Native(source,replacement.id)},Native(target,folder)},job,completion);
            };
            const auto worker=VolumeSession::VolumeSessionPidForTest(target);
            bool readResource=false;
            Storage::Job cancellation;
            cancellation.transport([&](const Storage::Progress& progress){
                if(progress.fileBytes>replacementSize){
                    readResource=true;
                    cancellation.cancel();
                }
            },[](const auto&){return Storage::ConflictChoice::Replace;});
            Completion aborted;
            const auto cancelled=overwrite(cancellation,aborted);
            Check(readResource&&cancelled.outcome==Storage::Outcome::Cancelled&&!cancelled.partial&&
                  !aborted.complete&&VolumeSession::VolumeSessionPidForTest(target)==worker,
                  "Cancelling selected-name replacement did not retain the original group and worker");
            verifyOverwrite(*target,false);

            unsigned overwritePrompts=0;
            Storage::Job replace;
            replace.transport({},[&](const auto&){++overwritePrompts;return Storage::ConflictChoice::Replace;});
            Completion overwritten;
            const auto replacementResult=overwrite(replace,overwritten);
            if(replacementResult.code)throw HfsPlus::Error(replacementResult.message);
            Check(overwritten.complete&&overwritePrompts==1,"Missing selected-name overwrite approval");

            std::vector<Location> selected;
            for(unsigned group=0;group<2;++group){
                uint32_t inode=0;
                for(unsigned alias=0;alias<3;++alias){
                    Entry entry;
                    Check(source->FindChild(alias?b.id:a.id,names[group][alias],entry)&&entry.hardLink&&entry.linkCount==3,
                          "Missing APFS hard-link group member");
                    if(!alias)inode=entry.resolvedId;
                    Check(inode&&entry.resolvedId==inode,"The APFS source names do not share an inode");
                    selected.push_back(Native(source,entry.id));
                }
            }
            const std::vector<Location> overSymlinks(selected.begin()+3,selected.end());
            bool resourceStarted=false;
            Storage::Job cancelGroup;
            cancelGroup.transport([&](const Storage::Progress& progress){
                if(progress.fileBytes>sizes[0]){
                    resourceStarted=true;
                    cancelGroup.cancel();
                }
            },[](const auto&){return Storage::ConflictChoice::Replace;});
            Completion cancelledGroup;
            const auto interrupted=ExecuteInWorker(OrdinaryCopy{overSymlinks,Native(target,folder)},
                                                   cancelGroup,cancelledGroup);
            Check(resourceStarted&&interrupted.outcome==Storage::Outcome::Cancelled&&!interrupted.partial&&
                  !cancelledGroup.complete,"Expected cancellation before publishing the first copied group member");
            target.reset();
            target=OpenBrowserImage(targetPath);
            Check(bool(target),"Could not reopen the cancelled HFS destination");
            for(size_t index=0;index<paths.size();++index){
                Entry retained;
                const auto name=HfsPlus::Normalize(names[1][index],target->CaseSensitive());
                Check(target->FindChild(folder,name,retained)&&retained.symbolicLink&&
                      target->SymbolicLinkTarget(retained.id)==linkTarget(index),
                      "Cancelling the group copy changed a destination symlink");
            }
            verifyReferents(*target);

            unsigned prompts=0;
            Storage::Job job;
            job.transport({},[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
            Completion completion;
            const auto result=ExecuteInWorker(OrdinaryCopy{std::move(selected),Native(target,folder)},job,completion);
            if(result.code)throw HfsPlus::Error(result.message);
            Check(completion.complete&&prompts==4,"Incomplete APFS hard-link copy or missing approval");
        }
        auto target=OpenBrowserImage(targetPath);
        Entry folder,unrelated;
        Check(target&&target->FindChild(2,u"Copied groups",folder)&&target->FindChild(2,u"Unrelated alias",unrelated),
              "Missing reopened HFS link fixtures");
        Verify(*target,unrelated.id,513,145);
        verifyOverwrite(*target,true);
        verifyReferents(*target);
        std::array<uint32_t,2> inodes{};
        for(unsigned group=0;group<2;++group){
            std::set<uint32_t> namesSeen;
            for(const auto& name:names[group]){
                Entry entry;
                const auto normalized=HfsPlus::Normalize(name,target->CaseSensitive());
                Check(target->FindChild(folder.id,normalized,entry)&&!entry.symbolicLink&&entry.hardLink&&
                      entry.linkCount==3&&namesSeen.insert(entry.id).second,
                      "HFS did not reopen all three hard-link names");
                if(!inodes[group])inodes[group]=entry.resolvedId;
                Check(inodes[group]&&entry.resolvedId==inodes[group]&&entry.resolvedId!=unrelated.resolvedId,
                      "Copied HFS aliases did not share a new inode");
                for(unsigned fork=0;fork<2;++fork){
                    Check(entry.forks[fork].size==sizes[fork],"Copied HFS hard link lost a fork");
                    HfsPlus::Bytes actual(sizes[fork]),expected(sizes[fork]);
                    target->Read(entry.id,fork,0,actual.data(),actual.size());
                    for(size_t i=0;i<expected.size();++i)expected[i]=pattern(group,fork,i);
                    Check(actual==expected,"Copied HFS hard-link fork bytes differ");
                }
            }
        }
        Check(inodes[0]!=inodes[1],"Independent APFS hard-link groups merged on HFS");
    }
    fprintf(report,"PASS: ordinary APFS overwrite replaces only one HFS+ or HFSX name; cancellation and reopening preserve both forks of its other aliases.\n");
    if(fixture)fprintf(report,"PASS: hard-link copies from a disposable macOS-created APFS image.\n");
    fprintf(report,"PASS: APFS copies into HFS+ and both HFSX variants share one inode per link group and preserve both forks after reopening. An overwritten name is detached from unrelated aliases.\n");
    fprintf(report,"PASS: APFS hard-link groups replace HFS+ and HFSX symlinks, preserve both forks and one shared inode, and leave all referents untouched. Cancellation during the first resource fork preserves every original symlink.\n");
}

void TestLateHardLinkOverwriteCancellation(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const std::u16string names[]={u"First overwrite",u"Second overwrite",u"Third overwrite"};
    constexpr auto survivor=u"Unselected destination alias";
    constexpr uint64_t sourceSize=8191,oldSize=6311;
    constexpr BYTE sourceBias=23,oldBias=81;
    const auto macosFixture=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    const auto format=[&](const std::wstring& path,bool apfs,Kind kind,bool compatibility){
        if(compatibility){
            Check(CopyFileW(macosFixture,path.c_str(),TRUE)!=FALSE,"Could not copy the macOS APFS fixture");
            return;
        }
        auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
        if(apfs){
            Storage::Job formatJob;
            Apfs::Format(*disk,u"Late cancellation",false,formatJob);
        }else{
            HfsPlus::Volume::Format(*disk,u"Late cancellation",true,kind);
        }
    };
    const auto seed=[&](Volume& volume,uint64_t size,BYTE bias){
        auto member=Put(volume,2,names[0],size,bias);
        for(size_t i=1;i<ARRAYSIZE(names);++i)member=volume.MakeHardLink(member,2,names[i]);
        return member;
    };
    unsigned index=0;
    for(const bool apfsSource:{true,false}){
        for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
            const auto base=root+"\\late-link-cancel-"+std::to_string(index++);
            const auto sourcePath=Wide(base+"-source.img");
            format(sourcePath,apfsSource,kind,false);
            {
                auto source=OpenBrowserImage(sourcePath);
                Check(source&&source->Writable()&&source->Supports().CanCreateHardLinks(),"Source cannot participate in cut/paste");
                seed(*source,sourceSize,sourceBias);
            }
            const bool compatibility=!apfsSource&&kind==Kind::HfsXCaseSensitive&&macosFixture;
            for(const bool cut:{false,true}){
                const auto targetPath=Wide(base+(cut?"-cut.img":"-copy.img"));
                format(targetPath,!apfsSource,kind,compatibility);
                Storage::Result result;
                Completion completion;
                unsigned prompts=0;
                bool firstPrompt=false,secondPrompt=false;
                Storage::Progress pending;
                {
                    auto source=OpenBrowserImage(sourcePath);
                    auto target=OpenBrowserImage(targetPath);
                    Check(source&&target&&target->Supports().CanCreateHardLinks(),"Cannot mount overwrite fixtures");
                    if(!apfsSource)Check(target->Writable()!=compatibility,"Wrong APFS writer for cancellation test");
                    const auto member=seed(*target,oldSize,oldBias);
                    target->MakeHardLink(member,2,survivor);
                    std::vector<Location> selected;
                    for(const auto& name:names){
                        Entry item;
                        Check(source->FindChild(2,name,item)&&item.hardLink&&item.linkCount==3,
                              "Missing source hard-link name before transfer");
                        auto location=Native(source,item.id);
                        location.cut=cut;
                        selected.push_back(std::move(location));
                    }
                    Storage::Job job;
                    job.transport({},[&](const std::string& label){
                        ++prompts;
                        if(prompts==1){
                            firstPrompt=label.find("First overwrite")==0;
                            return Storage::ConflictChoice::Replace;
                        }
                        secondPrompt=label.find("Second overwrite")==0;
                        pending=job.progress();
                        return Storage::ConflictChoice::Cancel;
                    });
                    result=ExecuteInWorker(OrdinaryCopy{selected,Native(target,2)},job,completion);
                }
                auto source=OpenBrowserImage(sourcePath);
                auto target=OpenBrowserImage(targetPath);
                Check(source&&target,"Could not reopen images after cancellation");
                Check(prompts==2&&firstPrompt&&secondPrompt&&pending.files==1&&pending.totalFiles==3&&
                      pending.processed==sourceSize+173,"Cancellation did not stop at the second overwrite");
                Check(result.outcome==Storage::Outcome::Cancelled&&result.partial&&result.completed==1&&
                      result.skipped==0&&!result.moved&&!completion.complete,
                      "Cancellation failed to report the one completed overwrite");

                Entry preserved;
                Check(target->FindChild(2,survivor,preserved)&&preserved.hardLink&&preserved.linkCount==3,
                      "The untouched destination alias lost its original inode");
                Verify(*target,preserved.id,oldSize,oldBias);
                uint32_t sourceInode=0;
                for(size_t i=0;i<ARRAYSIZE(names);++i){
                    Entry original,published;
                    Check(source->FindChild(2,names[i],original)&&original.hardLink&&original.linkCount==3&&
                          (!sourceInode||original.resolvedId==sourceInode),"A cancelled transfer changed the source group");
                    sourceInode=original.resolvedId;
                    Verify(*source,original.id,sourceSize,sourceBias);
                    Check(target->FindChild(2,names[i],published),"A destination name disappeared on cancellation");
                    if(i==0){
                        Check(!published.hardLink&&published.resolvedId!=preserved.resolvedId,
                              "The completed overwrite changed the shared destination inode");
                        Verify(*target,published.id,sourceSize,sourceBias);
                    }else{
                        Check(published.hardLink&&published.linkCount==3&&published.resolvedId==preserved.resolvedId,
                              "A pending overwrite detached a destination alias");
                        Verify(*target,published.id,oldSize,oldBias);
                    }
                }
            }
        }
    }
    fprintf(report,"PASS: cancellation before the second hard-link overwrite reports one completed file on copy and cut/paste in both APFS/HFS+/HFSX directions. Reopening confirms source retention, original alias sharing and exact data and resource forks.\n");
}

void TestApfsToHfsNameCollisions(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const auto sourcePath=Wide(root+"\\collision-source.img");
    {
        auto disk=HfsPlus::CreateImage(sourcePath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Case-sensitive source",true,job);
    }
    auto source=OpenBrowserImage(sourcePath);
    Check(source&&source->CaseSensitive(),"Missing case-sensitive APFS source");
    const auto first=Put(*source,2,u"Readme",4097,3);
    const auto second=Put(*source,2,u"README",4111,11);
    const auto folder=source->MakeDirectory(2,u"Nested");
    const auto nested=source->MakeDirectory(folder,u"Notes folder");
    const auto plain=Put(*source,nested,u"Notes",919,19);
    const auto ignorable=Put(*source,nested,u"Notes\u200c",929,29);
    // Separate source folders can contain equivalent spellings even when
    // APFS would reject them as siblings. Both will land in the same folder.
    const auto composed=Put(*source,folder,u"Caf\u00e9",1001,31);
    const auto decomposed=Put(*source,nested,u"Cafe\u0301",1009,37);
    Check(source->Stat(composed).name!=source->Stat(decomposed).name,
          "APFS fixture did not store distinct Unicode spellings");

    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseInsensitive,Kind::HfsXCaseSensitive}){
        const auto path=Wide(root+"\\collision-destination-"+std::to_string(index++)+".dsk");
        {
            auto disk=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Collision destination",true,kind);
        }
        const bool sensitive=kind==Kind::HfsXCaseSensitive;
        auto target=OpenBrowserImage(path);
        Check(target&&VolumeSession::VolumeSessionPidForTest(target)&&target->CaseSensitive()==sensitive,
              "Wrong HFS+/HFSX destination comparison mode");
        const auto existing=Put(*target,2,u"Readme",3001,41);
        const auto before=target->List().size();
        const auto copy=[&](const std::vector<Location>& inputs,bool expectCollision,unsigned expectedPrompts){
            unsigned prompts=0;
            Storage::Job job;
            job.transport({},[&](const auto&){
                ++prompts;
                return Storage::ConflictChoice::ReplaceAll;
            });
            Completion completion;
            const auto result=ExecuteInWorker(OrdinaryCopy{inputs,Native(target,2)},job,completion);
            if(expectCollision){
                Check(result.code&&!completion.complete&&
                      result.message.find("Source names collide")!=std::string::npos,
                      "Copy did not report colliding HFS+/HFSX destination names");
            }else if(result.code||!completion.complete){
                throw HfsPlus::Error("Copy of distinct destination names failed: "+result.message);
            }
            Check(prompts==expectedPrompts,"A collision reached overwrite handling, or ordinary overwrite was bypassed");
        };
        copy({Native(source,composed),Native(source,decomposed)},true,0);
        Entry duplicate;
        Check(!target->FindChild(2,u"Cafe\u0301",duplicate),
              "A Unicode-normalization collision published a destination file");
        const std::vector<Location> selected{Native(source,first),Native(source,second)};
        copy(selected,!sensitive,sensitive?1:0);
        if(!sensitive){
            Verify(*target,existing,3001,41);
            // Cut/paste uses the same preflight before removing any source entries.
            auto moving=selected;
            for(auto& entry:moving)entry.cut=true;
            copy(moving,true,0);
            Verify(*source,first,4097,3);
            Verify(*source,second,4111,11);
        }
        copy({Native(source,folder)},!sensitive,0);
        target.reset();
        target=OpenImage(path);
        Check(bool(target),"Could not reopen HFS collision destination");
        Entry readme,upper,outer,inner,note,joined;
        Check(target->FindChild(2,u"Readme",readme),"Missing destination file after reopening");
        if(sensitive){
            Check(target->FindChild(2,u"README",upper)&&upper.id!=readme.id&&
                  target->FindChild(2,u"Nested",outer)&&target->FindChild(outer.id,u"Notes folder",inner)&&
                  target->FindChild(inner.id,u"Notes",note)&&target->FindChild(inner.id,u"Notes\u200c",joined)&&note.id!=joined.id,
                  "Case-sensitive HFSX lost one of the distinct source names");
            Verify(*target,readme.id,4097,3);
            Verify(*target,upper.id,4111,11);
            Verify(*target,note.id,919,19);
            Verify(*target,joined.id,929,29);
        }else{
            Verify(*target,readme.id,3001,41);
            Check(!target->FindChild(2,u"Nested",outer)&&target->List().size()==before,
                  "A rejected copy published files or folders");
            copy({Native(source,folder)},true,0); // In-process destination uses the same collation.
        }
    }
    Verify(*source,plain,919,19);
    Verify(*source,ignorable,929,29);
    fprintf(report,"PASS: HFS+ and case-insensitive HFSX reject APFS name collisions before Replace All, including nested ignorable characters and cut/paste. Both HFSX modes reject canonically equivalent names; case-sensitive HFSX preserves case-distinct names.\n");
}

void TestCopySpecialFiles(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const auto hfsPath=Wide(root+"\\named-pipe-source.dsk");
    const std::u16string names[]={u"Input pipe",u"Character node",u"Block node",u"Socket",u"Whiteout"};
    const SpecialFileType types[]={SpecialFileType::Fifo,SpecialFileType::CharacterDevice,
                                   SpecialFileType::BlockDevice,SpecialFileType::Socket,SpecialFileType::Whiteout};
    const auto metadata=[&](size_t index){
        const auto type=types[index];
        return PosixMetadata{123,456,0620,0,PosixMetadata::NoDump,type,
                             IsDevice(type)?0x0e000040u+uint32_t(index):0u};
    };
    const HfsPlus::Attributes attributes{{u"user.pipe",{2,7,4}}};
    {
        auto source=Image(hfsPath,Kind::HfsXCaseSensitive);
        for(size_t i=0;i<ARRAYSIZE(names);++i){
            Entry entry;
            entry.name=names[i];
            entry.posix=metadata(i);
            entry.finder[0]=42;
            entry.attributes=attributes;
            entry.attributesKnown=true;
            source->Put(2,entry,[](unsigned,uint64_t,void*,size_t){
                throw HfsPlus::Error("Unexpected special-file payload read");
            },false);
        }
    }
    const auto verify=[&](Volume& volume,size_t i){
        Entry pipe;
        Check(volume.FindChild(2,names[i],pipe)&&!pipe.directory&&!pipe.symbolicLink&&
              pipe.posix==metadata(i)&&pipe.finder[0]==42&&!pipe.forks[0].size&&!pipe.forks[1].size&&
              volume.GetAttributes(pipe.id)==attributes,"Special-file metadata changed after reopening");
        bool refused=false;
        try{volume.Read(pipe.id,0,0,nullptr,0);}catch(const std::exception&){refused=true;}
        Check(refused,"Special file exposed a regular file fork");
        return pipe.id;
    };
    auto hfs=OpenBrowserImage(hfsPath);
    Check(bool(hfs),"Cannot reopen HFSX pipe fixture");
    std::vector<Location> selection;
    for(size_t i=0;i<ARRAYSIZE(names);++i)selection.push_back(Native(hfs,verify(*hfs,i)));
    const auto transfer=[](std::vector<Location> inputs,Location destination,bool approve=false){
        Storage::Job job;
        job.transport({},[&](const auto&){
            return approve?Storage::ConflictChoice::ReplaceAll:Storage::ConflictChoice::Cancel;
        });
        Completion completion;
        return ExecuteInWorker(OrdinaryCopy{std::move(inputs),destination},job,completion);
    };
    const auto oldMetadata=[](PosixMetadata value){
        value.uid=51;
        if(IsDevice(value.special))value.device=0x0b000022;
        return value;
    };
    const auto seedOld=[&](Volume& volume){
        Entry old;
        old.name=names[1];
        old.posix=oldMetadata(metadata(2));
        old.attributes={{u"user.old",{91}}};
        old.attributesKnown=true;
        volume.Put(2,old,[](unsigned,uint64_t,void*,size_t){
            throw HfsPlus::Error("Unexpected special-file payload read");
        },false);
    };
    const auto verifyOld=[&](Volume& volume){
        Entry old;
        Check(volume.FindChild(2,names[1],old)&&old.posix==oldMetadata(metadata(2))&&
              volume.GetAttributes(old.id)==HfsPlus::Attributes{{u"user.old",{91}}},
              "Cancelled overwrite changed the original device node");
    };
    const auto host=Wide(root+"\\named-pipe-windows");
    Check(CreateDirectoryW(host.c_str(),nullptr)!=FALSE,"Cannot create pipe test folder");
    const auto refused=transfer(selection,Location::Windows(host));
    Check(refused.code&&!refused.partial&&refused.message.find("cannot preserve a special file")!=std::string::npos,
          ("Windows should reject special files before publication: "+refused.message).c_str());
    const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    for(bool external:{false,true}){
        if(external&&!fixture)continue;
        const auto path=Wide(root+(external?"\\pipe-macos.img":"\\pipe-native.img"));
        if(external){
            Check(CopyFileW(fixture,path.c_str(),TRUE)!=FALSE,"Cannot copy macOS APFS fixture");
        }else{
            auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
            Storage::Job job;
            Apfs::Format(*disk,u"Pipe transfer",true,job);
        }
        auto apfs=OpenBrowserImage(path);
        Check(bool(apfs),"Cannot open APFS named-pipe target");
        seedOld(*apfs);
        Put(*apfs,2,names[0],8193,43);
        auto result=transfer({selection[0]},Native(apfs));
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              "Cancelling a special file over a regular APFS destination should retain the destination");
        result=transfer({selection[1]},Native(apfs));
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              result.message.empty()?"Expected device overwrite cancellation":result.message.c_str());
        apfs.reset();
        apfs=OpenBrowserImage(path);
        Check(bool(apfs),"Cannot reopen cancelled APFS special overwrite");
        verifyOld(*apfs);
        Entry oldRegular;
        Check(apfs->FindChild(2,names[0],oldRegular),"Missing cancelled APFS regular destination");
        Verify(*apfs,oldRegular.id,8193,43);
        result=transfer(selection,Native(apfs),true);
        Check(!result.code,result.message.c_str());
        apfs.reset();
        apfs=OpenBrowserImage(path);
        Check(apfs&&apfs->Supports().create,"Cannot reopen writable APFS pipe target");
        std::vector<Location> apfsSelection;
        for(size_t i=0;i<ARRAYSIZE(names);++i)apfsSelection.push_back(Native(apfs,verify(*apfs,i)));
        apfs->MakeDirectory(2,u"After pipe");
        const auto roundTrip=Wide(root+(external?"\\pipe-return-macos.dsk":"\\pipe-return-native.dsk"));
        auto returned=Image(roundTrip,Kind::HfsPlus);
        seedOld(*returned);
        Put(*returned,2,names[0],8193,127);
        result=transfer({apfsSelection[0]},Native(returned));
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              "Cancelling a special file over a regular destination should retain the destination");
        result=transfer({apfsSelection[1]},Native(returned));
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              result.message.empty()?"Expected HFS device overwrite cancellation":result.message.c_str());
        returned.reset();
        returned=OpenBrowserImage(roundTrip);
        Check(bool(returned),"Cannot reopen cancelled HFS special overwrite");
        verifyOld(*returned);
        Entry oldFile;
        Check(returned->FindChild(2,names[0],oldFile),"Missing cancelled regular destination");
        Verify(*returned,oldFile.id,8193,127);
        result=transfer(apfsSelection,Native(returned),true);
        Check(!result.code,result.message.c_str());
        returned.reset();
        returned=OpenBrowserImage(roundTrip);
        Check(bool(returned),"Cannot reopen HFS+ named-pipe copy");
        for(size_t i=0;i<ARRAYSIZE(names);++i)verify(*returned,i);
        Entry existing;
        Check(apfs->FindChild(2,names[1],existing),"Cannot find overwritten device node");
        apfs->Remove(existing.id);
        apfsSelection.clear();
        apfs.reset();
        apfs=OpenBrowserImage(path);
        Check(apfs&&!apfs->FindChild(2,names[1],existing),"APFS device node survived deletion");
        apfs->MakeDirectory(2,u"After device deletion");
        const auto ordinary=Put(*apfs,2,names[1],16385,118);
        result=transfer({Native(apfs,ordinary)},Native(returned));
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              "Cancelling a regular file over a special destination should retain the destination");
        returned.reset();
        returned=OpenBrowserImage(roundTrip);
        Check(bool(returned),"Cannot reopen HFS+ after cancelling regular replacement");
        verify(*returned,1);
        result=transfer({Native(apfs,ordinary)},Native(returned),true);
        Check(!result.code,result.message.c_str());
        returned.reset();
        returned=OpenBrowserImage(roundTrip);
        Check(returned&&returned->FindChild(2,names[1],existing)&&existing.posix&&!existing.posix->IsSpecial(),
              "Regular copy did not replace the special entry");
        Verify(*returned,existing.id,16385,118);

        const auto folder=returned->MakeDirectory(2,u"Regular counterparts");
        const auto copiedFile=Put(*returned,folder,names[0],16385,74);
        result=transfer({Native(returned,copiedFile)},Native(apfs));
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              "Cancelling a regular file over an APFS special entry should retain the destination");
        apfs.reset();
        apfs=OpenBrowserImage(path);
        Check(bool(apfs),"Cannot reopen APFS after cancelling regular replacement");
        verify(*apfs,0);
        result=transfer({Native(returned,copiedFile)},Native(apfs),true);
        Check(!result.code,result.message.c_str());
        apfs.reset();
        apfs=OpenBrowserImage(path);
        Check(apfs&&apfs->Supports().create&&apfs->FindChild(2,names[0],existing)&&existing.posix&&
              !existing.posix->IsSpecial(),"Regular copy did not replace the APFS special entry");
        Verify(*apfs,existing.id,16385,74);
        verify(*apfs,2);
        apfs->MakeDirectory(2,u"After cross-type replacement");
        const auto exerciseLinks=[&](const std::shared_ptr<Volume>& from,std::shared_ptr<Volume>& to,
                                     const std::wstring& targetPath){
            const HfsPlus::Bytes targetText{'R','e','f','e','r','e','n','t',0};
            const auto makeLink=[&](Volume& volume,uint32_t parent,const std::u16string& name){
                Entry link;
                link.name=name;
                link.symbolicLink=true;
                Storage::Job job;
                volume.PutSymbolicLink(parent,link,targetText,job);
            };
            const auto sourceFolder=from->MakeDirectory(2,u"Link test source");
            makeLink(*from,sourceFolder,names[2]);
            Entry link;
            Check(from->FindChild(sourceFolder,names[2],link),"Missing source symbolic link");
            auto cancelled=transfer({Native(from,link.id)},Native(to));
            Check(cancelled.outcome==Storage::Outcome::Cancelled&&!cancelled.partial,
                  "Cancelling a symbolic link over a special entry should retain the entry");
            to.reset();
            to=OpenBrowserImage(targetPath);
            Check(bool(to),"Cannot reopen cancelled symbolic link copy");
            verify(*to,2);
            auto changed=transfer({Native(from,link.id)},Native(to),true);
            Check(!changed.code,changed.message.c_str());
            to.reset();
            to=OpenBrowserImage(targetPath);
            Check(to&&to->FindChild(2,names[2],link)&&to->SymbolicLinkTarget(link.id)==targetText,
                  "Symbolic link did not replace the special entry");

            const auto targetFolder=to->MakeDirectory(2,u"Link test destination");
            Put(*to,targetFolder,u"Referent",4097,93);
            makeLink(*to,targetFolder,names[3]);
            const auto special=verify(*from,3);
            cancelled=transfer({Native(from,special)},Native(to,targetFolder));
            Check(cancelled.outcome==Storage::Outcome::Cancelled&&!cancelled.partial,
                  "Cancelling a special entry over a symbolic link should retain the link");
            const auto reopenFolder=[&]{
                to.reset();
                to=OpenBrowserImage(targetPath);
                Entry dir,referent;
                Check(to&&to->FindChild(2,u"Link test destination",dir)&&
                      to->FindChild(dir.id,u"Referent",referent),"Missing symbolic link referent after reopen");
                Verify(*to,referent.id,4097,93);
                return dir.id;
            };
            auto parent=reopenFolder();
            Check(to->FindChild(parent,names[3],link)&&to->SymbolicLinkTarget(link.id)==targetText,
                  "Cancelled overwrite changed the symbolic link");
            changed=transfer({Native(from,special)},Native(to,parent),true);
            Check(!changed.code,changed.message.c_str());
            parent=reopenFolder();
            Check(to->FindChild(parent,names[3],link)&&!link.symbolicLink&&link.posix==metadata(3)&&
                  to->GetAttributes(link.id)==attributes,"Special entry did not replace the symbolic link");
        };
        exerciseLinks(returned,apfs,path);
        exerciseLinks(apfs,returned,roundTrip);
        const auto exerciseHardLink=[&](const std::shared_ptr<Volume>& from,std::shared_ptr<Volume>& to,
                                        const std::wstring& targetPath){
            auto parent=to->MakeDirectory(2,u"Special over hard link");
            const auto regular=Put(*to,parent,u"Keep this alias",16385,119);
            to->MakeHardLink(regular,parent,names[3]);
            const auto source=verify(*from,3);
            const auto reopen=[&]{
                to.reset();
                to=OpenBrowserImage(targetPath);
                Entry dir;
                Check(to&&to->FindChild(2,u"Special over hard link",dir),"Missing hard-link test folder");
                return dir.id;
            };
            auto result=transfer({Native(from,source)},Native(to,parent));
            Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
                  "Cancelling special entry replacement should preserve both aliases");
            parent=reopen();
            Entry kept,selected;
            Check(to->FindChild(parent,u"Keep this alias",kept)&&to->FindChild(parent,names[3],selected)&&
                  kept.hardLink&&selected.hardLink&&kept.resolvedId==selected.resolvedId,
                  "Cancelled special entry replacement broke the hard-link group");
            Verify(*to,kept.id,16385,119);
            result=transfer({Native(from,source)},Native(to,parent),true);
            Check(!result.code,result.message.c_str());
            parent=reopen();
            Check(to->FindChild(parent,u"Keep this alias",kept)&&to->FindChild(parent,names[3],selected)&&
                  selected.posix==metadata(3)&&!selected.hardLink&&to->GetAttributes(selected.id)==attributes,
                  "Special entry did not replace only the selected hard-link name");
            Verify(*to,kept.id,16385,119);

            auto whiteout=verify(*from,4);
            result=transfer({Native(from,whiteout)},Native(to,parent),true);
            Check(!result.code,result.message.c_str());
            const auto sourceFolder=from->MakeDirectory(2,u"Hard links over special entries");
            const auto file=Put(*from,sourceFolder,names[3],8193,73);
            from->MakeHardLink(file,sourceFolder,names[4]);
            Entry first,second;
            Check(from->FindChild(sourceFolder,names[3],first)&&from->FindChild(sourceFolder,names[4],second),
                  "Missing source hard-link group");
            const std::vector<Location> links{Native(from,first.id),Native(from,second.id)};
            result=transfer(links,Native(to,parent));
            Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
                  "Cancelling hard-link overwrite should retain the special entries");
            parent=reopen();
            Check(to->FindChild(parent,names[3],first)&&first.posix==metadata(3)&&
                  to->FindChild(parent,names[4],second)&&second.posix==metadata(4),
                  "Cancelled hard-link overwrite changed a special entry");
            result=transfer(links,Native(to,parent),true);
            Check(!result.code,result.message.c_str());
            parent=reopen();
            Check(to->FindChild(parent,names[3],first)&&to->FindChild(parent,names[4],second)&&
                  first.hardLink&&second.hardLink&&first.resolvedId==second.resolvedId,
                  "Overwriting special entries broke the incoming hard-link group");
            Verify(*to,first.id,8193,73);
            Verify(*to,second.id,8193,73);
        };
        exerciseHardLink(returned,apfs,path);
        exerciseHardLink(apfs,returned,roundTrip);
    }
    fprintf(report,"PASS: named pipes, device nodes, sockets and whiteouts round-trip through HFS+/HFSX and both APFS writers; replacement across special-file types preserves metadata and cancelling retains the old node after reopening. HFS+ and both APFS writers also verify replacement and cancellation between regular files, symbolic links, and special entries. Replacing a hard-link name with a special entry preserves both forks of its surviving alias. A hard-link group can also replace special entries while sharing both forks.\n");
}

void TestCopyDirectoryHardLinksToApfs(const std::string& root,FILE* report)
{
    const auto sourcePath=Wide(root+"\\linked-directory-source.dsk");
    auto source=Image(sourcePath,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
    Check(bool(source),"Cannot open directory hard-link fixture");
    const auto tree=source->MakeDirectory(2,u"Tree");
    const auto original=source->MakeDirectory(tree,u"Original folder");
    Put(*source,original,u"Child",7001,17);
    source->MakeHardLink(original,tree,u"Alias folder");
    const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    for(bool external:{false,true}){
        if(external&&!fixture)continue;
        const auto path=Wide(root+(external?"\\linked-folders-macos.img":"\\linked-folders-native.img"));
        if(external){
            Check(CopyFileW(fixture,path.c_str(),TRUE)!=FALSE,"Cannot copy macOS APFS fixture");
        }else{
            auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
            Storage::Job job;
            Apfs::Format(*disk,u"Linked folder copies",true,job);
        }
        auto target=OpenBrowserImage(path);
        Check(target&&target->Supports().create&&target->Writable()!=external,"Wrong APFS destination writer");
        const auto run=[&](bool cut,Storage::ConflictChoice answer,unsigned expectedPrompts){
            auto item=Native(source,tree);
            item.cut=cut;
            auto operation=Submit(OrdinaryCopy{{item},Native(target)});
            auto job=operation.Job();
            const auto start=GetTickCount();
            unsigned prompts=0;
            uint64_t responded=0;
            while(!job->done()){
                if(GetTickCount()-start>30000){
                    job->cancel();
                    throw HfsPlus::Error("Linked folder copy timed out");
                }
                const auto prompt=job->conflictState();
                if(!prompt.path.empty()&&responded!=prompt.id){
                    responded=prompt.id;
                    ++prompts;
                    if(prompt.kind!=Storage::CopyPrompt::IndependentFolders){
                        job->cancel();
                        throw HfsPlus::Error("Wrong prompt for directory hard-link conversion");
                    }
                    job->resolve(prompt.id,answer);
                }
                Sleep(5);
            }
            Check(prompts==expectedPrompts,"Incorrect number of directory conversion prompts");
            return job->result();
        };
        const auto cut=run(true,Storage::ConflictChoice::Replace,0);
        Check(cut.outcome==Storage::Outcome::Failed&&!cut.partial&&!cut.moved&&
              cut.message.find("cannot preserve directory hard links")!=std::string::npos,
              cut.message.empty()?"Expected directory hard-link cut refusal":cut.message.c_str());
        const auto cancelled=run(false,Storage::ConflictChoice::Cancel,1);
        Check(cancelled.outcome==Storage::Outcome::Cancelled&&!cancelled.partial,"Conversion should cancel before writing");
        target.reset();
        target=OpenBrowserImage(path);
        Entry folder;
        Check(target&&!target->FindChild(2,u"Tree",folder),"Declining conversion created a destination folder");
        const auto copied=run(false,Storage::ConflictChoice::Replace,1);
        Check(!copied.code,copied.message.c_str());
        target.reset();
        source.reset();
        target=OpenBrowserImage(path);
        source=OpenBrowserImage(sourcePath);
        Check(target&&source&&target->FindChild(2,u"Tree",folder),"Cannot reopen copied linked folder tree");
        Entry first,second,child;
        Check(target->FindChild(folder.id,u"Original folder",first)&&
              target->FindChild(folder.id,u"Alias folder",second)&&first.directory&&second.directory&&
              !first.hardLink&&!second.hardLink&&first.resolvedId!=second.resolvedId,
              "APFS should contain two independent directory copies");
        for(auto id:{first.id,second.id}){
            Check(target->FindChild(id,u"Child",child),"Converted folder lost a file");
            Verify(*target,child.id,7001,17);
        }
        for(const bool changed:{false,true}){
            if(changed)Put(*target,second.id,u"Added after copy",257,72);
            for(const bool hfsx:{false,true}){
                const auto output=path+(changed?L"-diverged":L"-returned")+(hfsx?L".hfsx":L".hfs");
                auto returned=Image(output,hfsx?HfsPlus::Volume::FormatKind::HfsXCaseSensitive:
                                                 HfsPlus::Volume::FormatKind::HfsPlus);
                Storage::Job job;
                Completion done;
                const auto result=ExecuteInWorker(OrdinaryCopy{{Native(target,folder.id)},Native(returned)},job,done);
                Check(!result.code&&done.complete,result.message.c_str());
                returned.reset();
                returned=OpenBrowserImage(output);
                Entry treeCopy,a,b,extra;
                Check(returned&&returned->FindChild(2,u"Tree",treeCopy)&&
                      returned->FindChild(treeCopy.id,u"Original folder",a)&&
                      returned->FindChild(treeCopy.id,u"Alias folder",b),
                      "Missing directory names after returning to HFS Plus");
                if(!changed&&a.resolvedId!=b.resolvedId){
                    const auto m=target->Stat(first.id),n=target->Stat(second.id);
                    fprintf(report,"Directory diagnostic equal: times=%d finder=%d posix=%d attrs=%d; attrs=%zu/%zu\n",
                            int(m.apfsTimestamps==n.apfsTimestamps),int(m.finder==n.finder),int(m.posix==n.posix),
                            int(target->GetAttributes(m.id)==target->GetAttributes(n.id)),
                            target->GetAttributes(m.id).size(),target->GetAttributes(n.id).size());
                    if(m.apfsTimestamps&&n.apfsTimestamps)for(unsigned i=0;i<4;++i)
                        fprintf(report,"Timestamp %u %llu / %llu\n",i,(*m.apfsTimestamps)[i],(*n.apfsTimestamps)[i]);
                }
                Check((a.resolvedId==b.resolvedId)==!changed&&a.hardLink==!changed&&b.hardLink==!changed,
                      "Returned directory link layout did not reflect whether the copies diverged");
                for(auto id:{a.id,b.id}){
                    Check(returned->FindChild(id,u"Child",child),"Returned folder lost a file");
                    Verify(*returned,child.id,7001,17);
                }
                Check(!returned->FindChild(a.id,u"Added after copy",extra)&&
                      returned->FindChild(b.id,u"Added after copy",extra)==changed,
                      "Restoration discarded a change to one independent copy");
            }
        }
        Check(source->FindChild(tree,u"Original folder",first)&&
              source->FindChild(tree,u"Alias folder",second)&&first.hardLink&&first.resolvedId==second.resolvedId,
              "Source directory hard links changed");
    }
    fprintf(report,"PASS: HFSX directory hard-link copies to both APFS writers require approval, preserve both folder trees and forks after reopening, and refuse Cut/Paste before publication. Verified copies regain directory hard links on HFS+ and HFSX; divergent copies remain independent.\n");
}

void TestMfsTransferVersions(const std::string& root,FILE* report)
{
    const auto sourcePath=Wide(root+"\\versioned-source.dsk");
    const auto targetPath=Wide(root+"\\versioned-target.dc42");
    constexpr auto name=u"Versioned";
    const auto seed=[](Volume& volume,const std::u16string& name,unsigned version,BYTE bias){
        Entry entry;
        entry.name=name;
        entry.version=version;
        entry.finder[0]=42;
        entry.forks[0].size=5011;
        entry.forks[1].size=173;
        return volume.Put(2,entry,[bias](unsigned fork,uint64_t at,void* output,size_t count){
            auto* bytes=static_cast<BYTE*>(output);
            for(size_t i=0;i<count;++i)bytes[i]=BYTE((at+i)*7+fork*13+bias);
        },false);
    };
    {
        auto source=MfsImageFixture(sourcePath,false);
        seed(*source,name,0,41);
        seed(*source,name,7,42);
        auto target=MfsImageFixture(targetPath,true);
        seed(*target,name,0,51);
        seed(*target,name,7,52);
    }
    const auto select=[&](Volume& volume,unsigned version){
        Entry key,entry;
        key.name=name;
        key.version=version;
        Check(volume.FindTransferChild(2,key,entry)&&entry.version==version,"Wrong MFS version selected");
        return entry.id;
    };
    auto source=OpenBrowserImage(sourcePath);
    Check(bool(source),"Cannot reopen versioned source");
    const auto transfer=[&](Location target,unsigned version,bool cut,unsigned& prompts){
        auto input=Native(source,select(*source,version));
        input.cut=cut;
        Storage::Job job;
        job.transport({},[&](const auto& label){
            ++prompts;
            Check(label.find("[version 7]")!=std::string::npos,"Overwrite did not identify the MFS version");
            return Storage::ConflictChoice::ReplaceAll;
        });
        Completion completion;
        return ExecuteInWorker(OrdinaryCopy{{input},std::move(target)},job,completion);
    };
    const auto hfsPath=Wide(root+"\\versionless-target.dsk");
    const auto classicPath=root+"\\versionless-classic.dsk";
    const auto apfsPath=Wide(root+"\\versionless-apfs.img");
    {
        auto disk=HfsPlus::CreateImage(apfsPath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Versionless",true,job);
    }
    {
        auto hfs=Image(hfsPath,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
        auto classic=ClassicImage(classicPath);
        auto apfs=OpenBrowserImage(apfsPath);
        const auto host=Wide(root+"\\versionless-windows");
        Check(CreateDirectoryW(host.c_str(),nullptr)!=FALSE,"Cannot create Windows version test folder");
        auto windows=OpenWindows(host);
        for(const auto& target:{hfs,classic,apfs,windows}){
            Check(bool(target),"Cannot open versionless destination");
            Put(*target,2,name,4081,60);
            unsigned prompts=0;
            for(bool cut:{false,true}){
                const auto destination=target==windows?Location::Windows(host):Native(target);
                const auto result=transfer(destination,7,cut,prompts);
                Check(result.outcome==Storage::Outcome::Failed&&!result.partial&&!result.moved&&!prompts&&
                      result.message.find("cannot preserve MFS file version 7")!=std::string::npos,
                      result.message.empty()?"Nonzero MFS version was silently discarded":result.message.c_str());
            }
            Entry original;
            Check(target->FindChild(2,name,original),"Rejected MFS transfer changed destination");
            Verify(*target,original.id,4081,60);
        }
    }
    source.reset();
    source=OpenBrowserImage(sourcePath);
    Check(bool(source),"Cannot reopen rejected cut source");
    Verify(*source,select(*source,0),5011,41);
    Verify(*source,select(*source,7),5011,42);
    auto target=OpenBrowserImage(targetPath);
    Check(bool(target),"Cannot reopen versioned target");
    unsigned prompts=0;
    const auto copied=transfer(Native(target),7,false,prompts);
    Check(!copied.code&&prompts==1,copied.message.c_str());
    target.reset();
    target=OpenBrowserImage(targetPath);
    Check(bool(target),"Cannot reopen MFS version overwrite");
    Verify(*target,select(*target,0),5011,51);
    Verify(*target,select(*target,7),5011,42);
    const auto moved=transfer(Native(target),7,true,prompts);
    Check(!moved.code&&moved.moved&&prompts==2,moved.message.c_str());
    target.reset();
    source.reset();
    target=OpenBrowserImage(targetPath);
    source=OpenBrowserImage(sourcePath);
    Check(target&&source,"Cannot reopen version-specific cut/paste");
    Entry removed,selection;
    selection.name=name;
    selection.version=7;
    Check(!source->FindTransferChild(2,selection,removed),"Cut retained the moved MFS version");
    Verify(*source,select(*source,0),5011,41);
    Verify(*target,select(*target,0),5011,51);
    Verify(*target,select(*target,7),5011,42);
    const auto host=Wide(root+"\\default-version-export");
    Check(CreateDirectoryW(host.c_str(),nullptr)!=FALSE,"Cannot create default version export folder");
    const auto exported=transfer(Location::Windows(host),0,false,prompts);
    Check(!exported.code&&prompts==2,exported.message.c_str());
    auto windows=OpenWindows(host);
    Entry file;
    Check(windows&&windows->FindChild(2,name,file)&&!file.version,"MFS version zero did not export normally");
    Verify(*windows,file.id,5011,41);
    fprintf(report,"PASS: ordinary MFS copies preserve distinct versions, identify the precise overwrite and reject nonzero versions on versionless targets before prompting or deleting; version zero still exports.\n");
}

void TestClassicMetadataCompanion(FILE* report)
{
    Entry source;
    source.name=u"Original Unicode name \\u03A9";
    source.datesValid=true;
    source.created=2082844800;
    source.modified=2082844811;
    source.additionalDates={2082844822,2082844833,2082844844};
    source.apfsTimestamps=std::array<uint64_t,4>{1,11000000003,33000000005,22000000007};
    source.posix=PosixMetadata{501,20,0750,PosixMetadata::UserImmutable,PosixMetadata::NoDump};
    source.attributes[u"user.empty"]={};
    source.attributes[u"user.large"]=HfsPlus::AttributeData::Snapshot(1024*1024+9,
        [](uint64_t at,void* out,size_t count){
            auto* bytes=static_cast<uint8_t*>(out);
            for(size_t i=0;i<count;++i)bytes[i]=uint8_t((at+i)*13);
        });
    ClassicCopyArchive::Digest binding{};
    binding[0]=12;
    Storage::Job job;
    const auto stored=ClassicCopyArchive::Encode(ClassicCopyArchive::Metadata(source),binding,job);
    const auto decoded=ClassicCopyArchive::Decode(stored,binding,job);
    const auto* restored=decoded?&decoded->entry:nullptr;
    Check(restored&&restored->attributes==source.attributes&&restored->posix==source.posix&&
          restored->apfsTimestamps==source.apfsTimestamps&&restored->name==source.name&&restored->created==source.created&&
          restored->modified==source.modified&&restored->additionalDates.backup==source.additionalDates.backup,
          "Classic companion lost source metadata");
    auto stale=binding;
    ++stale.front();
    Check(!ClassicCopyArchive::Decode(stored,stale,job),"Stale classic companion was accepted");
    uint8_t byte=0;
    stored.Read(stored.size()-33,&byte,1);
    const auto corrupted=stored.Patch(stored.size()-33,{uint8_t(byte^1)});
    bool rejected=false;
    try{ClassicCopyArchive::Decode(corrupted,binding,job);}catch(const HfsPlus::Error&){rejected=true;}
    Check(rejected,"Corrupted classic companion was accepted");
    fprintf(report,"PASS: classic metadata companion streams attributes and validates object binding and payload checksum.\n");
}

void TestClassicArchiveRoundTrip(const std::string& root,FILE* report)
{
    const auto base=root+"\\classic-archive";
    const auto format=[&](const std::wstring& path){
        auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Classic archive test",true,job);
    };
    const auto sourcePath=Wide(base+"-source.img");
    format(sourcePath);
    auto source=OpenBrowserImage(sourcePath);
    auto classic=ClassicImage(base+".dsk");
    Check(source&&classic,"Cannot mount classic archive test fixtures");
    Entry folder;
    folder.name=u"Archived folder";
    folder.directory=true;
    folder.attributes[u"user.folder"]={7,1};
    const auto folderId=source->MakeDirectoryWithAttributes(2,folder);
    Entry file;
    file.name=u"Archived file";
    file.posix=PosixMetadata{501,20,0740,0,PosixMetadata::NoDump};
    file.datesValid=true;
    file.created=2082844800;
    file.modified=2082844811;
    file.additionalDates={2082844822,2082844833};
    file.apfsTimestamps=std::array<uint64_t,4>{1,11000000003,33000000005,22000000007};
    file.attributes[u"user.file"]={4,2};
    HfsPlus::Bytes acl(68);
    HfsPlus::Disk::P32(acl.data(),0x012cc16d);
    acl[4]=1;
    HfsPlus::Disk::P32(acl.data()+36,1);
    acl[44]=2;
    HfsPlus::Disk::P32(acl.data()+60,1);
    HfsPlus::Disk::P32(acl.data()+64,2);
    file.attributes[u"com.apple.system.Security"]=acl;
    file.attributesKnown=true;
    file.forks[0].size=4097;
    file.forks[1].size=173;
    const auto input=source->Put(folderId,file,[](unsigned fork,uint64_t at,void* out,size_t count){
        auto* bytes=static_cast<uint8_t*>(out);
        for(size_t i=0;i<count;++i)bytes[i]=uint8_t(at+i+fork);
    },false);
    const auto original=source->Stat(input);
    auto archive=Submit(OrdinaryCopy{{Native(source,folderId)},Native(classic)});
    Conflict(archive);
    const auto prompt=archive.Job()->conflictState();
    Check(prompt.kind==Storage::CopyPrompt::ClassicMetadataLoss,"Archive copy prompt missing");
    archive.Job()->resolve(prompt.id,Storage::ConflictChoice::ReplaceAll);
    Wait(archive);
    auto archived=archive.Job()->result();
    Check(!archived.code&&archive.Snapshot().complete,archived.message.c_str());
    classic.reset();
    classic=OpenBrowserImage(Wide(base+".dsk"));
    Entry archivedFolder,archivedFile;
    Check(classic&&classic->FindChild(2,folder.name,archivedFolder)&&
          classic->FindChild(archivedFolder.id,file.name,archivedFile),"Classic archive did not reopen");
    const auto returnPath=Wide(base+"-return.img");
    format(returnPath);
    auto returned=OpenBrowserImage(returnPath);
    Check(bool(returned),"Cannot mount classic archive return image");
    auto cutItem=Native(classic,archivedFolder.id);
    cutItem.cut=true;
    auto cut=Submit(OrdinaryCopy{{cutItem},Native(returned)});
    Wait(cut);
    const auto refused=cut.Job()->result();
    Entry absent;
    Check(refused.outcome==Storage::Outcome::Failed&&!refused.moved&&!refused.partial&&
          !returned->FindChild(2,folder.name,absent)&&classic->FindChild(2,folder.name,archivedFolder),
          "Classic archive Cut/Paste must retain the original tree");
    auto restore=Submit(OrdinaryCopy{{Native(classic,archivedFolder.id)},Native(returned)});
    Wait(restore);
    const auto restored=restore.Job()->result();
    Check(!restored.code&&restore.Snapshot().complete,restored.message.c_str());
    returned.reset();
    returned=OpenBrowserImage(returnPath);
    Entry copiedFolder,copiedFile;
    Check(returned&&returned->FindChild(2,folder.name,copiedFolder)&&
          returned->FindChild(copiedFolder.id,file.name,copiedFile)&&
          returned->List(copiedFolder.id).size()==1&&returned->GetAttributes(copiedFolder.id)==folder.attributes&&
          returned->GetAttributes(copiedFile.id)==file.attributes&&copiedFile.posix==original.posix&&
          returned->Stat(copiedFile.id).apfsTimestamps==original.apfsTimestamps,
          "Archive round trip lost file or folder metadata");
    for(unsigned fork=0;fork<2;++fork){
        HfsPlus::Bytes before(size_t(file.forks[fork].size)),after(before.size());
        source->Read(input,fork,0,before.data(),before.size());
        returned->Read(copiedFile.id,fork,0,after.data(),after.size());
        Check(before==after,"Archive round trip changed a fork");
    }
    hfsdirent properties{};
    Check(classic->GetClassicProperties(archivedFile.id,properties),"Cannot read archive binding fixture");
    properties.mddate+=3;
    classic->SetClassicProperties(archivedFile.id,properties);
    Storage::Job job;
    Check(!ClassicCopyArchive::Load(*classic,classic->Stat(archivedFile.id),job),
          "A modified classic file accepted stale archive metadata");
    fprintf(report,"PASS: an opt-in classic HFS archive restores file and folder metadata and both forks, and rejects stale companions after reopening.\n");
}

void TestClassicArchiveNamePreview(const std::string& root,FILE* report)
{
    const auto base=root+"\\classic-name-archive";
    const std::u16string folderName=u"Long folder \\u540D with a MacRoman filename overflow";
    const auto sourcePath=Wide(base+"-source.dsk");
    auto source=Image(sourcePath,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
    auto classic=ClassicImage(base+".dsk");
    const auto folder=source->MakeDirectory(2,folderName);
    Put(*source,folder,u"Report",4097,32);
    Put(*source,folder,u"report",4099,63);
    const auto initiallyProjected=ProjectClassicCopyName(folderName);
    const auto sentinel=Put(*classic,2,initiallyProjected,1024,91);
    const auto approvedFolder=ProjectClassicCopyName(folderName,1);
    const auto run=[&]{return Submit(OrdinaryCopy{{Native(source,folder)},Native(classic)});};
    auto declined=run();
    Conflict(declined);
    auto preview=declined.Job()->conflictState();
    Check(preview.kind==Storage::CopyPrompt::ClassicArchiveNames&&
          preview.path.find(BrowserPresentation::Utf8(folderName))!=std::string::npos&&
          preview.path.find(BrowserPresentation::Utf8(approvedFolder))!=std::string::npos&&
          preview.path.find("~1")!=std::string::npos,
          "Classic archive preview omitted renamed or colliding files");
    declined.Job()->resolve(preview.id,Storage::ConflictChoice::Cancel);
    Wait(declined);
    Entry absent;
    Check(declined.Job()->result().outcome==Storage::Outcome::Cancelled&&
          !classic->FindChild(2,approvedFolder,absent),"Declining the preview created the renamed directory");
    auto copy=run();
    Conflict(copy);
    preview=copy.Job()->conflictState();
    Check(preview.kind==Storage::CopyPrompt::ClassicArchiveNames,"Missing archive preview on retry");
    copy.Job()->resolve(preview.id,Storage::ConflictChoice::Replace);
    Wait(copy);
    const auto copied=copy.Job()->result();
    Check(!copied.code&&copy.Snapshot().complete,copied.message.c_str());
    Verify(*classic,sentinel,1024,91);
    classic.reset();
    classic=OpenBrowserImage(Wide(base+".dsk"));
    Entry archived;
    Check(classic&&classic->FindChild(2,approvedFolder,archived),"Approved classic name was not used");
    const auto returnPath=Wide(base+"-return.dsk");
    auto returned=Image(returnPath,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
    auto restore=Submit(OrdinaryCopy{{Native(classic,archived.id)},Native(returned)});
    Wait(restore);
    const auto restored=restore.Job()->result();
    Check(!restored.code&&restore.Snapshot().complete,restored.message.c_str());
    returned.reset();
    returned=OpenBrowserImage(returnPath);
    Entry roundTrip,a,b;
    Check(returned&&returned->FindChild(2,folderName,roundTrip)&&
          returned->FindChild(roundTrip.id,u"Report",a)&&returned->FindChild(roundTrip.id,u"report",b)&&
          a.resolvedId!=b.resolvedId&&returned->List(roundTrip.id).size()==2,
          "Original Unicode and case-distinct names were not restored");
    Verify(*returned,a.id,4097,32);
    Verify(*returned,b.id,4099,63);
    fprintf(report,"PASS: classic archive previews MacRoman truncation and collisions, preserves occupied names, and restores original names and both forks after reopening.\n");
}

void TestClassicArchiveSpecialEntries(const std::string& root,FILE* report)
{
    using Mode=HfsPlus::Volume::CompressionMode;
    using Kind=HfsPlus::Volume::FormatKind;
    const auto base=root+"\\classic-archive-layout";
    auto source=Image(Wide(base+"-source.dsk"),Kind::HfsXCaseSensitive);
    auto classic=ClassicImage(base+".dsk");
    const auto folder=source->MakeDirectory(2,u"Archive layout");
    const auto original=Put(*source,folder,u"Original",4097,52);
    source->MakeHardLink(original,folder,u"Alias");
    const auto branch=source->MakeDirectory(folder,u"Branch");
    Put(*source,branch,u"Child",4011,33);
    source->MakeHardLink(branch,folder,u"Branch alias");
    Storage::Job setup;
    Entry link;
    link.name=u"Pointer";
    link.symbolicLink=true;
    link.posix=PosixMetadata{501,20,0777};
    const HfsPlus::Bytes target{'O','r','i','g','i','n','a','l',0};
    source->PutSymbolicLink(folder,link,target,setup);
    Entry device;
    device.name=u"Device";
    device.posix=PosixMetadata{501,20,0600,0,0,SpecialFileType::CharacterDevice,0x240001};
    source->Put(folder,device,[](unsigned,uint64_t,void*,size_t){throw HfsPlus::Error("Device payload read");},false);
    Entry compressed;
    compressed.name=u"Compressed";
    compressed.forks[0].size=100003;
    const auto payload=[](unsigned,uint64_t at,void* out,size_t count){
        auto* p=static_cast<uint8_t*>(out);
        for(size_t i=0;i<count;++i)p[i]=uint8_t((at+i)*13+(at+i)/1024);
    };
    const auto compressedId=source->Put(folder,compressed,payload,false);
    source->SetCompressionMethod(compressedId,Mode::Lzvn,{});
    Check(source->Stat(compressedId).compressionResourceSize>0,"Expected resource-backed compression");
    auto copy=Submit(OrdinaryCopy{{Native(source,folder)},Native(classic)});
    Conflict(copy);
    auto prompt=copy.Job()->conflictState();
    Check(prompt.kind==Storage::CopyPrompt::ClassicMetadataLoss,"Missing archive layout prompt");
    copy.Job()->resolve(prompt.id,Storage::ConflictChoice::ReplaceAll);
    Wait(copy);
    const auto copied=copy.Job()->result();
    Check(!copied.code&&copy.Snapshot().complete,copied.message.c_str());
    classic.reset();
    classic=OpenBrowserImage(Wide(base+".dsk"));
    Entry archived,placeholder;
    Check(classic&&classic->FindChild(2,u"Archive layout",archived)&&
          classic->FindChild(archived.id,u"Pointer",placeholder)&&!placeholder.symbolicLink&&
          placeholder.forks[0].size>target.size(),"Archived symlink is not a readable classic description");
    for(bool apfs:{false,true}){
        const auto returnPath=Wide(base+(apfs?"-return.img":"-return.dsk"));
        if(apfs){
            auto disk=HfsPlus::CreateImage(returnPath.c_str(),32*1024*1024);
            Storage::Job format;
            Apfs::Format(*disk,u"APFS archive return",true,format);
        }else Image(returnPath,Kind::HfsXCaseSensitive);
        auto returned=OpenBrowserImage(returnPath);
        Check(bool(returned),"Cannot reopen the archive layout destination");
        auto restore=Submit(OrdinaryCopy{{Native(classic,archived.id)},Native(returned)});
        if(apfs){
            Conflict(restore);
            const auto conversion=restore.Job()->conflictState();
            Check(conversion.kind==Storage::CopyPrompt::IndependentFolders,"APFS directory archive needs approval");
            restore.Job()->resolve(conversion.id,Storage::ConflictChoice::Replace);
        }
        Wait(restore);
        const auto restored=restore.Job()->result();
        Check(!restored.code&&restore.Snapshot().complete,restored.message.c_str());
        returned.reset();
        returned=OpenBrowserImage(returnPath);
        Entry tree,a,b,d,e,child,symlink,node,encoded;
        Check(returned&&returned->FindChild(2,u"Archive layout",tree)&&
              returned->FindChild(tree.id,u"Original",a)&&returned->FindChild(tree.id,u"Alias",b)&&
              a.hardLink&&b.hardLink&&a.resolvedId==b.resolvedId,
              "Classic archive did not restore file hard-link identity");
        Verify(*returned,a.id,4097,52);
        Verify(*returned,b.id,4097,52);
        Check(returned->FindChild(tree.id,u"Branch",d)&&returned->FindChild(tree.id,u"Branch alias",e)&&
              d.directory&&e.directory&&d.hardLink==!apfs&&e.hardLink==!apfs&&
              (d.resolvedId==e.resolvedId)==!apfs&&returned->FindChild(d.id,u"Child",child),
              "Classic archive did not restore the supported directory link layout");
        Verify(*returned,child.id,4011,33);
        Check(returned->FindChild(tree.id,u"Pointer",symlink)&&returned->SymbolicLinkTarget(symlink.id)==target&&
              returned->FindChild(tree.id,u"Device",node)&&node.posix==device.posix,
              "Classic archive did not restore the symbolic link or device metadata");
        Check(returned->FindChild(tree.id,u"Compressed",encoded)&&encoded.compressed,
              "Classic archive did not restore compression");
        encoded=returned->Stat(encoded.id);
        const auto& header=encoded.inlineCompression.empty()?encoded.resourceCompressionHeader:encoded.inlineCompression;
        Check(!header.empty()&&HfsPlus::Compression(header,0,{}).Type()==8,
              "Classic archive did not restore the resource-backed LZVN method");
        HfsPlus::Bytes expected(size_t(compressed.forks[0].size)),actual(expected.size());
        payload(0,0,expected.data(),expected.size());
        returned->Read(encoded.id,0,0,actual.data(),actual.size());
        Check(actual==expected,"Classic archive compression changed the logical file contents");
    }
    fprintf(report,"PASS: classic archive restores file and directory hard links, symbolic links, device metadata, and resource-backed LZVN after reopening.\n");
}

void TestClassicBackupDates(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    constexpr time_t fileBackup=1234567890,folderBackup=-61516800;
    const auto prefix=root+"\\classic-backup";
    auto source=ClassicImage(prefix+"-source.dsk");
    const auto folder=source->MakeDirectory(2,u"Backup folder");
    const auto file=Put(*source,folder,u"Backup file",4097,32);
    for(const auto& [id,date]:{std::pair{folder,folderBackup},std::pair{file,fileBackup}}){
        hfsdirent properties{};
        Check(source->GetClassicProperties(id,properties),"Cannot read classic backup fixture Properties");
        properties.bkdate=date;
        source->SetClassicProperties(id,properties);
    }
    const auto verify=[&](Volume& volume,bool classic){
        Entry directory,entry;
        Check(volume.FindChild(2,u"Backup folder",directory)&&
              volume.FindChild(directory.id,u"Backup file",entry),"Missing backup date round-trip entries");
        if(classic){
            hfsdirent folderProperties{},fileProperties{};
            Check(volume.GetClassicProperties(directory.id,folderProperties)&&uint32_t(folderProperties.bkdate)==uint32_t(folderBackup)&&
                  volume.GetClassicProperties(entry.id,fileProperties)&&uint32_t(fileProperties.bkdate)==uint32_t(fileBackup),
                  "Classic folder or file backup date changed after reopening");
        }
        Verify(volume,entry.id,4097,32);
        return directory.id;
    };
    for(unsigned i=0;i<5;++i){
        const auto base=prefix+"-"+std::to_string(i);
        const auto path=Wide(base+"-middle.img");
        if(i==3){
            auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
            Storage::Job format;
            Apfs::Format(*disk,u"Classic backup dates",true,format);
        }else if(i==4){
            auto disk=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Expanded dates",true,Kind::HfsPlus,true);
        }else{
            const Kind formats[]={Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive};
            Image(path,formats[i]);
        }
        auto middle=OpenBrowserImage(path);
        Check(bool(middle),"Cannot open backup date intermediate image");
        auto outbound=Submit(OrdinaryCopy{{Native(source,folder)},Native(middle)});
        Wait(outbound);
        const auto copied=outbound.Job()->result();
        Check(!copied.code&&outbound.Snapshot().complete,copied.message.c_str());
        middle.reset();
        middle=OpenBrowserImage(path);
        Check(bool(middle),"Cannot reopen backup date intermediate image");
        const auto middleFolder=verify(*middle,false);
        if(i<3){
            Entry entry;
            Check(middle->FindChild(middleFolder,u"Backup file",entry)&&
                  middle->Stat(middleFolder).additionalDates.backup==source->Stat(folder).additionalDates.backup&&
                  middle->Stat(entry.id).additionalDates.backup==source->Stat(file).additionalDates.backup,
                  "HFS Plus did not store native backup dates");
        }
        const auto returned=base+"-returned.dsk";
        auto classic=ClassicImage(returned);
        auto copy=Submit(OrdinaryCopy{{Native(middle,middleFolder)},Native(classic)});
        Conflict(copy);
        const auto warning=copy.Job()->conflictState();
        Check(warning.kind==Storage::CopyPrompt::ClassicMetadataLoss&&
              warning.path.find("Extended attributes")==std::string::npos,"Backup dates triggered an attribute-loss warning");
        copy.Job()->resolve(warning.id,Storage::ConflictChoice::Replace);
        Wait(copy);
        const auto restored=copy.Job()->result();
        Check(!restored.code&&copy.Snapshot().complete,restored.message.c_str());
        classic.reset();
        classic=OpenBrowserImage(Wide(returned));
        Check(bool(classic),"Cannot reopen classic backup date output");
        verify(*classic,true);
    }
    fprintf(report,"PASS: classic HFS file and folder backup dates and both forks survive HFS+/HFSX and APFS round trips after reopening.\n");
}

void TestClassicCopyLoss(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    for(unsigned i=0;i<3;++i){
        const auto base=root+"\\classic-copy-loss-"+std::to_string(i);
        const auto path=Wide(base+"-source.img");
        if(i==2){
            auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
            Storage::Job format;
            Apfs::Format(*disk,u"Classic copy preflight",true,format);
        }else Image(path,i?Kind::HfsXCaseSensitive:Kind::HfsPlus);
        auto source=OpenBrowserImage(path);
        auto destination=ClassicImage(base+"-destination.dsk");
        Check(source&&destination,"Cannot mount classic preflight fixtures");
        const auto folder=source->MakeDirectory(2,u"Linked files");
        const auto original=Put(*source,folder,u"Original",4097,44);
        source->MakeHardLink(original,folder,u"Alias");
        const auto sentinel=Put(*destination,2,u"Existing",4096,57);
        const auto free=destination->FreeBytes();
        auto item=Native(source,folder);
        auto run=[&]{return Submit(OrdinaryCopy{{item},Native(destination)});};

        auto declined=run();
        Conflict(declined);
        const auto prompt=declined.Job()->conflictState();
        Check(prompt.kind==Storage::CopyPrompt::ClassicMetadataLoss&&
              prompt.path.find("Unix ownership and permissions")!=std::string::npos&&
              prompt.path.find("Hard-link relationships")!=std::string::npos,
              "Classic warning omitted nested hard links or permissions");
        declined.Job()->resolve(prompt.id,Storage::ConflictChoice::Cancel);
        Wait(declined);
        Check(declined.Job()->result().outcome==Storage::Outcome::Cancelled,"Classic warning cancellation failed");

        item.cut=true;
        auto cut=run();
        Wait(cut);
        const auto refused=cut.Job()->result();
        Check(refused.outcome==Storage::Outcome::Failed&&!refused.partial&&!refused.moved&&
              refused.message.find("Hard-link relationships")!=std::string::npos,
              "Lossy classic Cut/Paste was not refused");
        Entry found;
        Check(destination->FreeBytes()==free&&!destination->FindChild(2,u"Linked files",found)&&
              source->FindChild(folder,u"Original",found)&&source->FindChild(folder,u"Alias",found),
              "Declining or refusing classic copy changed either volume");
        Verify(*destination,sentinel,4096,57);

        item.cut=false;
        auto approved=run();
        Conflict(approved);
        const auto warning=approved.Job()->conflictState();
        Check(warning.kind==Storage::CopyPrompt::ClassicMetadataLoss,"Expected one classic preflight warning");
        approved.Job()->resolve(warning.id,Storage::ConflictChoice::Replace);
        Pass(approved);
        destination.reset();
        destination=OpenBrowserImage(Wide(base+"-destination.dsk"));
        Entry copied,a,b;
        Check(destination&&destination->FindChild(2,u"Linked files",copied)&&
              destination->FindChild(copied.id,u"Original",a)&&destination->FindChild(copied.id,u"Alias",b)&&
              a.resolvedId!=b.resolvedId,"Classic HFS did not publish independent copies");
        Verify(*destination,a.id,4097,44);
        Verify(*destination,b.id,4097,44);
    }
    fprintf(report,"PASS: classic HFS warns once before copying nested APFS/HFS+/HFSX hard links, retains both forks on approval, and refuses destructive Cut/Paste.\n");
}

void TestClassicLockRoundTrip(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const auto sourcePath=root+"\\classic-lock-source.dsk";
    constexpr auto locked=u"Locked file", unlocked=u"Unlocked file";
    {
        auto classic=ClassicImage(sourcePath);
        const auto id=Put(*classic,2,locked,5003,34);
        Put(*classic,2,unlocked,5007,35);
        hfsdirent properties{};
        Check(classic->GetClassicProperties(id,properties),"Cannot read classic lock fixture");
        properties.flags|=HFS_ISLOCKED;
        classic->SetClassicProperties(id,properties);
    }
    const auto verify=[](Volume& volume,const std::u16string& name,bool expectLocked,uint64_t size,BYTE seed){
        Entry entry;
        Check(volume.FindChild(2,name,entry)&&entry.protectedFile==expectLocked,
              "Copy did not preserve the lock after reopening");
        Verify(volume,entry.id,size,seed);
        return entry.id;
    };
    const auto transfer=[](const std::shared_ptr<Volume>& from,const std::shared_ptr<Volume>& to,bool cancel){
        std::vector<Location> inputs;
        for(auto name:{u"Locked file",u"Unlocked file"}){
            Entry entry;
            Check(from->FindChild(2,name,entry),"Missing classic lock test input");
            inputs.push_back(Native(from,entry.id));
        }
        Storage::Job job;
        job.transport([&](const Storage::Progress& p){
            if(cancel&&p.fileBytes)job.cancel();
        },[](const auto&){return Storage::ConflictChoice::Replace;});
        Completion completion;
        auto result=std::async(std::launch::async,[&]{
            return ExecuteInWorker(OrdinaryCopy{inputs,Native(to)},job,completion);
        });
        const auto start=GetTickCount();
        bool warned=false,unexpected=false;
        while(result.wait_for(std::chrono::milliseconds(5))!=std::future_status::ready){
            const auto prompt=job.conflictState();
            if(!prompt.path.empty()&&!warned){
                warned=true;
                unexpected=prompt.kind!=Storage::CopyPrompt::ClassicMetadataLoss;
                job.resolve(prompt.id,unexpected?Storage::ConflictChoice::Cancel:Storage::ConflictChoice::Replace);
            }
            if(GetTickCount()-start>30000){job.cancel();break;}
        }
        const auto outcome=result.get();
        Check(!unexpected&&warned==(!strcmp(to->Kind(),"HFS")&&strcmp(from->Kind(),"HFS")),
              "Unexpected classic lock round-trip warning");
        return outcome;
    };
    auto source=OpenBrowserImage(Wide(sourcePath));
    Check(bool(source),"Cannot reopen classic lock source");
    verify(*source,locked,true,5003,34);
    const Kind kinds[]={Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive};
    const auto macos=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    for(unsigned i=0;i<6;++i){
        if(i==5&&!macos)continue;
        const auto base=root+"\\classic-lock-"+std::to_string(i);
        const auto middlePath=Wide(base+(i>=4?".img":".dsk"));
        if(i==5){
            Check(CopyFileW(macos,middlePath.c_str(),TRUE)!=FALSE,"Cannot copy macOS APFS lock fixture");
        }else if(i==4){
            auto disk=HfsPlus::CreateImage(middlePath.c_str(),32*1024*1024);
            Storage::Job job;
            Apfs::Format(*disk,u"Classic lock round trip",true,job);
        }else if(i==3){
            ClassicImage(base+".dsk");
        }else{
            Image(middlePath,kinds[i]);
        }
        auto middle=OpenBrowserImage(middlePath);
        Check(middle&&middle->Supports().create,"Cannot open intermediate lock fixture");
        Put(*middle,2,locked,4001,72);
        auto result=transfer(source,middle,false);
        Check(!result.code,result.message.c_str());
        middle.reset();
        middle=OpenBrowserImage(middlePath);
        Check(bool(middle),"Cannot reopen intermediate lock copy");
        const auto id=verify(*middle,locked,true,5003,34);
        if(i!=3){
            const auto metadata=middle->Stat(id);
            Check(metadata.posix&&metadata.posix->protection==PosixMetadata::UserImmutable,
                  "Classic lock did not map to user immutability");
        }
        verify(*middle,unlocked,false,5007,35);
        const auto destinationPath=base+"-returned.dsk";
        auto returned=ClassicImage(destinationPath);
        Put(*returned,2,locked,4077,73);
        result=transfer(middle,returned,true);
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              result.message.empty()?"Classic lock overwrite did not cancel":result.message.c_str());
        returned.reset();
        returned=OpenBrowserImage(Wide(destinationPath));
        Check(bool(returned),"Cannot reopen cancelled classic overwrite");
        verify(*returned,locked,false,4077,73);
        result=transfer(middle,returned,false);
        Check(!result.code,result.message.c_str());
        returned.reset();
        returned=OpenBrowserImage(Wide(destinationPath));
        Check(bool(returned),"Cannot reopen classic lock round trip");
        const auto resultId=verify(*returned,locked,true,5003,34);
        verify(*returned,unlocked,false,5007,35);
        hfsdirent properties{};
        Check(returned->GetClassicProperties(resultId,properties)&&(properties.flags&HFS_ISLOCKED),
              "The classic catalog lock bit was lost");
    }
    fprintf(report,"PASS: classic HFS lock bit round-trips through classic HFS, HFS+/HFSX and APFS with both forks; canceled overwrites preserve the original unlocked file.\n");
    if(macos)fprintf(report,"PASS: the classic lock round trip also works through a macOS APFS image.\n");
}

void TestClassicAndMfsCopyNameCollisions(const std::string& root,FILE* report)
{
    const auto sourcePath=Wide(root+"\\classic-mfs-collision-source.dsk");
    auto source=Image(sourcePath,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
    Check(source&&source->CaseSensitive(),"Missing case-sensitive collision source");
    constexpr auto lower=u"Case file";
    constexpr auto upper=u"CASE FILE";
    Put(*source,2,lower,5003,11);
    Put(*source,2,upper,5011,13);
    const auto tree=source->MakeDirectory(2,u"Tree");
    Put(*source,tree,u"Earlier file",513,15);
    const auto nested=source->MakeDirectory(tree,u"Nested");
    Put(*source,nested,lower,739,17);
    Put(*source,nested,upper,741,19);
    source.reset();

    const auto bytes=[](const std::wstring& path){
        auto disk=HfsPlus::OpenImage(path.c_str(),false);
        HfsPlus::Bytes data(size_t(disk->Size()));
        disk->Read(0,data.data(),data.size());
        return data;
    };
    const auto sourceBefore=bytes(sourcePath);
    for(const unsigned kind:{0u,1u,2u,3u}){
        const auto path=root+"\\classic-mfs-collision-"+std::to_string(kind)+".dsk";
        const auto widePath=Wide(path);
        const auto open=[&]{
            if(kind==0)return OpenClassic(path,"Classic:",true);
            return OpenBrowserImage(widePath);
        };
        auto seeded=kind<2?ClassicImage(path):MfsImageFixture(widePath,kind==3);
        Check(bool(seeded),"Cannot seed classic HFS or MFS destination");
        Put(*seeded,2,lower,9721,81);
        seeded.reset();
        if(kind==1){
            auto mounted=open();
            Check(mounted&&VolumeSession::VolumeSessionPidForTest(mounted),"Missing persistent classic HFS destination");
        }
        const auto targetBefore=bytes(widePath);
        const auto run=[&](bool cut,bool recursive){
            auto target=open();
            source=OpenBrowserImage(sourcePath);
            Check(target&&source&&target->Supports().create,"Cannot reopen collision fixtures");
            std::vector<Location> inputs;
            const auto select=[&](const std::u16string& name){
                Entry entry;
                Check(source->FindChild(2,name,entry),"Rejected move lost a source entry");
                auto item=Native(source,entry.id);
                item.cut=cut;
                inputs.push_back(std::move(item));
            };
            if(recursive)select(u"Tree");else{select(lower);select(upper);}
            if(kind<2&&!cut){
                auto copy=Submit(OrdinaryCopy{inputs,Native(target)});
                Conflict(copy);
                const auto preview=copy.Job()->conflictState();
                Check(preview.kind==Storage::CopyPrompt::ClassicArchiveNames,
                      "Classic name collision reached overwrite handling");
                copy.Job()->resolve(preview.id,Storage::ConflictChoice::Cancel);
                Wait(copy);
                Check(copy.Job()->result().outcome==Storage::Outcome::Cancelled&&!copy.Snapshot().complete,
                      "Declining the classic name preview published files");
                inputs.clear();
                target.reset();
                source.reset();
                Check(bytes(widePath)==targetBefore&&bytes(sourcePath)==sourceBefore,
                      "Declining the name preview changed destination or source bytes");
                return;
            }
            unsigned prompts=0;
            bool copying=false;
            Storage::Job job;
            job.transport([&](const Storage::Progress& p){copying|=!p.scanning;},
                          [&](const auto&){++prompts;return Storage::ConflictChoice::ReplaceAll;});
            Completion completion;
            {
                auto result=ExecuteInWorker(OrdinaryCopy{inputs,Native(target)},job,completion);
                const std::string expected=kind<2?"collide on the classic HFS destination":"collide on the MFS destination";
                Check(result.outcome==Storage::Outcome::Failed&&!result.partial&&!result.moved&&
                      !completion.complete&&!prompts&&!copying&&result.message.find(expected)!=std::string::npos,
                      result.message.empty()?"Collision reached overwrite or move handling":result.message.c_str());
            }
            inputs.clear();
            target.reset();
            source.reset();
            Check(bytes(widePath)==targetBefore&&bytes(sourcePath)==sourceBefore,
                  "Rejected collision changed destination or source image bytes");
        };
        for(bool cut:{false,true}){
            run(cut,false);
            if(kind<2)run(cut,true);
        }
    }
    Entry first,second;
    first.name=lower;
    second.name=upper;
    second.version=1;
    ValidateMfsCopyNames({first,second});
    fprintf(report,"PASS: classic HFS previews colliding copy names and refuses colliding Cut/Paste before writes; raw/DC42 MFS reject collisions within each version.\n");
}

void TestHfsxToApfsNameCollisions(const std::string& root,FILE* report)
{
    const auto sourcePath=Wide(root+"\\hfsx-apfs-collision-source.dsk");
    {
        auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
        HfsPlus::Volume::Format(*disk,u"Collision source",true,HfsPlus::Volume::FormatKind::HfsXCaseSensitive);
    }
    auto source=OpenBrowserImage(sourcePath);
    Check(source&&source->CaseSensitive()&&VolumeSession::VolumeSessionPidForTest(source),
          "Missing case-sensitive HFSX source");
    constexpr auto lower=u"HFV readme";
    constexpr auto upper=u"HFV README";
    const auto first=Put(*source,2,lower,4097,3);
    const auto second=Put(*source,2,upper,4111,11);
    const auto folder=source->MakeDirectory(2,u"Collision subtree");
    const auto independent=Put(*source,folder,u"Independent file",513,7);
    const auto nested=source->MakeDirectory(folder,u"Nested");
    const auto nestedFirst=Put(*source,nested,lower,917,19);
    const auto nestedSecond=Put(*source,nested,upper,929,23);
    std::vector<Location> selected{Native(source,first),Native(source,second)};
    auto moving=selected;
    for(auto& item:moving)item.cut=true;

    const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    for(unsigned index=0;index<3;++index){
        const bool external=index==1,sensitive=index==2;
        if(external&&!fixture)continue;
        const auto targetPath=Wide(root+"\\hfsx-apfs-collision-destination-"+std::to_string(index)+".img");
        if(external){
            Check(CopyFileW(fixture,targetPath.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
        }else{
            auto disk=HfsPlus::CreateImage(targetPath.c_str(),32*1024*1024);
            Storage::Job format;
            Apfs::Format(*disk,u"Collision destination",sensitive,format);
        }
        const auto checkpoint=[&]{
            auto disk=HfsPlus::OpenImage(targetPath.c_str(),false);
            const auto current=Apfs::Probe(*disk);
            Check(current.has_value(),"Missing APFS destination checkpoint");
            return current->transaction;
        };
        auto target=OpenBrowserImage(targetPath);
        Check(target&&target->Supports().create&&target->CaseSensitive()==sensitive&&
              target->Writable()!=external,"Wrong APFS destination writer or comparison mode");
        Put(*target,2,lower,3001,41);
        const auto originalNames=target->List().size();
        target.reset();
        const auto before=checkpoint();
        target=OpenBrowserImage(targetPath);
        Check(bool(target),"Could not reopen the APFS destination before copying");

        const auto copy=[&](const std::vector<Location>& entries){
            unsigned prompts=0;
            bool beganCopy=false;
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){beganCopy|=!progress.scanning;},
                          [&](const auto&){
                              ++prompts;
                              return Storage::ConflictChoice::ReplaceAll;
                          });
            Completion completion;
            const auto result=ExecuteInWorker(OrdinaryCopy{entries,Native(target,2)},job,completion);
            if(sensitive){
                if(result.code)throw HfsPlus::Error(result.message);
                Check(completion.complete&&prompts==1&&beganCopy,
                      "Case-sensitive APFS did not accept distinct names and an ordinary overwrite");
            }else{
                Check(result.code&&!result.partial&&!completion.complete&&
                      result.message.find("same APFS destination name")!=std::string::npos&&
                      !prompts&&!beganCopy,"An APFS collision reached overwrite handling or the copy phase");
            }
        };
        copy(selected);
        if(!sensitive){
            copy(moving);
            copy({Native(source,folder)});
        }
        target.reset();
        if(!sensitive)Check(checkpoint()==before,"An APFS name collision published a checkpoint");
        target=OpenBrowserImage(targetPath);
        Entry copied,other;
        Check(target&&target->FindChild(2,lower,copied),"Missing APFS destination file after reopening");
        if(sensitive){
            Check(target->FindChild(2,upper,other)&&copied.id!=other.id,
                  "Case-sensitive APFS merged distinct names");
            Verify(*target,copied.id,4097,3);
            Verify(*target,other.id,4111,11);
        }else{
            Check(target->List().size()==originalNames&&!target->FindChild(2,u"Collision subtree",other),
                  "Rejected APFS copy published a file or folder");
            Verify(*target,copied.id,3001,41);
        }
        if(external)fprintf(report,"PASS: macOS APFS compatibility writer rejects HFSX collisions before writing.\n");
    }
    selected.clear();
    moving.clear();
    source.reset();
    source=OpenBrowserImage(sourcePath);
    Entry a,b,tree,child,leaf;
    Check(source&&source->FindChild(2,lower,a)&&source->FindChild(2,upper,b)&&
          source->FindChild(2,u"Collision subtree",tree)&&source->FindChild(tree.id,u"Nested",child),
          "Rejected cut/paste removed an HFSX source entry");
    Verify(*source,a.id,4097,3);
    Verify(*source,b.id,4111,11);
    Verify(*source,independent,513,7);
    Verify(*source,nestedFirst,917,19);
    Verify(*source,nestedSecond,929,23);
    fprintf(report,"PASS: HFSX case collisions, including nested copies and cut/paste with Replace All, leave the APFS checkpoint and both source forks intact. Case-sensitive APFS keeps both names.\n");
}

void TestHfsCompressionWithResourceToApfs(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    using Mode=HfsPlus::Volume::CompressionMode;
    using HfsPlus::Bytes;
    const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    const std::array<std::u16string,2> names{u"Inline with resource",u"Maximum inline with resource"};
    std::array<Bytes,2> data{Bytes(18000,0x4a),Bytes(3786)};
    std::array<Bytes,2> resource{Bytes(173),Bytes(7209)};
    const HfsPlus::Attributes attributes{{u"user.transfer",{4,0,9}}};
    // Type 1 uses the full 3802-byte decmpfs attribute. Its encoded bytes
    // exceed the APFS writer's single-record capacity.
    uint32_t random=0x13426abd;
    for(auto& byte:data[1]){
        random^=random<<13;
        random^=random>>17;
        random^=random<<5;
        byte=uint8_t(random);
    }
    for(auto& fork:resource)
        for(size_t i=0;i<fork.size();++i)fork[i]=uint8_t(i*17+i/256);

    const auto verify=[&](Volume& volume,size_t i,bool compressed){
        Entry entry;
        Check(volume.FindChild(2,names[i],entry)&&entry.compressed==compressed,
              "Compression state differs after reopening");
        const Bytes* expected[]={&data[i],&resource[i]};
        for(unsigned fork=0;fork<2;++fork){
            Bytes actual(expected[fork]->size());
            Check(entry.forks[fork].size==actual.size(),"Compressed transfer changed a fork size");
            volume.Read(entry.id,fork,0,actual.data(),actual.size());
            Check(actual==*expected[fork],"Compressed transfer changed a logical fork");
        }
        auto metadata=volume.GetAttributes(entry.id);
        const auto found=metadata.find(u"user.transfer");
        Check(found!=metadata.end()&&found->second==attributes.begin()->second,
              "Compressed transfer lost an ordinary attribute");
        return entry.id;
    };
    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const auto base=root+"\\compressed-with-resource-"+std::to_string(index++);
        const auto sourcePath=Wide(base+"-source.dsk");
        {
            auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Compressed dual forks",true,kind);
            HfsPlus::Volume source(*disk,true);
            for(size_t i=0;i<names.size();++i)
                source.Put(2,names[i],data[i].size(),resource[i].size(),
                    [&](unsigned fork,uint64_t at,void* out,size_t size){
                        const auto& bytes=fork?resource[i]:data[i];
                        memcpy(out,bytes.data()+at,size);
                    },nullptr,false,nullptr,&attributes,i?Mode::StoredInline:Mode::Lzvn);
        }
        auto source=OpenBrowserImage(sourcePath);
        Check(bool(source),"Cannot reopen compressed HFS source");
        const std::vector<Location> inputs{Native(source,verify(*source,0,true)),
                                           Native(source,verify(*source,1,true))};
        for(bool external:{false,true}){
            if(external&&!fixture)continue;
            const auto path=Wide(base+(external?"-macos.img":"-native.img"));
            if(external){
                Check(CopyFileW(fixture,path.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
            }else{
                auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
                Storage::Job job;
                Apfs::Format(*disk,u"Compressed transfer destination",true,job);
            }
            auto target=OpenBrowserImage(path);
            Check(target&&target->Supports().create&&target->Writable()!=external,
                  "Wrong APFS writer for dual-fork copy");
            Put(*target,2,names[1],6301,59);
            unsigned overwritePrompts=0,fallbackPrompts=0;
            const auto transfer=[&](std::vector<Location> selected,Storage::ConflictChoice answer){
                auto operation=Submit(OrdinaryCopy{std::move(selected),Native(target,2)});
                auto job=operation.Job();
                const auto start=GetTickCount();
                uint64_t responded=0;
                while(!job->done()){
                    if(GetTickCount()-start>30000){
                        job->cancel();
                        throw HfsPlus::Error("Compressed dual-fork transfer timed out");
                    }
                    const auto prompt=job->conflictState();
                    if(!prompt.path.empty()&&responded!=prompt.id){
                        responded=prompt.id;
                        if(prompt.kind==Storage::CopyPrompt::Overwrite){
                            ++overwritePrompts;
                            job->resolve(prompt.id,answer);
                        }else{
                            ++fallbackPrompts;
                            job->resolve(prompt.id,Storage::ConflictChoice::Cancel);
                        }
                    }
                    Sleep(5);
                }
                return job->result();
            };
            auto result=transfer({inputs[1]},Storage::ConflictChoice::Cancel);
            Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial&&
                  overwritePrompts==1&&fallbackPrompts==0,"Declining the overwrite did not cancel before publication");
            target.reset();
            target=OpenBrowserImage(path);
            Entry original;
            Check(target&&target->FindChild(2,names[1],original)&&!original.compressed,
                  "Declining uncompressed copy lost the destination");
            Verify(*target,original.id,6301,59);
            result=transfer(inputs,Storage::ConflictChoice::Replace);
            Check(!result.code,result.message.c_str());
            Check(overwritePrompts==2&&fallbackPrompts==0,"Exact compression copy asked to discard compression");
            target.reset();
            target=OpenBrowserImage(path);
            Check(bool(target),"Cannot reopen dual-fork APFS target");
            const auto compressed=verify(*target,0,true);
            const auto header=source->Stat(verify(*source,0,true)).inlineCompression;
            Check(!header.empty()&&target->Stat(compressed).inlineCompression==header,
                  "APFS re-encoded the original inline compression");
            const auto full=verify(*target,1,true);
            const auto fullHeader=source->Stat(verify(*source,1,true)).inlineCompression;
            Check(fullHeader.size()==HfsPlus::InlineCompressionLimit&&
                  target->Stat(full).inlineCompression==fullHeader,
                  "APFS did not preserve the maximum-size compressed attribute");
            const auto returnedPath=path+L"-returned.dsk";
            auto returned=Image(returnedPath,kind);
            Storage::Job returnJob;
            Completion returnCompletion;
            const auto roundTrip=ExecuteInWorker(OrdinaryCopy{{Native(target,full)},Native(returned)},returnJob,returnCompletion);
            Check(!roundTrip.code&&returnCompletion.complete,roundTrip.message.c_str());
            returned.reset();
            returned=OpenBrowserImage(returnedPath);
            Check(returned&&returned->Stat(verify(*returned,1,true)).inlineCompression==fullHeader,
                  "Round trip re-encoded the streamed APFS compression attribute");
            target->Remove(full);
            target->Remove(compressed);
            target.reset();
            target=OpenBrowserImage(path);
            Entry removed;
            Check(target&&!target->FindChild(2,names[0],removed),
                  "Could not delete the compressed file with independent resource data");
            Check(!target->FindChild(2,names[1],removed),"Could not delete the APFS file with streamed compression");
            verify(*source,0,true);
            verify(*source,1,true);
        }
    }
    class ForkFixture final : public HfsPlus::Source {
        const Bytes& bytes_;
    public:
        explicit ForkFixture(const Bytes& bytes):bytes_(bytes){}
        uint64_t Size()const override{return bytes_.size();}
        bool Writable()const override{return false;}
        void Read(uint64_t at,void* output,size_t count)override{
            Check(at<=bytes_.size()&&count<=bytes_.size()-at,"Fixture fork read out of range");
            memcpy(output,bytes_.data()+at,count);
        }
        void Write(uint64_t,const void*,size_t)override{throw HfsPlus::Error("Read-only fixture fork");}
        void Flush()override{}
    };
    // LZFSE can match the repeated bytes across a distance beyond zlib's
    // window while keeping the encoded record small enough for an APFS leaf.
    const Bytes seed(data[1].begin(),data[1].begin()+2500);
    data[1].assign(62000,0);
    std::copy(seed.begin(),seed.end(),data[1].begin());
    std::copy(seed.begin(),seed.end(),data[1].begin()+40000);
    const auto apfsPath=Wide(root+"\\reverse-compressed-source.img");
    {
        auto disk=HfsPlus::CreateImage(apfsPath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"APFS dual forks",true,job);
        const Bytes empty;
        ForkFixture noData(empty);
        for(size_t i=0;i<names.size();++i){
            ForkFixture fork(resource[i]);
            HfsPlus::EncodedCompression encoded(data[i].size(),[&](uint64_t at,void* out,size_t count){
                memcpy(out,data[i].data()+at,count);
            },i?Mode::Lzfse:Mode::Zlib);
            const auto container=Apfs::Probe(*disk);
            Check(container&&!encoded.UsesResource(),"Cannot seed inline APFS compression fixture");
            Apfs::CreateCatalogFile(*disk,*container,2,names[i],noData,job,{},nullptr,false,
                                    &attributes,&fork,&encoded.Attribute());
        }
    }
    auto apfs=OpenBrowserImage(apfsPath);
    Check(bool(apfs),"Cannot reopen compressed APFS source");
    const std::vector<Location> reverseInputs{Native(apfs,verify(*apfs,0,true)),
                                               Native(apfs,verify(*apfs,1,true))};
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const auto path=Wide(root+"\\reverse-compressed-"+std::to_string(index++)+".dsk");
        auto target=Image(path,kind);
        Check(bool(target),"Cannot open reverse compression destination");
        Put(*target,2,names[1],6301,59);
        const auto transfer=[&](const std::vector<Location>& inputs,Storage::ConflictChoice answer){
            auto operation=Submit(OrdinaryCopy{inputs,Native(target)});
            auto job=operation.Job();
            const auto start=GetTickCount();
            uint64_t last=0;
            unsigned fallbacks=0;
            while(!job->done()){
                if(GetTickCount()-start>30000){
                    job->cancel();
                    throw HfsPlus::Error("Reverse compressed copy timed out");
                }
                const auto prompt=job->conflictState();
                if(!prompt.path.empty()&&prompt.id!=last){
                    last=prompt.id;
                    if(prompt.kind==Storage::CopyPrompt::Uncompressed){
                        ++fallbacks;
                        job->resolve(last,Storage::ConflictChoice::Cancel);
                    }else job->resolve(last,answer);
                }
                Sleep(5);
            }
            Check(!fallbacks,"Reverse copy unexpectedly asked to discard inline compression");
            return job->result();
        };
        auto result=transfer({reverseInputs[1]},Storage::ConflictChoice::Cancel);
        Check(result.outcome==Storage::Outcome::Cancelled&&!result.partial,
              "Cancelling the reverse overwrite did not cancel before publication");
        target.reset();
        target=OpenBrowserImage(path);
        Entry original;
        Check(target&&target->FindChild(2,names[1],original),"Cancelling the reverse overwrite lost the destination");
        Verify(*target,original.id,6301,59);
        result=transfer(reverseInputs,Storage::ConflictChoice::Replace);
        Check(!result.code,result.message.c_str());
        target.reset();
        target=OpenBrowserImage(path);
        Check(bool(target),"Cannot reopen HFS destination");
        for(size_t i=0;i<names.size();++i){
            const auto header=apfs->Stat(verify(*apfs,i,true)).inlineCompression;
            Check(!header.empty()&&target->Stat(verify(*target,i,true)).inlineCompression==header,
                  "HFS Plus re-encoded the original inline compression");
        }
    }
    verify(*apfs,0,true);
    verify(*apfs,1,true);
    fprintf(report,"PASS: APFS inline compression and genuine resource forks copy to HFS+ and both HFSX variants; exact encoded bytes survive reopening and cancellation preserves the destination.\n");
    fprintf(report,"PASS: compressed HFS+ and both HFSX variants preserve both forks and attributes on APFS after reopen; maximum inline compression survives as an APFS stream and returns byte for byte.\n");
    if(fixture)fprintf(report,"PASS: dual-fork compressed transfers also work on disposable macOS APFS fixtures.\n");
}

void EditApfsFixtureLeaf(const std::wstring& path,
                         const std::function<void(const Apfs::Node&,const uint8_t*,HfsPlus::Bytes&)>& edit)
{
    auto disk=HfsPlus::OpenImage(path.c_str(),true);
    const auto container=Apfs::Probe(*disk);
    Check(container&&container->volumes.size()==1,"Missing APFS fixture container");
    const auto volume=container->volumes.front();
    const Apfs::ObjectMap mappings(*disk,*container,container->objectMap,container->transaction);
    const auto volumeBlock=mappings.Lookup(volume);
    Check(bool(volumeBlock),"Missing APFS fixture volume mapping");
    const auto superblock=Apfs::ReadObject(*disk,*container,*volumeBlock,volume,container->transaction,13);
    const auto treeOid=Apfs::Le64(superblock.data()+136);
    const Apfs::ObjectMap objects(*disk,*container,Apfs::Le64(superblock.data()+128),container->transaction);
    const auto leafBlock=objects.Lookup(treeOid);
    Check(bool(leafBlock),"Missing APFS fixture catalog mapping");
    auto leaf=Apfs::ReadObject(*disk,*container,*leafBlock,treeOid,container->transaction,2,14);
    auto viewBytes=leaf;
    const auto* base=viewBytes.data();
    const Apfs::Node node(std::move(viewBytes),true,{});
    Check(!node.Level(),"Expected APFS fixture catalog to fit one leaf");
    edit(node,base,leaf);
    const auto checksum=Apfs::ObjectChecksum(leaf);
    for(unsigned i=0;i<8;++i)leaf[i]=uint8_t(checksum>>(8*i));
    disk->Write(container->offset+*leafBlock*container->blockSize,leaf.data(),leaf.size());
    disk->Flush();
}

void TestApfsImmutableCopy(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const auto sourcePath=Wide(root+"\\immutable-apfs.img");
    {
        auto disk=HfsPlus::CreateImage(sourcePath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Immutable APFS source",true,job);
    }
    auto source=OpenBrowserImage(sourcePath);
    Check(bool(source),"Cannot open immutable APFS fixture");
    const auto single=Put(*source,2,u"Locked single",7901,24);
    const auto original=Put(*source,2,u"Locked original",8303,25);
    source->MakeHardLink(original,2,u"Locked alias");
    const auto lock=[&](uint32_t id){
        auto entry=source->Stat(id);
        entry.protectedFile=true;
        source->SetBasicMetadata(id,entry);
    };
    lock(single);
    lock(original);
    Entry opaque;
    opaque.directory=true;
    opaque.name=u"Opaque directory";
    opaque.posix=PosixMetadata{123,456,0755,0,PosixMetadata::Opaque};
    const auto opaqueId=source->MakeDirectoryWithAttributes(2,opaque);
    Put(*source,opaqueId,u"Child",4097,65);
    source.reset();
    // Use both append-only flags, independently of immutability, on the single fixture file.
    EditApfsFixtureLeaf(sourcePath,[](const auto& node,const auto* base,auto& leaf){
        unsigned patched=0;
        for(size_t i=0;i<node.Count();++i){
            const auto record=node.At(i);
            if(record.key.size!=8||Apfs::Le64(record.key.data)>>60!=3||record.value.size<84||
               !(Apfs::Le32(record.value.data+68)&Apfs::UserImmutable))continue;
            auto* flags=leaf.data()+size_t(record.value.data-base)+68;
            const auto single=Apfs::Le32(record.value.data+56)==1;
            flags[0]=(single?4:2)|1;
            flags[2]=(single?4:2)|1;
            ++patched;
        }
        Check(patched==2,"Expected a single file and a hard-link inode");
    });
    source=OpenBrowserImage(sourcePath);
    Check(bool(source),"Cannot reopen immutable APFS source");
    std::vector<Location> inputs;
    const std::u16string names[]={u"Locked single",u"Locked original",u"Locked alias"};
    for(const auto& name:names){
        Entry file;
        Check(source->FindChild(2,name,file)&&file.protectedFile&&file.posix&&
              file.posix->protection==(inputs.empty()?(PosixMetadata::UserAppendOnly|PosixMetadata::SystemAppendOnly):
                                                        (PosixMetadata::UserImmutable|PosixMetadata::SystemImmutable)),
              "Missing APFS immutable fixture");
        inputs.push_back(Native(source,file.id));
    }
    Entry opaqueSource;
    Check(source->FindChild(2,opaque.name,opaqueSource)&&opaqueSource.posix&&
          opaqueSource.posix->flags==PosixMetadata::Opaque&&!opaqueSource.protectedFile,
          "Incorrect APFS opaque-directory fixture");
    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const auto path=Wide(root+"\\immutable-hfs-"+std::to_string(index++)+".dsk");
        auto target=Image(path,kind);
        Check(bool(target),"Cannot open immutable HFS destination");
        Put(*target,2,u"Locked single",4001,31);
        const auto copy=[&](bool stop){
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){
                if(stop&&progress.fileBytes)job.cancel();
            },[](const auto&){return Storage::ConflictChoice::Replace;});
            Completion completion;
            return ExecuteInWorker(OrdinaryCopy{inputs,Native(target)},job,completion);
        };
        const auto cancelled=copy(true);
        Check(cancelled.outcome==Storage::Outcome::Cancelled&&!cancelled.partial,
              "Immutable overwrite did not cancel before publication");
        target.reset();
        target=OpenBrowserImage(path);
        Entry old;
        Check(target&&target->FindChild(2,u"Locked single",old)&&!old.protectedFile,
              "Cancellation changed original HFS lock flags");
        Verify(*target,old.id,4001,31);
        Check(!target->FindChild(2,u"Locked original",old),"Cancellation published a new file");
        const auto copied=copy(false);
        Check(!copied.code,copied.message.c_str());
        target.reset();
        target=OpenBrowserImage(path);
        Check(bool(target),"Cannot reopen immutable HFS destination");
        uint32_t inode=0;
        for(size_t i=0;i<inputs.size();++i){
            Entry origin;
            Check(source->FindChild(2,names[i],origin),"Missing immutable source during verification");
            Entry entry;
            Check(target->FindChild(2,origin.name,entry)&&entry.protectedFile&&entry.posix&&
                  entry.posix->protection==origin.posix->protection&&
                  entry.posix->flags==(PosixMetadata::NoDump|PosixMetadata::Archived),
                  "Copy lost BSD flags after reopening");
            Verify(*target,entry.id,i?8303:7901,i?25:24);
            if(i==1)inode=entry.resolvedId;
            if(i)Check(entry.hardLink&&entry.resolvedId==inode&&entry.linkCount==2,
                       "Copied locked names do not share an inode");
            bool refused=false;
            try{target->Remove(entry.id);}catch(const std::exception&){refused=true;}
            Check(refused,"HFS allowed deletion of a protected entry");
        }
        Storage::Job job;
        Completion completion;
        const auto folderCopy=ExecuteInWorker(OrdinaryCopy{{Native(source,opaqueSource.id)},Native(target)},job,completion);
        Check(!folderCopy.code,folderCopy.message.c_str());
        target.reset();
        target=OpenBrowserImage(path);
        Entry folder,child;
        Check(target&&target->FindChild(2,opaque.name,folder)&&folder.posix&&
              folder.posix->flags==PosixMetadata::Opaque&&!folder.protectedFile&&
              target->FindChild(folder.id,u"Child",child),"Opaque directory changed after reopening");
        Verify(*target,child.id,4097,65);
        target->Rename(folder.id,2,u"Opaque renamed");
    }
    fprintf(report,"PASS: APFS append-only overwrite and new immutable hard-link copies keep HFS+/HFSX BSD protection, no-dump, archived and both forks after reopening; cancellation preserves the original.\n");
}

void TestHfsSystemImmutableToApfs(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const auto macos=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    const std::u16string names[]={u"System overwrite",u"System original",u"System alias",u"System folder"};
    const uint8_t system=PosixMetadata::SystemAppendOnly;
    const uint8_t both=PosixMetadata::SystemImmutable|PosixMetadata::UserImmutable|PosixMetadata::UserAppendOnly;
    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const auto base=root+"\\system-lock-copy-"+std::to_string(index++);
        const auto sourcePath=Wide(base+".dsk");
        {
            auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"System locks",true,kind);
            HfsPlus::Volume volume(*disk,true);
            Apfs::FinderBytes finder{};
            finder[0]=42;
            const auto put=[&](const std::u16string& name,BYTE seed,uint8_t flags){
                return volume.Put(2,name,5003,173,[seed](unsigned fork,uint64_t at,void* output,size_t count){
                    auto* bytes=static_cast<BYTE*>(output);
                    for(size_t i=0;i<count;++i)bytes[i]=BYTE((at+i)*7+fork*13+seed);
                },&finder,false,nullptr,nullptr,flags==both?HfsPlus::Volume::CompressionMode::Zlib:
                                                          HfsPlus::Volume::CompressionMode::Uncompressed,
                nullptr,PosixMetadata{123,456,0644,flags,PosixMetadata::NoDump|PosixMetadata::Archived});
            };
            put(names[0],77,system);
            const auto member=put(names[1],78,both);
            volume.MakeHardLink(member,2,names[2],false,true);
            const auto folder=volume.MakeDirectory(2,names[3]);
            volume.MakeDirectory(folder,u"Child");
            volume.SetDirectoryMetadata(folder,nullptr,nullptr,nullptr,PosixMetadata{123,456,0755,system,PosixMetadata::Opaque});
        }
        auto source=OpenBrowserImage(sourcePath);
        Check(bool(source),"Cannot reopen HFS system-lock fixture");
        std::vector<Location> inputs;
        for(size_t i=0;i<ARRAYSIZE(names);++i){
            Entry entry;
            Check(source->FindChild(2,names[i],entry)&&entry.posix&&
                  entry.posix->protection==(i==1||i==2?both:system),"Wrong HFS immutable fixture flags");
            inputs.push_back(Native(source,entry.id));
        }
        for(bool external:{false,true}){
            if(external&&!macos)continue;
            const auto path=Wide(base+(external?"-macos.img":"-native.img"));
            if(external){
                Check(CopyFileW(macos,path.c_str(),TRUE)!=FALSE,"Cannot copy the shared macOS APFS fixture");
            }else{
                auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
                Storage::Job job;
                Apfs::Format(*disk,u"System lock destination",true,job);
            }
            auto target=OpenBrowserImage(path);
            Check(target&&target->Supports().create&&target->Writable()!=external,"Wrong APFS writer for lock test");
            Put(*target,2,names[0],4801,32);
            const auto run=[&](bool cancel){
                Storage::Job job;
                job.transport([&](const Storage::Progress& p){
                    if(cancel&&p.fileBytes)job.cancel();
                },[](const auto&){return Storage::ConflictChoice::Replace;});
                Completion completion;
                return ExecuteInWorker(OrdinaryCopy{inputs,Native(target)},job,completion);
            };
            const auto stopped=run(true);
            Check(stopped.outcome==Storage::Outcome::Cancelled&&!stopped.partial,
                  stopped.message.empty()?"Expected cancellation during immutable overwrite":stopped.message.c_str());
            target.reset();
            target=OpenBrowserImage(path);
            Entry current;
            Check(target&&target->FindChild(2,names[0],current)&&!current.protectedFile,
                  "Cancelled system-lock copy changed the destination flags");
            Verify(*target,current.id,4801,32);
            Check(!target->FindChild(2,names[1],current),"Cancelled copy published a new alias");
            const auto result=run(false);
            Check(!result.code,result.message.c_str());
            target.reset();
            target=OpenBrowserImage(path);
            Check(bool(target),"Cannot reopen APFS system-lock destination");
            uint32_t inode=0;
            for(size_t i=0;i<ARRAYSIZE(names);++i){
                Check(target->FindChild(2,names[i],current)&&current.protectedFile&&current.posix&&
                      current.posix->protection==(i==1||i==2?both:system)&&
                      current.posix->flags==(i==3?PosixMetadata::Opaque:(PosixMetadata::NoDump|PosixMetadata::Archived)),
                      "APFS copy lost exact BSD flags after reopening");
                if(i==3){
                    Entry child;
                    Check(current.directory&&target->FindChild(current.id,u"Child",child),
                          "APFS folder locked before its children were copied");
                    continue;
                }
                Verify(*target,current.id,5003,i?78:77);
                if(i==1)inode=current.resolvedId;
                if(i)Check(current.hardLink&&current.compressed&&current.resolvedId==inode&&current.linkCount==2,
                           "APFS system-immutable aliases did not share one inode");
                bool refused=false;
                try{target->Rename(current.id,2,u"Invalid unlocked rename");}catch(const std::exception&){refused=true;}
                Check(refused,"APFS allowed rename of a system-immutable file");
            }
        }
    }
    fprintf(report,"PASS: HFS+/HFSX BSD protection, no-dump, archived and opaque flags survive APFS copy, overwrite, hard-link creation and reopen; cancelling preserves the original destination.\n");
    if(macos)fprintf(report,"PASS: system-immutable copies also passed on the macOS APFS compatibility writer.\n");
}

void TestCrossFormatTimestamps(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    using Dates=std::array<uint64_t,2>;
    constexpr auto epoch=HfsPlus::Time::Epoch, maximum=HfsPlus::Time::Maximum;
    const Dates legacy{0,UINT32_MAX}, expanded{epoch,maximum};
    const Dates tooLate{UINT64_MAX,maximum+1}, fractional{epoch-1,epoch};
    const Dates original{epoch+10,epoch+20};
    const auto extraDates=[=](const Dates& dates,BYTE seed){
        return HfsPlus::AdditionalFileDates{dates[1]>UINT32_MAX?dates[1]:epoch+1000+seed,
                                           epoch+2000+seed};
    };
    const auto formatApfs=[](const std::wstring& path){
        auto disk=HfsPlus::CreateImage(path.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Timestamp transfer",true,job);
    };
    const auto dated=[&](Volume& volume,const std::u16string& name,const Dates& dates,BYTE seed){
        Entry entry;
        entry.name=name;
        entry.datesValid=true;
        entry.created=dates[0];
        entry.modified=dates[1];
        entry.additionalDates=extraDates(dates,seed);
        entry.finder[0]=42;
        entry.forks[0].size=5003;
        entry.forks[1].size=173;
        return volume.Put(2,entry,[seed](unsigned fork,uint64_t at,void* output,size_t count){
            auto* bytes=static_cast<BYTE*>(output);
            for(size_t i=0;i<count;++i)bytes[i]=BYTE((at+i)*7+fork*13+seed);
        },false);
    };
    const auto verify=[&](Volume& volume,const std::u16string& name,const Dates& dates,BYTE seed){
        Entry entry;
        Check(volume.FindChild(2,name,entry)&&entry.datesValid&&entry.created==dates[0]&&entry.modified==dates[1],
              "Creation or modification timestamp changed after reopening");
        const auto expected=extraDates(dates,seed);
        Check(entry.additionalDates.accessed==expected.accessed&&entry.additionalDates.changed==expected.changed,
              "Access or attribute-change timestamp changed after reopening");
        Verify(volume,entry.id,5003,seed);
        return entry;
    };
    const auto sourcePath=Wide(root+"\\timestamp-apfs-source.img");
    formatApfs(sourcePath);
    {
        auto source=OpenBrowserImage(sourcePath);
        Check(bool(source),"Cannot open APFS timestamp source");
        dated(*source,u"Legacy limits",legacy,47);
        dated(*source,u"Expanded limits",expanded,48);
        dated(*source,u"Too late",Dates{epoch,tooLate[1]},49);
        dated(*source,u"Fractional",Dates{epoch,epoch},50);
    }
    EditApfsFixtureLeaf(sourcePath,[](const auto& node,const auto* base,auto& leaf){
        unsigned changed=0;
        for(size_t i=0;i<node.Count();++i){
            const auto record=node.At(i);
            if(record.key.size!=8||Apfs::Le64(record.key.data)>>60!=3||record.value.size<84||
               (Apfs::Le16(record.value.data+80)&0170000)!=0100000||
               Apfs::Le64(record.value.data+16))continue;
            auto* fields=leaf.data()+(record.value.data-base);
            const auto modification=Apfs::Le64(record.value.data+24);
            if(modification==(UINT64_C(1)+UINT32_MAX)*1000000000){
                const auto before1904=uint64_t(-int64_t(HfsPlus::Time::Epoch)*1000000000-1);
                Apfs::WriterDisk::Set(leaf,size_t(fields-leaf.data())+16,before1904,8);
                ++changed;
                continue;
            }
            if(modification)continue;
            Apfs::WriterDisk::Set(leaf,size_t(fields-leaf.data())+16,UINT64_MAX,8); // -1 ns
            Apfs::WriterDisk::Set(leaf,size_t(fields-leaf.data())+24,1,8);
            for(unsigned offset:{32,40})
                Apfs::WriterDisk::Set(leaf,size_t(fields-leaf.data())+offset,Apfs::Le64(record.value.data+offset)+1,8);
            ++changed;
        }
        Check(changed==2,"Expected both signed fractional timestamp fixtures");
    });
    auto source=OpenBrowserImage(sourcePath);
    Check(bool(source),"Cannot reopen APFS date fixture");
    verify(*source,u"Legacy limits",legacy,47);
    verify(*source,u"Expanded limits",expanded,48);
    verify(*source,u"Too late",tooLate,49);
    verify(*source,u"Fractional",fractional,50);

    const auto transfer=[](const std::shared_ptr<Volume>& from,const std::shared_ptr<Volume>& to,
                           const std::vector<std::u16string>& names,bool cut,unsigned& prompts,bool& rounded){
        std::vector<Location> inputs;
        for(const auto& name:names){
            Entry entry;
            Check(from->FindChild(2,name,entry),"Missing timestamp transfer source");
            auto location=Native(from,entry.id);
            location.cut=cut;
            inputs.push_back(std::move(location));
        }
        Storage::Job job;
        job.transport({},[&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
        Completion completion;
        const auto result=ExecuteInWorker(OrdinaryCopy{inputs,Native(to)},job,completion);
        rounded=job.progress().timestampPrecisionReduced;
        return result;
    };
    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        for(const bool wide:{false,true}){
            const auto base=root+"\\timestamp-copy-"+std::to_string(index++);
            const auto hfsPath=Wide(base+".dsk");
            {
                auto disk=HfsPlus::CreateImage(hfsPath.c_str(),16*1024*1024);
                HfsPlus::Volume::Format(*disk,u"Timestamp transfer",true,kind,wide);
            }
            const auto* valid=wide?u"Expanded limits":u"Legacy limits";
            const auto* invalid=wide?u"Too late":u"Expanded limits";
            const auto validDates=wide?expanded:legacy;
            const BYTE seed=wide?48:47;
            auto hfs=OpenBrowserImage(hfsPath);
            Check(bool(hfs),"Cannot open HFS date destination");
            dated(*hfs,valid,original,32);
            dated(*hfs,invalid,original,33);
            unsigned prompts=0;
            bool rounded=false;
            const std::vector<std::u16string> selected{valid,invalid,u"Fractional"};
            const auto copied=transfer(source,hfs,selected,false,prompts,rounded);
            Check(!copied.code&&prompts==2,copied.message.c_str());
            Check(rounded,"Missing native timestamp precision notice");
            hfs.reset();
            hfs=OpenBrowserImage(hfsPath);
            Check(bool(hfs),"Cannot reopen timestamp-rich HFS copy");
            verify(*hfs,valid,validDates,seed);
            Entry projected;
            const auto limit=wide?maximum:uint64_t(UINT32_MAX);
            Check(hfs->FindChild(2,invalid,projected)&&projected.modified==limit&&
                  projected.additionalDates.accessed==limit&&(!wide||projected.created==epoch),"HFS timestamp did not use the representable boundary");
            verify(*hfs,u"Fractional",wide?Dates{epoch,epoch}:fractional,50);

            const auto macos=_wgetenv(L"HFV_APFS_TEST_IMAGE");
            for(bool external:{false,true}){
                if(external&&!macos)continue;
                const auto apfsPath=Wide(base+(external?"-macos.img":"-native.img"));
                if(external){
                    Check(CopyFileW(macos,apfsPath.c_str(),TRUE)!=FALSE,"Cannot copy APFS timestamp fixture");
                }else formatApfs(apfsPath);
                auto apfs=OpenBrowserImage(apfsPath);
                Check(apfs&&apfs->Supports().create&&apfs->Writable()!=external,"Wrong APFS timestamp writer");
                dated(*apfs,valid,original,34);
                const auto before=prompts;
                const auto returned=transfer(hfs,apfs,selected,false,prompts,rounded);
                Check(!returned.code&&prompts==before+1,returned.message.c_str());
                Check(!rounded,"Copying whole-second HFS dates to APFS should not report precision loss");
                apfs.reset();
                apfs=OpenBrowserImage(apfsPath);
                Check(bool(apfs),"Cannot reopen APFS timestamp copy");
                verify(*apfs,valid,validDates,seed);
                const auto checkExact=[&](const auto* name,const Dates& dates,BYTE seed){
                    const auto expected=verify(*source,name,dates,seed);
                    const auto actual=verify(*apfs,name,dates,seed);
                    Check(expected.apfsTimestamps&&actual.apfsTimestamps==expected.apfsTimestamps,
                          "APFS nanosecond timestamps were not restored after reopening");
                };
                checkExact(invalid,wide?tooLate:expanded,wide?49:48);
                checkExact(u"Fractional",fractional,50);
                const auto rewriteHfsDate=[&](uint64_t modified){
                    Entry entry;
                    Check(hfs->FindChild(2,u"Fractional",entry),"Missing HFS timestamp edit fixture");
                    entry.modified=modified;
                    entry.attributes=hfs->GetAttributes(entry.id);
                    entry.attributesKnown=true;
                    hfs->Put(2,entry,[](unsigned fork,uint64_t at,void* output,size_t count){
                        auto* bytes=static_cast<BYTE*>(output);
                        for(size_t i=0;i<count;++i)bytes[i]=BYTE((at+i)*7+fork*13+50);
                    },true);
                    hfs.reset();
                    hfs=OpenBrowserImage(hfsPath);
                    Check(bool(hfs),"Cannot reopen HFS timestamp edit fixture");
                };
                constexpr auto edited=epoch+123;
                rewriteHfsDate(edited);
                const auto changed=transfer(hfs,apfs,{u"Fractional"},false,prompts,rounded);
                Check(!changed.code,changed.message.c_str());
                apfs.reset();
                apfs=OpenBrowserImage(apfsPath);
                Check(bool(apfs),"Cannot reopen APFS after copying edited timestamps");
                const auto actual=verify(*apfs,u"Fractional",Dates{epoch-1,edited},50);
                auto expected=verify(*source,u"Fractional",fractional,50).apfsTimestamps;
                Check(bool(expected),"Missing source APFS timestamps");
                (*expected)[1]=UINT64_C(123)*1000000000;
                Check(actual.apfsTimestamps==expected,"Editing modification time discarded untouched APFS timestamps");
                rewriteHfsDate(epoch);
            }
        }
    }
    fprintf(report,"PASS: APFS and HFS+/HFSX creation, modification, access and attribute-change dates and both forks survive copies and overwrites after reopening. HFS Plus projects native dates to whole seconds and its supported range; exact APFS dates return after reopening.\n");
}

void TestSparseApfsCopy(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    constexpr uint64_t logical=8*1024*1024, resourceStored=24*1024+7, resourceHole=28*1024, resource=32*1024+7;
    constexpr auto name=u"Sparse dual-fork file";
    const auto sourcePath=Wide(root+"\\sparse-apfs-source.img");
    {
        auto disk=HfsPlus::CreateImage(sourcePath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Sparse transfer",true,job);
    }
    {
        auto mounted=OpenBrowserImage(sourcePath);
        Check(bool(mounted),"Cannot open sparse APFS test source");
        Entry entry;
        entry.name=name;
        entry.forks[0].size=4096;
        entry.forks[1].size=resourceStored;
        mounted->Put(2,entry,[](unsigned fork,uint64_t at,void* out,size_t count){
            auto* bytes=static_cast<BYTE*>(out);
            for(size_t i=0;i<count;++i)bytes[i]=BYTE((at+i)*7+fork*13+24);
        },false);
    }
    // Append holes to both streams without changing physical allocation.
    EditApfsFixtureLeaf(sourcePath,[=](const auto& node,const auto* base,auto& leaf){
        unsigned modified=0;
        uint64_t stream=0,resourceStream=0;
        for(size_t i=0;i<node.Count();++i){
            const auto rec=node.At(i);
            if(rec.key.size>10&&Apfs::Le64(rec.key.data)>>60==4&&rec.value.size==52&&
               std::string_view(reinterpret_cast<const char*>(rec.key.data+10),rec.key.size-11)=="com.apple.ResourceFork"){
                resourceStream=Apfs::Le64(rec.value.data+4);
                Apfs::WriterDisk::Set(leaf,size_t(rec.value.data-base)+12,resource,8);
            }
            if(rec.key.size!=8||Apfs::Le64(rec.key.data)>>60!=3||rec.value.size<96||
               (Apfs::Le16(rec.value.data+80)&0170000)!=0100000)continue;
            const auto fields=Apfs::Le16(rec.value.data+92);
            size_t offset=96+4*size_t(fields);
            for(unsigned i=0;i<fields;++i){
                const auto* field=rec.value.data+96+i*4;
                const auto size=Apfs::Le16(field+2);
                Check(offset<=rec.value.size&&size<=rec.value.size-offset,"Invalid sparse fixture inode");
                if(field[0]==8){
                    Check(size==40&&Apfs::Le64(rec.value.data+offset)==4096,"Unexpected sparse fixture stream");
                    auto* length=leaf.data()+(rec.value.data-base)+offset;
                    for(unsigned b=0;b<8;++b)length[b]=uint8_t(logical>>(8*b));
                    stream=Apfs::Le64(rec.value.data+8);
                    ++modified;
                }
                offset+=(size+7)&~size_t(7);
            }
        }
        Check(modified==1&&stream&&resourceStream,"Missing sparse fixture streams");
        namespace Disk=Apfs::WriterDisk;
        std::vector<Disk::Record> records;
        unsigned addedHoles=0;
        for(size_t i=0;i<node.Count();++i){
            const auto rec=node.At(i);
            const auto key=leaf.data()+(rec.key.data-base);
            const auto value=leaf.data()+(rec.value.data-base);
            records.push_back({HfsPlus::Bytes(key,key+rec.key.size),HfsPlus::Bytes(value,value+rec.value.size)});
            if(rec.key.size!=16)continue;
            const auto extentKey=Apfs::Le64(key);
            const bool resourceExtent=extentKey==((uint64_t(8)<<60)|resourceStream);
            if(!resourceExtent&&extentKey!=((uint64_t(8)<<60)|stream))continue;
            const auto allocated=resourceExtent?resourceHole:4096;
            const auto end=resourceExtent?36*1024:logical;
            Check(Apfs::Le64(key+8)==0&&rec.value.size==24&&Apfs::Le64(value)==allocated,
                  "Unexpected allocated extent in sparse fixture");
            Disk::Record hole{records.back().key,HfsPlus::Bytes(24)};
            Disk::Set(hole.key,8,allocated,8);
            Disk::Set(hole.value,0,end-allocated,8);
            records.push_back(std::move(hole));
            ++addedHoles;
        }
        Check(addedHoles==2,"Missing sparse fixture extents");
        const auto rebuilt=Disk::EncodeLeaf(records,Apfs::Le32(leaf.data()+leaf.size()-40));
        std::copy(rebuilt.begin()+32,rebuilt.end(),leaf.begin()+32);
    });
    const auto verifySparse=[&](Volume& volume){
        Entry entry;
        Check(volume.FindChild(2,name,entry)&&entry.forks[0].size==logical&&entry.forks[1].size==resource,
              "Sparse copy lost a fork or changed its length");
        std::vector<BYTE> bytes(256*1024);
        for(unsigned fork=0;fork<2;++fork){
            for(uint64_t at=0;at<entry.forks[fork].size;at+=bytes.size()){
                const size_t count=size_t((std::min)(uint64_t(bytes.size()),entry.forks[fork].size-at));
                volume.Read(entry.id,fork,at,bytes.data(),count);
                for(size_t i=0;i<count;++i){
                    const BYTE expected=(at+i<(fork?resourceStored:4096))?BYTE((at+i)*7+fork*13+24):0;
                    Check(bytes[i]==expected,"Sparse copy changed payload, hole or resource bytes");
                }
            }
        }
        return entry;
    };
    unsigned index=0;
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const auto path=Wide(root+"\\sparse-hfs-"+std::to_string(index++)+".dsk");
        {
            auto disk=HfsPlus::CreateImage(path.c_str(),64*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Nearly full HFS",true,kind);
        }
        auto target=OpenBrowserImage(path);
        auto source=OpenBrowserImage(sourcePath);
        Check(target&&source&&source->Writable(),"Sparse fixture must be eligible for cut/paste");
        const auto sparse=verifySparse(*source);
        const auto holes=source->SparseRanges(sparse.id);
        Check(holes[0]==std::vector<SparseRange>{{4096,logical-4096}}&&
              holes[1]==std::vector<SparseRange>{{resourceHole,resource-resourceHole}},
              "APFS sparse ranges did not describe both forks");
        Put(*target,2,name,131072,91);
        const auto filler=Put(*target,2,u"Space filler",target->FreeBytes()-logical-8192);
        const auto free=target->FreeBytes();
        Check(free>=logical&&free<logical+resource,"Failure fixture must run out at resource allocation");
        const auto run=[&](bool cut){
            auto selection=Native(source,sparse.id);
            selection.cut=cut;
            unsigned prompts=0;
            uint64_t read=0;
            Storage::Job job;
            job.transport([&](const Storage::Progress& progress){read=(std::max)(read,uint64_t(progress.fileBytes));},
                          [&](const auto&){++prompts;return Storage::ConflictChoice::Replace;});
            Completion completion;
            const auto result=ExecuteInWorker(OrdinaryCopy{{selection},Native(target)},job,completion);
            Check(prompts==1,"Sparse overwrite must request approval");
            Check(job.progress().sparseHolesAllocated==!result.code,
                  "Sparse allocation notice must only report a published copy");
            return std::make_pair(result,read);
        };
        for(bool cut:{false,true}){
            const auto [result,read]=run(cut);
            Check(result.outcome==Storage::Outcome::Failed&&!result.partial&&!result.moved&&read==0&&
                  result.message.find("Not enough free HFS+ blocks")!=std::string::npos,
                  result.message.empty()?"Expected out-of-space failure before streaming either fork":result.message.c_str());
            target.reset();
            source.reset();
            target=OpenBrowserImage(path);
            source=OpenBrowserImage(sourcePath);
            Entry old;
            Check(target&&source&&target->FreeBytes()==free&&target->FindChild(2,name,old),
                  "Failed sparse overwrite lost destination data or free space");
            Verify(*target,old.id,131072,91);
            Check(verifySparse(*source).identity==sparse.identity,"Failed cut/paste changed the source");
        }
        target->Remove(filler);
        Put(*target,2,u"Smaller filler",target->FreeBytes()-logical-resource-128*1024);
        Check(target->FreeBytes()<logical+resource+256*1024,"Success fixture has too much free space");
        const auto [result,read]=run(false);
        Check(!result.code,result.message.c_str());
        target.reset();
        target=OpenBrowserImage(path);
        Check(bool(target),"Cannot reopen the copied sparse file");
        const auto copied=verifySparse(*target);
        const auto attributes=target->GetAttributes(copied.id);
        const auto layout=attributes.find(u"org.hfvexplorer.apfs-sparse-ranges");
        const HfsPlus::Bytes expected{
            'H','F','V','S',1,0,0,0,
            0,0,128,0,0,0,0,0, 1,0,0,0,0,0,0,0,
            0,16,0,0,0,0,0,0, 0,240,127,0,0,0,0,0,
            7,128,0,0,0,0,0,0, 1,0,0,0,0,0,0,0,
            0,112,0,0,0,0,0,0, 7,16,0,0,0,0,0,0
        };
        Check(layout!=attributes.end()&&HfsPlus::Bytes(layout->second)==expected,
              "HFS+ copy did not retain the exact sparse fork layout after reopening");
        auto candidate=target->Stat(copied.id);
        candidate.attributes=attributes;
        Storage::Job validation;
        const Reader readSparseCopy=[&](unsigned fork,uint64_t offset,void* bytes,size_t size){
            target->Read(copied.id,fork,offset,bytes,size);
        };
        Check(ValidateApfsSparseLayout(candidate,readSparseCopy,validation)==holes,
              "Valid sparse layout could not be recovered from both reopened forks");
        const auto stale=ValidateApfsSparseLayout(candidate,[&](unsigned fork,uint64_t offset,void* bytes,size_t size){
            readSparseCopy(fork,offset,bytes,size);
            if(fork==0&&offset==4096&&size)static_cast<uint8_t*>(bytes)[0]=1;
        },validation);
        Check(stale[0].empty()&&stale[1].empty(),"Sparse marker would discard a later nonzero write");

        auto bad=expected;
        bad[39]=0xff; // Make the first range length exceed the entire fork.
        candidate.attributes[ApfsSparseLayoutAttribute]=std::move(bad);
        bool readPayload=false;
        const auto malformed=ValidateApfsSparseLayout(candidate,[&](unsigned,uint64_t,void*,size_t){
            readPayload=true;
        },validation);
        Check(!readPayload&&malformed[0].empty()&&malformed[1].empty(),
              "Malformed sparse layout reached the payload reader");

        const auto nativePath=path+L".apfs";
        {
            auto disk=HfsPlus::CreateImage(nativePath.c_str(),32*1024*1024);
            Storage::Job format;
            Apfs::Format(*disk,u"Restored sparse forks",true,format);
        }
        const auto macos=_wgetenv(L"HFV_APFS_TEST_IMAGE");
        const auto macosPath=path+L"-macos.apfs";
        if(macos)Check(CopyFileW(macos,macosPath.c_str(),TRUE)!=FALSE,"Cannot copy the shared APFS fixture");
        for(const auto& returnedPath:{nativePath,macos?macosPath:L""}){
            if(returnedPath.empty())continue;
            auto returned=OpenBrowserImage(returnedPath);
            Check(returned&&returned->Supports().create,"Cannot mount writable sparse round-trip target");
            Storage::Job copy;
            Completion completion;
            const auto restored=ExecuteInWorker(OrdinaryCopy{{Native(target,copied.id)},Native(returned)},copy,completion);
            Check(!restored.code&&completion.complete,restored.message.c_str());
            returned.reset();
            returned=OpenBrowserImage(returnedPath);
            Check(returned&&returned->Supports().create,"Sparse restoration made APFS unwritable after reopening");
            const auto restoredEntry=verifySparse(*returned);
            Check(returned->SparseRanges(restoredEntry.id)==holes,
                  "Reopened APFS copy did not restore the exact holes in both forks");
        }
    }
    {
        auto source=OpenBrowserImage(sourcePath);
        Check(bool(source),"Cannot reopen sparse classic archive fixture");
        const auto sparse=verifySparse(*source);
        const auto holes=source->SparseRanges(sparse.id);
        const auto classicPath=root+"\\sparse-archive.dsk";
        auto classic=ClassicImage(classicPath,16*1024*1024);
        auto archive=Submit(OrdinaryCopy{{Native(source,sparse.id)},Native(classic)});
        Conflict(archive);
        const auto prompt=archive.Job()->conflictState();
        Check(prompt.kind==Storage::CopyPrompt::ClassicMetadataLoss&&
              prompt.path.find("Sparse allocation")!=std::string::npos,"Classic archive omitted sparse allocation");
        archive.Job()->resolve(prompt.id,Storage::ConflictChoice::ReplaceAll);
        Wait(archive);
        const auto archived=archive.Job()->result();
        Check(!archived.code&&archive.Snapshot().complete,archived.message.c_str());
        classic.reset();
        classic=OpenBrowserImage(Wide(classicPath));
        Entry dense;
        Check(classic&&classic->FindChild(2,sparse.name,dense),"Missing dense classic sparse archive");
        const auto returnedPath=Wide(root+"\\sparse-classic-return.img");
        {
            auto disk=HfsPlus::CreateImage(returnedPath.c_str(),32*1024*1024);
            Storage::Job format;
            Apfs::Format(*disk,u"Classic sparse return",true,format);
        }
        auto returned=OpenBrowserImage(returnedPath);
        auto restore=Submit(OrdinaryCopy{{Native(classic,dense.id)},Native(returned)});
        Wait(restore);
        const auto restored=restore.Job()->result();
        Check(!restored.code&&restore.Snapshot().complete,restored.message.c_str());
        returned.reset();
        returned=OpenBrowserImage(returnedPath);
        Check(bool(returned),"Cannot reopen the classic sparse return");
        const auto actual=verifySparse(*returned);
        Check(returned->SparseRanges(actual.id)==holes,"Classic archive did not restore holes in both forks");
        fprintf(report,"PASS: classic HFS archives preserve APFS sparse data and resource layout across reopening.\n");
    }
    fprintf(report,"PASS: sparse APFS data and resource forks survive HFS+ and HFSX reopen with a dense allocation notice; out-of-space copy and cut/paste stop before reading payload and retain both source and original destination forks and free space.\n");
}

void TestApfsToHfsCompression(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    using Bytes=HfsPlus::Bytes;
    using Mode=HfsPlus::Volume::CompressionMode;
    const auto sourcePath=Wide(root+"\\compressed-source.img");
    {
        auto disk=HfsPlus::CreateImage(sourcePath.c_str(),32*1024*1024);
        Storage::Job job;
        Apfs::Format(*disk,u"Compressed APFS source",true,job);
    }

    const HfsPlus::Attributes metadata{{u"user.transfer",{4,0,9}}};
    const std::array<std::u16string,2> names{u"Inline compressed",u"Resource compressed"};
    std::array<Bytes,2> contents{Bytes(1500),Bytes(2*65536+91)};
    {
        auto source=OpenBrowserImage(sourcePath);
        Check(source&&source->Supports().CanEditCompression(),"Cannot seed compressed APFS source");
        for(size_t i=0;i<names.size();++i){
            auto& bytes=contents[i];
            for(size_t j=0;j<bytes.size();++j)bytes[j]=uint8_t(j%251+j/65536);
            Entry entry;
            entry.name=names[i];
            entry.forks[0].size=bytes.size();
            entry.attributes=metadata;
            entry.attributesKnown=true;
            const auto id=source->Put(2,entry,[&](unsigned,uint64_t at,void* data,size_t count){
                memcpy(data,bytes.data()+at,count);
            },false);
            Storage::Job job;
            source->SetCompressionMethodForJob(id,i?Mode::Lzvn:Mode::Zlib,{},job);
        }
    }
    auto source=OpenBrowserImage(sourcePath);
    Check(bool(source),"Cannot reopen compressed APFS source");
    std::vector<Location> inputs;
    for(const auto& name:names){
        Entry entry;
        Check(source->FindChild(2,name,entry)&&entry.compressed&&!entry.forks[1].size,
              "APFS source file was not stored compressed");
        inputs.push_back(Native(source,entry.id));
    }

    std::shared_ptr<Volume> apple;
    Bytes appleInline;
    const auto fixture=_wgetenv(L"HFV_APFS_TEST_IMAGE");
    if(fixture){
        auto disk=HfsPlus::OpenImage(fixture,false);
        apple=Apfs::Open(disk,"macos-apfs-compression-source");
        Entry entry;
        Check(apple&&apple->FindChild(2,u"decmpfs.txt",entry)&&entry.compressed&&
              entry.forks[0].size==112000&&!entry.forks[1].size,
              "Missing compressed file in the shared macOS APFS image");
        appleInline=apple->Stat(entry.id).inlineCompression;
        inputs.push_back(Native(apple,entry.id));
    }

    Entry resourceEntry;
    Check(source->FindChild(2,names[1],resourceEntry),"Missing source compression resource");
    resourceEntry=source->Stat(resourceEntry.id);
    Check(resourceEntry.compressionResourceSize&&resourceEntry.resourceCompressionHeader.size()==16,
          "Missing raw decmpfs metadata on the source");
    Bytes stored(size_t(resourceEntry.compressionResourceSize));
    source->ReadCompressionResource(resourceEntry.id,0,stored.data(),stored.size());
    unsigned index=0;
    std::wstring path;
    for(auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        path=Wide(root+"\\compressed-destination-"+std::to_string(index++)+".dsk");
        {
            auto disk=HfsPlus::CreateImage(path.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Compression destination",true,kind);
        }
        auto target=OpenBrowserImage(path);
        Check(target&&VolumeSession::VolumeSessionPidForTest(target),"Missing HFS compression worker");
        const auto copy=[&](const std::vector<Location>& files,Storage::Job& job){
            Completion completion;
            auto result=ExecuteInWorker(OrdinaryCopy{files,Native(target,2)},job,completion);
            if(!result.code)Check(completion.complete,"Incomplete compressed-file copy");
            return result;
        };
        const auto original=Put(*target,2,names[1],4096);
        const auto pid=VolumeSession::VolumeSessionPidForTest(target);
        Storage::Job cancel;
        cancel.transport([&](const Storage::Progress& progress){
            if(progress.fileBytes)cancel.cancel();
        },[](const auto&){return Storage::ConflictChoice::Replace;});
        Check(copy({inputs[1]},cancel).outcome==Storage::Outcome::Cancelled&&
              VolumeSession::VolumeSessionPidForTest(target)==pid&&!target->Stat(original).compressed,
              "Cancelled compressed overwrite did not retain the original file and worker");
        Verify(*target,original,4096);
        Storage::Job job;
        job.transport({},[](const auto&){return Storage::ConflictChoice::Replace;});
        const auto result=copy(inputs,job);
        Check(!result.code,result.message.c_str());
        target.reset();

        // Inspect the stored decmpfs headers as well as logical fork readback.
        auto disk=HfsPlus::OpenImage(path.c_str(),false);
        HfsPlus::Volume reopened(*disk,false);
        for(size_t i=0;i<inputs.size();++i){
            const auto name=i<names.size()?names[i]:u"decmpfs.txt";
            const auto length=i<contents.size()?contents[i].size():112000;
            HfsPlus::Entry entry;
            Check(reopened.FindChild(2,name,entry)&&entry.compressed&&
                  entry.forks[0].size==length&&!entry.forks[1].size,
                  "Compressed copy lost its encoding or logical fork size after reopening");
            auto attributes=reopened.GetAttributes(entry.id);
            const auto header=attributes.find(u"com.apple.decmpfs");
            Check(header!=attributes.end(),"Destination has no decmpfs header");
            if(i==0)Check(header->second.size()>16,"Destination lost inline compression");
            else if(i==1){
                Check(Bytes(header->second)==resourceEntry.resourceCompressionHeader&&
                      reopened.CompressionResourceSize(entry.id)==stored.size(),
                      "Destination re-encoded the resource compression header");
                Bytes actual(stored.size());
                reopened.ReadCompressionResource(entry.id,0,actual.data(),actual.size());
                Check(actual==stored,"Destination re-encoded the stored compression resource");
            }
            else if(appleInline.empty())Check(header->second.size()==16,"Destination lost resource compression");
            else Check(Bytes(header->second)==appleInline,"Destination re-encoded the macOS inline stream");
            attributes.erase(header);
            attributes.erase(MacBrowser::ApfsTimesAttribute);
            if(i<names.size())Check(attributes==metadata,"Compression copy lost an ordinary extended attribute");
            Bytes readback(length);
            Check(reopened.Read(entry.id,0,0,readback.data(),readback.size())==length,
                  "Short read from reopened compressed copy");
            if(i<contents.size())Check(readback==contents[i],"Compressed copy changed logical bytes");
            else for(size_t at=0;at<readback.size();at+=28)
                Check(!memcmp(readback.data()+at,"a compressible line of text\n",28),
                      "Compressed copy changed macOS fixture bytes");
        }
    }
    auto hfs=OpenBrowserImage(path);
    Entry hfsEntry;
    Check(hfs&&hfs->FindChild(2,names[1],hfsEntry),"Cannot reopen HFS compression source");
    for(bool external:{false,true}){
        if(external&&!fixture)continue;
        const auto out=Wide(root+(external?"\\resource-compression-macos.img":"\\resource-compression-native.img"));
        if(external)Check(CopyFileW(fixture,out.c_str(),TRUE)!=FALSE,"Cannot copy APFS fixture");
        else{
            auto disk=HfsPlus::CreateImage(out.c_str(),32*1024*1024);
            Storage::Job format;
            Apfs::Format(*disk,u"Resource compression round trip",true,format);
        }
        auto target=OpenBrowserImage(out);
        Check(target&&target->Supports().create&&target->Writable()!=external,"Wrong APFS compression writer");
        Storage::Job job;
        Completion completion;
        const auto result=ExecuteInWorker(OrdinaryCopy{{Native(hfs,hfsEntry.id)},Native(target)},job,completion);
        Check(!result.code&&completion.complete,result.message.c_str());
        target.reset();
        target=OpenBrowserImage(out);
        Entry entry;
        Check(target&&target->FindChild(2,names[1],entry)&&entry.compressed&&!entry.forks[1].size,
              "APFS reopened the compression resource as a genuine resource fork");
        entry=target->Stat(entry.id);
        Check(entry.resourceCompressionHeader==resourceEntry.resourceCompressionHeader&&
              entry.compressionResourceSize==stored.size(),"APFS re-encoded the compression resource header");
        Bytes actual(stored.size());
        target->ReadCompressionResource(entry.id,0,actual.data(),actual.size());
        Check(actual==stored,"APFS re-encoded the stored compression resource");
        Bytes logical(contents[1].size());
        target->Read(entry.id,0,0,logical.data(),logical.size());
        Check(logical==contents[1]&&target->GetAttributes(entry.id)==metadata,"Compression round trip changed file or metadata");
    }
    fprintf(report,"PASS: exact resource-backed compression bytes survive APFS to HFS Plus and back through both APFS writers.\n");
    if(apple)fprintf(report,"PASS: macOS-compressed APFS file copied into HFS+ and both HFSX variants.\n");
    fprintf(report,"PASS: APFS inline and resource-backed compression survived HFS+/HFSX copy, overwrite and reopen with exact bytes; cancelled overwrite retained both original forks.\n");
}

void TestCrossFormatAttributes(const std::string& root,FILE* report)
{
    using Kind=HfsPlus::Volume::FormatKind;
    const auto acl=[](unsigned count){
        HfsPlus::Bytes bytes(44+24*count);
        HfsPlus::Disk::P32(bytes.data(),0x012cc16d);
        for(unsigned i=0;i<32;++i)bytes[4+i]=uint8_t(i+1);
        HfsPlus::Disk::P32(bytes.data()+36,count);
        for(unsigned i=0;i<count;++i){
            auto* ace=bytes.data()+44+24*i;
            ace[0]=uint8_t(i+1);
            HfsPlus::Disk::P32(ace+16,i%2?2:1);
            HfsPlus::Disk::P32(ace+20,2);
        }
        // Use the same file-backed interface as a staged attribute edit.
        return HfsPlus::AttributeData::Snapshot(bytes.size(),[&](uint64_t at,void* data,size_t size){
            memcpy(data,bytes.data()+at,size);
        },true);
    };
    const auto streamed=[](uint64_t length,unsigned seed){
        return HfsPlus::AttributeData::Snapshot(length,[seed](uint64_t at,void* data,size_t count){
            auto bytes=static_cast<uint8_t*>(data);
            for(size_t i=0;i<count;++i)bytes[i]=uint8_t((at+i)*17+((at+i)>>8)+seed);
        },true);
    };
    const HfsPlus::Attributes folderAttrs{
        {u"com.apple.system.Security",acl(128)},
        {u"user.folder.inline",{5,3,0,1}},
        {u"user.folder.large",streamed(137023,29)}
    };
    const HfsPlus::Attributes fileAttrs{
        {u"com.apple.system.Security",acl(2)},
        {u"user.inline",{0x80,0,0x7f}},
        {u"user.empty",HfsPlus::Bytes{}},
        {u"user.accent.\u00e9",{3,2,1}},
        {u"user."+std::u16string(61,u'\u00e9'),{4,5,6}},
        {u"user.large",streamed(2*1024*1024+17,73)}
    };
    const HfsPlus::Attributes staleAttrs{
        {u"com.apple.system.Security",acl(1)},
        {u"user.old",streamed(8193,5)}
    };
    for(const auto kind:{Kind::HfsPlus,Kind::HfsXCaseSensitive,Kind::HfsXCaseInsensitive}){
        const char* format=kind==Kind::HfsPlus?"hfsplus":kind==Kind::HfsXCaseSensitive?"hfsx":"hfsx-folded";
        const auto sourcePath=Wide(root+"\\"+format+"-xattrs.dsk");
        const auto targetPath=Wide(root+"\\"+format+"-xattrs-apfs.img");
        {
            auto disk=HfsPlus::CreateImage(sourcePath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Attribute source",true,kind);
            HfsPlus::Volume hfs(*disk,true);
            const auto folder=hfs.MakeDirectory(2,u"Attribute transfer",&folderAttrs);
            hfs.Put(folder,u"Document",4097,711,[](unsigned fork,uint64_t,void* data,size_t count){
                memset(data,fork?0x79:0x34,count);
            },nullptr,false,nullptr,&fileAttrs);
        }
        const auto external=kind==Kind::HfsXCaseSensitive?_wgetenv(L"HFV_APFS_CASE_SENSITIVE_FIXTURE"):nullptr;
        if(external){
            Check(CopyFileW(external,targetPath.c_str(),TRUE)!=FALSE,"Could not copy macOS APFS fixture");
        }else{
            auto disk=HfsPlus::CreateImage(targetPath.c_str(),32*1024*1024);
            Storage::Job job;
            Apfs::Format(*disk,u"Attribute destination",true,job);
        }
        {
            auto source=OpenBrowserImage(sourcePath);
            auto target=OpenBrowserImage(targetPath);
            Entry folder,file;
            Check(source&&VolumeSession::VolumeSessionPidForTest(source)&&target&&
                  source->FindChild(2,u"Attribute transfer",folder)&&source->FindChild(folder.id,u"Document",file)&&
                  source->GetAttributes(folder.id)==folderAttrs&&source->GetAttributes(file.id)==fileAttrs,
                  "HFS+ worker did not return the source attributes and ACLs");
            const auto copy=[&](bool replace){
                bool prompted=false;
                Storage::Job job;
                job.transport({},[&](const auto&){
                    prompted=true;
                    return replace?Storage::ConflictChoice::Replace:Storage::ConflictChoice::Cancel;
                });
                Completion completion;
                const auto result=ExecuteInWorker(OrdinaryCopy{{Native(source,folder.id)},Native(target,2)},job,completion);
                if(result.code)throw HfsPlus::Error(result.message);
                Check(completion.complete&&prompted==replace,"Unexpected attribute copy completion or overwrite prompt");
            };
            copy(false);
            Entry copiedFolder,copiedFile;
            Check(target->FindChild(2,u"Attribute transfer",copiedFolder)&&
                  target->FindChild(copiedFolder.id,u"Document",copiedFile)&&
                  target->GetAttributes(copiedFolder.id)==folderAttrs&&target->GetAttributes(copiedFile.id)==fileAttrs,
                  "APFS copy lost source folder or file attributes");
            target->SetAttributes(copiedFile.id,staleAttrs);
            copy(true);
        }
        auto target=OpenBrowserImage(targetPath);
        Entry folder,file;
        Check(target&&target->FindChild(2,u"Attribute transfer",folder)&&target->FindChild(folder.id,u"Document",file)&&
              target->GetAttributes(folder.id)==folderAttrs&&target->GetAttributes(file.id)==fileAttrs,
              "Reopened APFS copy lost attributes or retained stale overwrite metadata");
        const size_t sizes[]={4097,711};
        for(unsigned fork=0;fork<2;++fork){
            Check(file.forks[fork].size==sizes[fork],"Attribute copy lost a file fork");
            HfsPlus::Bytes contents(sizes[fork]);
            target->Read(file.id,fork,0,contents.data(),contents.size());
            Check(std::all_of(contents.begin(),contents.end(),[&](uint8_t b){return b==(fork?0x79:0x34);}),
                  "Attribute copy changed file fork data");
        }
        const auto destinationPath=Wide(root+"\\"+format+"-xattrs-return.dsk");
        {
            auto disk=HfsPlus::CreateImage(destinationPath.c_str(),16*1024*1024);
            HfsPlus::Volume::Format(*disk,u"Attribute return destination",true,kind);
        }
        {
            auto destination=OpenBrowserImage(destinationPath);
            Check(destination&&VolumeSession::VolumeSessionPidForTest(destination),"Missing HFS+ attribute-copy worker");
            const auto copyToHfs=[&](uint32_t parent,Storage::Job& job,Completion& completion){
                return ExecuteInWorker(OrdinaryCopy{{Native(target,folder.id)},Native(destination,parent)},job,completion);
            };
            const auto workerPid=VolumeSession::VolumeSessionPidForTest(destination);
            for(const bool afterForkRead:{false,true}){
                bool interrupted=false;
                Storage::Job cancellation;
                cancellation.transport([&](const Storage::Progress& progress){
                    if(!interrupted&&progress.path=="Document"&&(!afterForkRead||progress.fileBytes)){
                        interrupted=true;
                        cancellation.cancel();
                    }
                },{});
                Completion aborted;
                const auto cancelled=copyToHfs(2,cancellation,aborted);
                if(!interrupted||cancelled.outcome!=Storage::Outcome::Cancelled)
                    throw HfsPlus::Error("Expected cancellation during APFS attribute copy: "+cancelled.message);
                Check(VolumeSession::VolumeSessionPidForTest(destination)==workerPid,
                      "Cancelling the APFS attribute copy stopped the destination worker");
                Entry staged,absent;
                Check(destination->FindChild(2,u"Attribute transfer",staged),
                      "Could not find the folder created before cancelling the APFS copy");
                Check(!destination->FindChild(staged.id,u"Document",absent),
                      "Cancelling the APFS attribute copy published an incomplete file");
                Check(UserCopyAttributes(*destination,staged.id)==folderAttrs,
                      "A partial APFS copy published an HFS folder without its original attributes and ACL");
            }

            Storage::Job retry;
            Completion done;
            const auto result=copyToHfs(2,retry,done);
            if(result.code)throw HfsPlus::Error(result.message);
            Check(done.complete,"Incomplete APFS attribute copy to HFS+");

            const auto merge=destination->MakeDirectory(2,u"Merge");
            const auto oldFolder=destination->MakeDirectory(merge,u"Attribute transfer");
            destination->SetAttributes(oldFolder,staleAttrs);
            const auto oldFile=Put(*destination,oldFolder,u"Document",512);
            destination->SetAttributes(oldFile,staleAttrs);
            Storage::Job overwrite;
            bool approved=false;
            overwrite.transport({},[&](const auto&){approved=true;return Storage::ConflictChoice::Replace;});
            Completion merged;
            const auto replacement=copyToHfs(merge,overwrite,merged);
            if(replacement.code)throw HfsPlus::Error(replacement.message);
            Check(merged.complete&&approved,"Missing APFS to HFS attribute overwrite approval");
        }

        auto destination=OpenBrowserImage(destinationPath);
        Entry merge;
        Check(destination&&destination->FindChild(2,u"Merge",merge),"Could not reopen the HFS attribute fixture");
        for(const auto parent:{uint32_t(2),merge.id}){
            Entry copiedFolder,copiedFile;
            Check(destination->FindChild(parent,u"Attribute transfer",copiedFolder)&&
                  destination->FindChild(copiedFolder.id,u"Document",copiedFile)&&
                  UserCopyAttributes(*destination,copiedFolder.id)==(parent==2?folderAttrs:staleAttrs),
                  "Reopened HFS folder lost attributes or changed existing merge metadata");
            const auto attributes=UserCopyAttributes(*destination,copiedFile.id);
            Check(attributes==fileAttrs&&attributes.at(u"user.large").FileBacked(),
                  "Reopened HFS file lost inline attributes, streamed attributes or its ACL");
            for(unsigned fork=0;fork<2;++fork){
                Check(copiedFile.forks[fork].size==sizes[fork],"Reopened HFS attribute copy lost a fork");
                HfsPlus::Bytes contents(sizes[fork]),original(sizes[fork]);
                destination->Read(copiedFile.id,fork,0,contents.data(),contents.size());
                target->Read(file.id,fork,0,original.data(),original.size());
                Check(contents==original,"APFS to HFS attribute copy changed fork contents");
            }
        }
        if(external)fprintf(report,"PASS: attributes and ACLs round-trip through the shared macOS APFS fixture.\n");
    }
    fprintf(report,"PASS: APFS, HFS+ and both HFSX variants preserve file and folder ACLs and inline, empty, Unicode-named, and streamed extended attributes after reopening. Interrupted copies retain new folder ACLs, and merges keep existing folder attributes.\n");
}
} // namespace

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
        TestCopySpecialFiles(root,report);
        TestCopyDirectoryHardLinksToApfs(root,report);
        TestMfsTransferVersions(root,report);
        TestClassicMetadataCompanion(report);
        TestClassicArchiveRoundTrip(root,report);
        TestClassicArchiveNamePreview(root,report);
        TestClassicArchiveSpecialEntries(root,report);
        TestClassicBackupDates(root,report);
        TestClassicCopyLoss(root,report);
        TestClassicLockRoundTrip(root,report);
        TestClassicAndMfsCopyNameCollisions(root,report);
        TestHfsSystemImmutableToApfs(root,report);
        TestCrossFormatTimestamps(root,report);
        TestWorkerTransport();
        TestVolumeSessionProtocol();
        TestHfsToApfsPosix(root,report);
        TestCopyPosixIntoHfs(root,report);
        TestSymbolicLinkCopies(root,report);
        TestApfsRegularOverwritesHfsSymlink(root,report);
        TestHfsToApfsHardLinks(root,report);
        TestHfsToApfsOverwriteTypes(root,report);
        TestApfsToHfsHardLinks(root,report);
        TestLateHardLinkOverwriteCancellation(root,report);
        TestCrossFormatAttributes(root,report);
        TestHfsCompressionWithResourceToApfs(root,report);
        TestApfsToHfsCompression(root,report);
        TestApfsImmutableCopy(root,report);
        TestSparseApfsCopy(root,report);
        TestApfsToHfsNameCollisions(root,report);
        TestHfsxToApfsNameCollisions(root,report);
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
        for (const auto kind : {HfsPlus::Volume::FormatKind::HfsPlus,
                                HfsPlus::Volume::FormatKind::HfsXCaseSensitive,
                                HfsPlus::Volume::FormatKind::HfsXCaseInsensitive}) {
            const auto path = Wide(root + "\\lookup-" + std::to_string(static_cast<int>(kind)) + ".dsk");
            uint32_t file, alias, variant = 0;
            {
                auto seed = Image(path, kind);
                const auto folder = seed->MakeDirectory(2, u"Original");
                file = Put(*seed, folder, u"Mixed");
                if (kind == HfsPlus::Volume::FormatKind::HfsXCaseSensitive)
                    variant = Put(*seed, folder, u"mixed");
                alias = seed->MakeHardLink(folder, 2, u"Alias");
            }
            const auto mounted = OpenBrowserImage(path);
            Check(mounted && ExecutionFor(*mounted).Persistent(), "lookup volume did not use a persistent worker");
            const auto listings = VolumeSession::VolumeListingRequestsForTest();
            Entry found;
            Check(mounted->FindChild(alias, u"Mixed", found) && found.id == file && found.parent == alias,
                  "single-entry lookup lost the visible directory alias");
            Entry requested;
            requested.name = u"mixed";
            Check(mounted->FindTransferChild(alias, requested, found) &&
                  found.id == (variant ? variant : file), "lookup used the wrong collation for a copy conflict");
            Check(!mounted->FindChild(alias, u"Absent", found) &&
                  VolumeSession::VolumeListingRequestsForTest() == listings,
                  "single-entry lookup requested a full directory listing");
        }
        fprintf(report, "PASS: single-entry worker lookup, HFS+/HFSX collation and directory aliases without a List request.\n");
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
                      classic->Name() == u"Classic" && !classic->Supports().attributes,
                  "classic image did not share a writable persistent mount");
            Entry sentinel;
            std::wstring classicMacBinary;
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
                    if(mode=='m')classicMacBinary=exportedFile.WindowsPath();
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
            const auto freeBeforeImport=classic->FreeBytes();
            Check(!classicMacBinary.empty(),"missing real MacBinary import fixture");
            const auto import=[&]{
                LegacyConversion request{Location::Windows(classicMacBinary),Native(classic,2)};
                request.options.input='m';
                request.target=ConversionTarget::Folder;
                auto job=Submit(request);
                Wait(job);
                return job.Job()->result();
            };
            VolumeSession::CrashVolumeSessionDuringPutForTest(classic);
            const auto interrupted=import();
            Check(interrupted.code && WorkerTransport::lastExitCode==80 && !classic->Writable(),
                  "classic import worker did not stop during the new file's transfer");
            classic->Reload();
            Entry imported;
            Check(classic->Writable() && VolumeSession::VolumeSessionPidForTest(classic)!=beforeCrash &&
                      classic->FindChild(2,u"Crash sentinel",sentinel) &&
                      !classic->FindChild(2,u"Renamed payload",imported) &&
                      classic->FreeBytes()==freeBeforeImport,
                  "classic worker did not roll back the interrupted import and restore writable access");
            Verify(*classic,sentinel.id,4096);
            const auto retried=import();
            if(retried.code)throw HfsPlus::Error(retried.message);
            Check(classic->FindChild(2,u"Renamed payload",imported),"retried MacBinary import did not publish");
            Verify(*classic,imported.id);
            hfsdirent importedProperties{};
            Check(classic->GetClassicProperties(imported.id,importedProperties)&&(importedProperties.fdflags&14)==6,
                  "retried MacBinary import lost Finder metadata");
            classic.reset();
            classic=OpenBrowserImage(Wide(path));
            Check(classic&&classic->Writable()&&classic->FindChild(2,u"Renamed payload",imported),
                  "completed MacBinary import did not survive a fresh mount");
            Verify(*classic,imported.id);
            fprintf(report,"PASS: classic HFS shared mount, Finder edits, rename, conversions, and durable rollback and retry after a worker crash during a new MacBinary import\n");
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
                      !volume->Supports().createFolders && volume->Supports().basicMetadata,
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
            const auto imageBytes=[&]{
                MfsImageSource disk;
                std::string message;
                if(!disk.Open(path.c_str(),message))throw HfsPlus::Error(message);
                HfsPlus::Bytes bytes(size_t(disk.Length()));
                Check(disk.ReadAt(0,bytes.data(),bytes.size()),"Cannot read MFS test image");
                return bytes;
            };
            const auto baseline=imageBytes();
            volume=OpenBrowserImage(path);
            Check(volume && volume->Writable(),"clean MFS image did not reopen writable");
            const auto beforeCrash=VolumeSession::VolumeSessionPidForTest(volume);
            Entry beforeRestart;
            Check(volume->FindChild(2,u"Renamed payload",beforeRestart),"Capture MFS entry before worker restart");
            auto stale=FileOperations::Location::Mounted(volume,beforeRestart.id);
            stale.expectedIdentity=beforeRestart.identity;
            VolumeSession::CrashVolumeSessionDuringPutForTest(volume);
            bool failed=false;
            try{
                auto replacement=beforeRestart;
                volume->Put(2,replacement,[](unsigned fork,uint64_t,void* bytes,size_t size){
                    memset(bytes,fork?0xA7:0x5B,size);
                },true);
            }catch(const HfsPlus::Error&){failed=true;}
            Check(failed && WorkerTransport::lastExitCode==81 && !volume->Writable(),
                  "MFS worker did not stop after a flushed sector write");
            volume->Reload();
            Check(volume->Writable() && VolumeSession::VolumeSessionPidForTest(volume)!=beforeCrash,
                  "MFS crash refresh did not recover a writable image");
            Entry afterRestart;
            Check(volume->FindChild(2,u"Renamed payload",afterRestart)&&
                  afterRestart.identity.primary==beforeRestart.identity.primary&&
                  !StorageLocation::MatchesEntry(stale,afterRestart),"Worker restart accepted a stale selection");
            stale={};
            volume.reset();
            Check(imageBytes()==baseline,"MFS payload crash recovery did not restore the exact image");

            volume=OpenBrowserImage(path);
            Check(volume&&volume->FindChild(2,u"Renamed payload",afterRestart),"Missing MFS rename fixture");
            VolumeSession::CrashVolumeSessionOnRenameForTest(volume);
            failed=false;
            try{volume->Rename(afterRestart.id,2,u"Name interrupted during catalog write");}
            catch(const HfsPlus::Error&){failed=true;}
            Check(failed&&WorkerTransport::lastExitCode==81&&!volume->Writable(),
                  "MFS worker did not stop inside the catalog update");
            volume.reset();
            Check(imageBytes()!=baseline,"Catalog crash did not change the MFS image");
            volume=OpenBrowserImage(path);
            Entry retained,unexpected;
            Check(volume&&volume->Writable()&&volume->FindChild(2,u"Renamed payload",retained)&&
                  !volume->FindChild(2,u"Name interrupted during catalog write",unexpected),
                  "Fresh MFS mount did not recover the old catalog");
            Verify(*volume,retained.id,8192);
            Check(retained.version==beforeRestart.version&&retained.finder==beforeRestart.finder,
                  "Interrupted MFS catalog write changed file metadata");
            volume.reset();
            Check(imageBytes()==baseline,"MFS catalog recovery did not restore the exact image or DC42 checksums");
            volume=OpenBrowserImage(path);
            const auto retry=Put(*volume,2,u"Write after recovery",4096);
            Verify(*volume,retry,4096);
            fprintf(report,"PASS: %s MFS payload and catalog crashes recover byte-for-byte, restore both forks and metadata, invalidate stale selections, and allow a retry.\n",dc42?"Disk Copy 4.2":"raw");
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
        auto classicCopy=Submit(roundtrip);
        Conflict(classicCopy);
        Check(classicCopy.Job()->conflictState().kind==Storage::CopyPrompt::ClassicMetadataLoss,"Missing classic copy warning");
        classicCopy.Job()->resolve(Storage::ConflictChoice::Replace);
        Pass(classicCopy);
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
        for(auto kind:{HfsPlus::Volume::FormatKind::HfsPlus,HfsPlus::Volume::FormatKind::HfsXCaseSensitive}){
            const auto format=std::to_string(int(kind));
            const auto imagePath=Wide(root+"\\native-conversion-"+format+".dsk");
            { auto file=HfsPlus::CreateImage(imagePath.c_str(),16*1024*1024);
              HfsPlus::Volume::Format(*file,u"Native conversion",true,kind); }
            auto mounted=MacBrowser::OpenBrowserImage(imagePath);
            const auto id=Put(*mounted,2,u"Native source",4096);
            const auto folder=mounted->MakeDirectory(2,u"Imported");
            const auto exportPath=Wide(root+"\\native-conversion-"+format);
            Check(CreateDirectoryW(exportPath.c_str(),nullptr)!=FALSE,"Create transfer output folder");
            const auto exportRoot=Location::Windows(exportPath);
            for(char mode:{'5','m','3','b'}){
                const auto subName=U16(Wide(std::string("Mode ")+mode));
                const auto sub=mounted->MakeDirectory(folder,subName);
                const auto subRoot=Location::Mounted(mounted,sub);
                LegacyCopyOptions options;
                options.output=mode;
                auto transfers=PlanTransfers({{Location::Mounted(mounted,id)},exportRoot},{options,false,false});
                Check(transfers.size()==1&&transfers[0].Conversion()&&
                      !InspectPlanForTest(transfers[0],mounted->ResourceKey()).temporaryWorker,
                      "Native container export did not use the retained volume");
                Pass(Submit(transfers[0]));
                const auto carrier=exportPath+(mode=='b'?L"\\Native source.hqx":L"\\Native source.bin");
                options.input='a';
                transfers=PlanTransfers({{Location::Windows(carrier)},subRoot},{options,false,false});
                Check(transfers.size()==1&&transfers[0].Conversion(),"Native automatic import bypassed the container adapter");
                Pass(Submit(transfers[0]));
                Entry imported;
                Check(mounted->FindChild(sub,u"Native source",imported),"Native decoded file missing");
                Verify(*mounted,imported.id,4096);
                Check(DeleteFileW(carrier.c_str())!=FALSE,"Remove test carrier before next mode");
            }
            const auto ordinary=exportPath+L"\\ordinary.bin";
            {
                auto output=HfsPlus::CreateFormatOutput(ordinary.c_str(),4);
                output->Write(0,"data",4);
                output->Keep();
            }
            LegacyCopyOptions automatic;
            automatic.input='a';
            auto transfers=PlanTransfers({{Location::Windows(ordinary)},Location::Mounted(mounted,folder)},
                                         {automatic,false,false});
            Pass(Submit(transfers[0]));
            Entry ordinaryFile;
            Check(mounted->FindChild(folder,u"ordinary.bin",ordinaryFile)&&ordinaryFile.forks[0].size==4,
                  "Native automatic mode treated an ordinary .bin as MacBinary");
        }
        fprintf(report,"PASS: HFS+ and HFSX convert MacBinary I/II/III and BinHex with both forks on retained image sessions.\n");

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
                const auto copyToFloppy=[&](bool replacing){
                    auto operation=Submit(physical);
                    Conflict(operation);
                    const auto warning=operation.Job()->conflictState();
                    Check(warning.kind==Storage::CopyPrompt::ClassicMetadataLoss,"Missing floppy copy warning");
                    operation.Job()->resolve(warning.id,Storage::ConflictChoice::Replace);
                    if(replacing)Conflict(operation,warning.id);
                    return operation;
                };
                Pass(copyToFloppy(false));
                auto overwrite = copyToFloppy(true);
                overwrite.Job()->resolve(Storage::ConflictChoice::Skip);
                Wait(overwrite);
                Check(!overwrite.Job()->result().code && !overwrite.Snapshot().complete,
                      "process floppy skip");
                overwrite = copyToFloppy(true);
                overwrite.Job()->cancel();
                Wait(overwrite);
                Check(overwrite.Job()->result().error == ERROR_CANCELLED, "process floppy cancel");
                overwrite = copyToFloppy(true);
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
