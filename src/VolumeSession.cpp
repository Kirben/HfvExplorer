// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "VolumeSession.h"
#include "VolumeSessionProtocol.h"
#include "WorkerTransport.h"
#include "StorageHandoff.h"
#include "VolumeCoordinator.h"
#include "floppy.h"
#include <algorithm>

namespace VolumeSession {
using namespace FileOperations;
using namespace MacBrowser;
using namespace CopyWorkerProtocol;
using namespace WorkerTransport;
namespace {
namespace Protocol=VolumeSessionProtocol;
using SessionCommandCode=Protocol::Command;
#ifdef HFV_TESTING
std::atomic<HANDLE> readPauseEntered{nullptr}, readPauseRelease{nullptr};
std::atomic<HANDLE> writePauseEntered{nullptr}, writePauseRelease{nullptr};
void PauseForTest(const std::atomic<HANDLE>& entered,const std::atomic<HANDLE>& release) {
    if(auto event=entered.load()){
        SetEvent(event);
        Check(WaitForSingleObject(release.load(),15000)==WAIT_OBJECT_0,
              "The test did not release the paused fork transfer");
    }
}
#endif

void SessionReply(HANDLE output, unsigned code = 0, const std::string& error = {},
                  Storage::Rollback rollback = Storage::Rollback::None) {
    Send(output,SessionResult,Protocol::Encode(Protocol::Reply{code,error,rollback}));
}

class SessionError : public HfsPlus::Error {
public:
    using HfsPlus::Error::Error;
};

class PersistentVolume final : public Volume, public WorkerEndpoint {
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
        const auto reply=Protocol::DecodeReply(packet);
        RecordRollback(reply.rollback);
        if(reply.code==2)throw HfsPlus::ExternalJournalRequired(reply.error);
        if(reply.code==1)throw SessionError(reply.error);
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
            auto request=Protocol::EncodeCommand(command,data);
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
        process->Launch(inherited.files, L"--volume-session");
        for (const auto& file : inherited.files) SendImageFile(process->input.value,file);
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
    WorkerExecution Execution() const override {
        WorkerExecution out;
        out.placement=WorkerExecution::Placement::PersistentSession;
        out.classicConversion=Classic();
        out.stableCatalogIds=format_==ImageFormat::HfsPlus;
        return out;
    }
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
        Call(SessionCommandCode::GetClassicProperties, Protocol::Encode(Protocol::CatalogItem{id}), false, [&] {
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
        bool exists=false;
        Call(SessionCommandCode::FindMfs,Protocol::Encode(Protocol::FindMfsRequest{parent,name,version}),false,[&]{
            auto packet=Response(SessionBytes);
            exists=packet.flag();packet.end();
            if(exists)found=ReadEntry();
        });
        return exists;
    }
    bool SupportsBasicMetadata()const override{return Mfs();}
    void SetBasicMetadata(uint32_t id,const Entry& entry)override {
        std::lock_guard<std::mutex> lock(mutex_);
        Simple(SessionCommandCode::SetBasicMetadata,Protocol::EncodeBasicMetadata(id,entry),true);
    }
    void SetClassicProperties(uint32_t id, const hfsdirent& entry) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Simple(SessionCommandCode::SetClassicProperties,Protocol::EncodeClassicProperties(id,entry),true);
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
        std::vector<Entry> entries;
        Call(SessionCommandCode::List,Protocol::Encode(Protocol::ListRequest{parent}),false,[&] {
            auto packet = Response(SessionBytes); auto count = packet.get(); packet.end();
            Check(count <= UINT32_MAX, "Invalid volume directory count");
            for (uint64_t i = 0; i < count; ++i) entries.push_back(ReadEntry());
        });
        return entries;
    }
    Entry Stat(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry entry;
        Call(SessionCommandCode::Stat,Protocol::Encode(Protocol::StatRequest{id}),false,[&]{entry=ReadEntry();});
        return entry;
    }
    void Read(uint32_t id, unsigned fork, uint64_t offset, void* out, size_t bytes) override {
        auto output = static_cast<uint8_t*>(out);
        uint64_t generation=0;
        do {
            const auto count = (std::min)(bytes, Protocol::ReadLimit);
            {
                // Leave the pipe available for browsing between fork chunks.
                std::lock_guard<std::mutex> lock(mutex_);
                Check(!generation || generation == generation_, "The mounted volume changed during a fork read");
                generation=generation_;
                Call(SessionCommandCode::Read,Protocol::Encode(Protocol::ReadRequest{id,fork,offset,count}),false,[&] {
                    auto packet = Response(SessionBytes);
                    Check(packet.bytes.size() == count, "Short volume session read");
                    if (count) memcpy(output, packet.bytes.data(), count);
                });
            }
#ifdef HFV_TESTING
            if (!offset && bytes>count) PauseForTest(readPauseEntered,readPauseRelease);
#endif
            if (count) output += count;
            offset += count; bytes -= count;
        } while (bytes);
    }
    HfsPlus::Attributes GetAttributes(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        AttributeSets sets;
        Call(SessionCommandCode::Attributes, Protocol::Encode(Protocol::CatalogItem{id}), false, [&] {
            auto packet = Response(AttributeSet);
            ReceiveAttributes(worker_->output.value, packet, sets);
            Check(sets[0].has_value(), "Missing volume attributes");
        });
        return std::move(*sets[0]);
    }
    void SetAttributes(uint32_t id, const HfsPlus::Attributes& attributes) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Call(SessionCommandCode::SetAttributes, Protocol::Encode(Protocol::CatalogItem{id}), true, [] {}, &attributes);
    }
    void SetCompressionMethod(uint32_t id, HfsPlus::Volume::CompressionMode method,
                              std::function<void(uint64_t,uint64_t)> progress) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto args=Protocol::Encode(Protocol::CompressionRequest{id,method});
        std::exception_ptr stopped;
        try {
            Call(SessionCommandCode::Compression, std::move(args), true, [&] {
                for (;;) {
                    Data packet;
                    auto message = Receive(worker_->output.value, packet);
                    if (message == SessionResult) ReadResult(packet); // Early failure.
                    if (message == SessionBytes) { packet.end(); break; }
                    Check(message == SessionProgress, "Unexpected volume compression response");
                    const auto [done,total]=Protocol::DecodeCompressionProgress(packet);
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
        uint32_t id = 0;
        Call(SessionCommandCode::MakeDirectory,Protocol::Encode(Protocol::DirectoryName{parent,name}),true,[&] {
            auto packet = Response(SessionBytes); id = uint32_t(packet.get()); packet.end();
        });
        return id;
    }
    uint32_t Put(uint32_t parent, const Entry& entry, Reader reader, bool replace) override {
        return StreamPut(parent, entry, std::move(reader), replace, 0, 0, {});
    }
    uint32_t PutResourceReplacement(uint32_t id, const Entry& entry, Reader reader,
                                     std::function<void(uint64_t,size_t)> localProgress) override {
        return StreamPut(entry.parent, entry, std::move(reader), true, id, 0,
            [progress=std::move(localProgress)](unsigned,uint64_t offset,size_t size){
                if(progress)progress(offset,size);
            });
    }
    void PutFromSource(uint32_t parent,const Entry& entry,bool replace,
                       std::function<void(unsigned,uint64_t,size_t)> progress) {
        StreamPut(parent,entry,{},replace,0,entry.id,std::move(progress));
    }
private:
    uint32_t StreamPut(uint32_t parent, const Entry& entry, Reader reader, bool replace, uint32_t original,
                       uint32_t source,std::function<void(unsigned,uint64_t,size_t)> localProgress) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto args=Protocol::EncodePut({parent,replace,entry.attributesKnown,original,source},entry);
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
                    const auto [fork,offset,size]=Protocol::DecodeForkRange(packet,entry);
                    const bool localRead = source || (original && !fork);
                    Data bytes;
                    try {
#ifdef HFV_TESTING
                        if(!fork && !offset)PauseForTest(writePauseEntered,writePauseRelease);
#endif
                        if (localRead) {
                            if (localProgress) localProgress(unsigned(fork), offset, size_t(size));
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
    void ValidateEntry(const Entry& entry) const override {
        auto& self = const_cast<PersistentVolume&>(*this);
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.Call(SessionCommandCode::ValidateEntry,Protocol::EncodeEntryCheck(entry),false,[]{},&entry.attributes);
    }
    void FinalizeDirectory(uint32_t id, const Entry& desired, const Entry& expected, bool merge) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto args=Protocol::EncodeDirectoryUpdate({id,merge,desired.attributesKnown},desired,expected);
        Call(SessionCommandCode::FinalizeDirectory,std::move(args),true,[]{},&desired.attributes);
    }
    bool CanMoveFrom(const Volume& source) const override {
        return &source == this && Writable() && !Mfs();
    }
    void MoveHere(Volume& source,uint32_t id,uint32_t parent,const Entry& entry) override {
        Check(CanMoveFrom(source), "Move must stay in the same mounted session");
        Rename(id,parent,entry.name);
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto command = SessionCommandCode::Rename;
#ifdef HFV_TESTING
        if (crashRename_) { crashRename_ = false; command = SessionCommandCode::TestCrashRename; }
#endif
        Simple(command,Protocol::Encode(Protocol::RenameRequest{id,parent,name}),true);
    }
    void Remove(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Simple(SessionCommandCode::Remove, Protocol::Encode(Protocol::CatalogItem{id}), true);
    }
    uint32_t MakeHardLink(uint32_t source,uint32_t parent,const std::u16string& name,bool replace=false) override {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t id = 0;
        auto args=Protocol::Encode(Protocol::HardLinkRequest{source,parent,name,replace});
        Call(SessionCommandCode::HardLink,std::move(args),true,[&] {
            auto packet = Response(SessionBytes); id = uint32_t(packet.get()); packet.end();
        });
        return id;
    }
    void ValidateRemoval(uint32_t id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        Simple(SessionCommandCode::ValidateRemoval, Protocol::Encode(Protocol::CatalogItem{id}), false);
    }
    void ValidateCopyTo(uint32_t id,Volume& target,uint32_t parent) override {
        if (target.ResourceKey()!=key_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        Simple(SessionCommandCode::ValidateCopy,Protocol::Encode(Protocol::CopyCheck{id,parent}),false);
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

uint32_t WriteStream(HANDLE input,HANDLE output,Volume& volume,Protocol::PutRequest request,
                     SessionCommandCode command)
{
    const auto& header=request.header;
    const auto parent=header.parent,original=header.original,source=header.source;
    auto& entry=request.entry;
    if(source){
        const auto current=volume.Stat(source);
        Check(!current.directory && current.parent!=parent &&
              current.forks[0].size==entry.forks[0].size && current.forks[1].size==entry.forks[1].size,
              "The same-image fork source changed");
    }
    if (original) {
        const auto current = volume.Stat(original);
        Check(!current.directory && current.parent == parent &&
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
            const auto count = (std::min)(size, Protocol::ReadLimit);
            const auto localSource = source ? source : !fork ? original : 0;
            if (localSource) volume.Read(localSource, fork, offset, buffer, count);
            Send(output,SessionReadRequest,Protocol::Encode(Protocol::ForkRange{fork,offset,count}));
            Data bytes;
            const auto reply=Receive(input,bytes);
            if(!Protocol::DecodeForkReply(reply,bytes,count,localSource!=0))throw Storage::Cancelled();
            if(!localSource)memcpy(buffer,bytes.bytes.data(),count);
            offset += count; buffer += count; size -= count;
        }
    };
    return original ? volume.PutResourceReplacement(original, entry, std::move(read), {})
                             : volume.Put(parent, entry, std::move(read), header.replace);
}

void ServeVolume(HANDLE input, HANDLE output, Volume& volume) {
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
        const auto command=Protocol::DecodeCommand(packet,attributes[0].has_value(),attributes[1].has_value());
        Storage::OutcomeEvidence evidence;
        hfs_take_rollback();
        try {
            switch (command) {
            case SessionCommandCode::Root: packet.end(); writeNumber(volume.FreeBytes()); break;
            case SessionCommandCode::List: {
                const auto request=Protocol::DecodeList(packet);
                auto entries=volume.List(request.parent);
                writeNumber(entries.size());
                for (const auto& entry : entries) writeEntry(entry);
                break;
            }
            case SessionCommandCode::Stat: {
                const auto request=Protocol::DecodeStat(packet);
                writeEntry(volume.Stat(request.id));
                break;
            }
            case SessionCommandCode::Read: {
                const auto request=Protocol::DecodeRead(packet);
                Data bytes;
                bytes.bytes.resize(size_t(request.count));
                volume.Read(request.id,request.fork,request.offset,bytes.bytes.data(),bytes.bytes.size());
                Send(output, SessionBytes, std::move(bytes));
                break;
            }
            case SessionCommandCode::Attributes: {
                auto values=volume.GetAttributes(Protocol::DecodeItem(packet).id);
                Storage::Job transfer;
                SendAttributes(output, 0, values, transfer);
                break;
            }
            case SessionCommandCode::SetAttributes: {
                volume.SetAttributes(Protocol::DecodeItem(packet).id,*attributes[0]);
                break;
            }
            case SessionCommandCode::MakeDirectory: {
                const auto request=Protocol::DecodeDirectoryName(packet);
                writeNumber(volume.MakeDirectory(request.parent,request.name));
                break;
            }
#ifdef HFV_TESTING
            case SessionCommandCode::TestCrashRename:
#endif
            case SessionCommandCode::Rename: {
                const auto request=Protocol::DecodeRename(packet);
                volume.Rename(request.id,request.parent,request.name);
#ifdef HFV_TESTING
                if (command == SessionCommandCode::TestCrashRename) ExitProcess(79);
#endif
                break;
            }
            case SessionCommandCode::Remove: {
                volume.Remove(Protocol::DecodeItem(packet).id);
                break;
            }
            case SessionCommandCode::HardLink: {
                const auto request=Protocol::DecodeHardLink(packet);
                writeNumber(volume.MakeHardLink(request.source,request.parent,request.name,request.replace));
                break;
            }
            case SessionCommandCode::Compression: {
                const auto request=Protocol::DecodeCompression(packet);
                volume.SetCompressionMethod(request.id,request.method,[&](uint64_t done,uint64_t total){
                    Send(output,SessionProgress,Protocol::Encode(Protocol::CompressionProgress{done,total}));
                    Data answer;
                    const auto decision=Receive(input,answer);
                    if(!Protocol::DecodeContinue(decision,answer))throw Storage::Cancelled();
                });
                Send(output, SessionBytes);
                break;
            }
#ifdef HFV_TESTING
            case SessionCommandCode::TestCrashPut:
#endif
            case SessionCommandCode::Put: {
                auto request=Protocol::DecodePut(packet);
                request.entry.attributes=std::move(*attributes[0]);
                writeNumber(WriteStream(input,output,volume,std::move(request),command));
                break;
            }
            case SessionCommandCode::FinalizeDirectory: {
                auto request=Protocol::DecodeDirectoryUpdate(packet);
                request.desired.attributes=std::move(*attributes[0]);
                volume.FinalizeDirectory(request.header.id,request.desired,request.expected,request.header.merge);
                break;
            }
            case SessionCommandCode::ValidateEntry: {
                auto entry=Protocol::DecodeEntryCheck(packet);
                entry.attributes = std::move(*attributes[0]);
                volume.ValidateEntry(entry);
                break;
            }
            case SessionCommandCode::FindMfs: {
                auto request=Protocol::DecodeFindMfs(packet);
                Entry found;
                bool exists;
                if(request.version){
                    Entry selector;
                    selector.name=std::move(request.name);
                    selector.version=*request.version;
                    exists=volume.FindTransferChild(request.parent,selector,found);
                }else exists=volume.FindChild(request.parent,request.name,found);
                writeNumber(exists);
                if(exists)writeEntry(found);
                break;
            }
            case SessionCommandCode::SetBasicMetadata: {
                const auto request=Protocol::DecodeBasicMetadata(packet);
                volume.SetBasicMetadata(request.id,request.entry);
                break;
            }
            case SessionCommandCode::CloseClassic:
                packet.end();
                Check(!strcmp(volume.Kind(),"HFS"), "Unexpected classic close command");
                volume.Close();
                SessionReply(output);
                return;
            case SessionCommandCode::GetClassicProperties: {
                const auto id=Protocol::DecodeItem(packet).id;
                hfsdirent entry{};
                Check(volume.GetClassicProperties(id, entry), "Classic Finder properties are unavailable");
                Data result; EncodeClassic(result, entry);
                Send(output, SessionBytes, std::move(result)); break;
            }
            case SessionCommandCode::SetClassicProperties: {
                const auto request=Protocol::DecodeClassicProperties(packet);
                volume.SetClassicProperties(request.id,request.properties);
                break;
            }
            case SessionCommandCode::ValidateRemoval: {
                volume.ValidateRemoval(Protocol::DecodeItem(packet).id);
                break;
            }
            case SessionCommandCode::ValidateCopy: {
                const auto request=Protocol::DecodeCopyCheck(packet);
                volume.ValidateCopyTo(request.id,volume,request.parent);
                break;
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

int Child(const char* inputText, const char* outputText) {
    HANDLE input = HANDLE(uintptr_t(_strtoui64(inputText, nullptr, 10)));
    HANDLE output = HANDLE(uintptr_t(_strtoui64(outputText, nullptr, 10)));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        std::vector<ImageHandleTransfer::File> files;
        for (;;) {
            Data data;
            auto message = Receive(input, data);
            if (message == File) {
                files.push_back(DecodeImageFile(data));
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
void PauseVolumeWriteForTest(HANDLE entered,HANDLE release) {
    if(entered)writePauseRelease=release;
    writePauseEntered=entered;
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
bool SharesMountedImage(const Location& source,const Location& destination) {
    return source.IsMounted() && destination.IsMounted() && source.Volume()==destination.Volume() &&
           dynamic_cast<PersistentVolume*>(destination.Volume().get());
}

void PutFromMountedSource(Volume& volume,uint32_t parent,const Entry& entry,bool replace,
                          std::function<void(unsigned,uint64_t,size_t)> progress) {
    dynamic_cast<PersistentVolume&>(volume).PutFromSource(parent,entry,replace,std::move(progress));
}
} // namespace VolumeSession
