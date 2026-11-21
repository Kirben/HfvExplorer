// SPDX-License-Identifier: GPL-2.0-or-later
#include "FloppyWriter.h"
#include "FloppyWriteLease.h"
#ifdef HFV_APP_REMOVABLE_UNDO
#include "OperationCompletion.h"
#endif
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
}
namespace {
const DWORD kImage=1474560,kTrack=18*512;
class WindowsFloppyWriteApi : public FloppyWriteApi {
    bool control(HANDLE h,DWORD code) {DWORD returned;return DeviceIoControl(h,code,NULL,0,NULL,0,&returned,NULL)!=FALSE;}
public:
    UINT DriveType(int d) override {return NativeFloppyReadApi().DriveType(d);}
    HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f) override {return NativeFloppyReadApi().Open(p,a,s,f);}
    bool Geometry(HANDLE h,DISK_GEOMETRY& g) override {return NativeFloppyReadApi().Geometry(h,g);}
    bool Verify(HANDLE h,DWORD& c) override {return VerifyFloppyWriteMedia(h,c);}
    bool Read(HANDLE h,ULONGLONG o,DWORD n,void* p,DWORD& got) override {return NativeFloppyReadApi().Read(h,o,n,p,got);}
    void Close(HANDLE h) override {CloseHandle(h);}
    bool Lock(HANDLE h) override {return control(h,FSCTL_LOCK_VOLUME);}
    bool Dismount(HANDLE h) override {return control(h,FSCTL_DISMOUNT_VOLUME);}
    bool Unlock(HANDLE h) override {return control(h,FSCTL_UNLOCK_VOLUME);}
    bool Write(HANDLE h,ULONGLONG o,DWORD n,const void* p,DWORD& wrote) override {
        LARGE_INTEGER position;position.QuadPart=o;wrote=0;
        return SetFilePointerEx(h,position,NULL,FILE_BEGIN)&&WriteFile(h,p,n,&wrote,NULL);
    }
    bool Flush(HANDLE h) override {return FlushFileBuffers(h)!=FALSE;}
};
WindowsFloppyWriteApi nativeWriter;
unsigned word(const BYTE* p) {return unsigned(p[0])*256+p[1];}
DWORD number(const BYTE* p) {return DWORD(word(p))*65536+word(p+2);}
bool geometry1440(const DISK_GEOMETRY& g) {
    return g.MediaType==F3_1Pt44_512 && g.Cylinders.QuadPart==80 && g.TracksPerCylinder==2 && g.SectorsPerTrack==18 && g.BytesPerSector==512;
}
}
#ifdef HFV_TESTING
FloppyWriteApi& NativeFloppyWriteApiForTest(){return nativeWriter;}
#endif
#ifdef HFV_APP_REMOVABLE_UNDO
namespace FloppyRecovery {
void Verify(FloppyWriteApi& api,HANDLE handle,DWORD generation)
{
    DWORD now=0;
    if(!api.Verify(handle,now))
        throw Storage::MediaIoError(GetLastError(),"Cannot verify floppy media for recovery");
    if(now!=generation)throw Storage::MediaChanged("The floppy changed during recovery");
}
void Read(FloppyWriteApi& api,HANDLE handle,DWORD generation,uint64_t offset,DWORD bytes,void* buffer)
{
    Verify(api,handle,generation);
    DWORD got=0;
    if(!api.Read(handle,offset,bytes,buffer,got)||got!=bytes)
        throw Storage::MediaIoError(ERROR_READ_FAULT,"Cannot read the original floppy sectors");
    Verify(api,handle,generation);
}
void Replay(FloppyWriteApi& api,HANDLE handle,DWORD generation,int drive)
{
    auto* scratch=VirtualAlloc(nullptr,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!scratch)throw std::bad_alloc();
    struct Free {void* p;~Free(){VirtualFree(p,0,MEM_RELEASE);}} free{scratch};
    RemovableUndo::Recover(RemovableUndo::FloppyKey(drive),Range,
      [&](uint64_t at,DWORD bytes,void* data){Read(api,handle,generation,at,bytes,data);},
      [&](uint64_t at,DWORD bytes,const void* data){
        Verify(api,handle,generation);
        DWORD got=0;
        if(!api.Write(handle,at,bytes,data,got)||got!=bytes)
            throw Storage::MediaIoError(ERROR_WRITE_FAULT,"Cannot restore the original floppy sectors");
        if(!api.Flush(handle)){
            const auto error=GetLastError();
            if(error!=ERROR_NOT_SUPPORTED&&error!=ERROR_INVALID_FUNCTION)
                throw Storage::MediaIoError(error,"Cannot flush the recovered floppy");
        }
        Read(api,handle,generation,at,bytes,scratch);
        if(memcmp(scratch,data,bytes))
            throw Storage::MediaIoError(ERROR_CRC,"Floppy recovery readback did not match");
    });
}
}

#endif
bool ValidateFloppyHfs(const std::vector<BYTE>& image) {
    if(image.size()!=kImage) return false;const BYTE* m=image.data()+1024;
    unsigned count=word(m+18),first=word(m+28);DWORD block=number(m+20);
    return word(m)==0x4244 && count && first>=3 && block>=512 && block%512==0 && m[36] && m[36]<=27 &&
        ULONGLONG(first)*512+ULONGLONG(count)*block<=kImage;
}
FloppyWriter::FloppyWriter(FloppyWriteApi* api):api_(api?api:&nativeWriter),error_(0),writesStarted_(false) {}
void FloppyWriter::Mark(const char* stage,DWORD offset,DWORD requested) {
 stage_=stage;offset_=offset;requested_=requested;transferred_=0;
 char line[256];sprintf(line,"stage=%s offset=%lu sector=%lu requested=%lu written_tracks=%u verified_tracks=%u\r\n",stage,offset,offset/512,requested,writtenTracks_,verifiedTracks_);
 diagnostics_+=line;
}
void FloppyWriter::Report(bool ok,int drive) {
 char line[768];unsigned track=offset_/kTrack;
 sprintf(line,"\r\nRESULT=%s target=%c: error=%lu stage=%s offset=%lu sector=%lu cylinder=%u head=%u sector_in_track=%lu requested=%lu transferred=%lu completed_writes=%u/%u completed_readbacks=%u/%u writes_started=%d\r\n",
 ok?"PASS":"FAIL",'A'+drive,error_,stage_.c_str(),offset_,offset_/512,track/2,track%2,(offset_%kTrack)/512+1,requested_,transferred_,writtenTracks_,plannedTracks_,verifiedTracks_,plannedTracks_,writesStarted_);
 diagnostics_+=line;
}
bool FloppyWriter::WriteImage(int drive,const std::vector<BYTE>& image,FloppyProgress progress,void* context,const std::vector<BYTE>* expectedOriginal,std::optional<DWORD> expectedGeneration) {
    error_=flushError_=0;writesStarted_=false;writtenTracks_=verifiedTracks_=0;plannedTracks_=160;diagnostics_.clear();Mark("Validate source image");
    if(drive<0 || drive>25 || !ValidateFloppyHfs(image) || (expectedOriginal && expectedOriginal->size()!=kImage)) {error_=ERROR_INVALID_PARAMETER;Report(false,drive);return false;}
    Mark("Acquire exclusive floppy access");
    FloppyWriteLease lease;
    if(!lease.Acquire(drive)){error_=GetLastError();Report(false,drive);return false;}
    std::vector<DWORD> tracks;
    for(DWORD offset=0;offset<kImage;offset+=kTrack)
        if(!expectedOriginal || memcmp(image.data()+offset,expectedOriginal->data()+offset,kTrack))tracks.push_back(offset);
    plannedTracks_=unsigned(tracks.size());
    Mark("Check drive type");
    if(api_->DriveType(drive)!=DRIVE_REMOVABLE) {error_=ERROR_BAD_DEVICE;Report(false,drive);return false;}
    char path[16];sprintf(path,"\\\\.\\%c:",'A'+drive);
    Mark("Open read/write handle");
    HANDLE handle=api_->Open(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH);
    if(handle==INVALID_HANDLE_VALUE) {error_=GetLastError();Report(false,drive);return false;}
    bool locked=false;BYTE* scratch=nullptr;
#ifdef HFV_APP_REMOVABLE_UNDO
    std::unique_ptr<RemovableUndo::Log> undo;
#endif
    DWORD generation=0,current=0;
    auto finish=[&](bool ok)->bool {
        DWORD failure=ok?ERROR_SUCCESS:GetLastError();
#ifdef HFV_APP_REMOVABLE_UNDO
        if(!ok&&undo){
            undo.reset();
            try{
                FloppyRecovery::Replay(*api_,handle,generation,drive);
                Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Restored);
                diagnostics_+="Original floppy sectors restored and verified.\r\n";
                writesStarted_=false;
            }catch(const std::exception& error){
                Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Unverified);
                diagnostics_+=std::string("Floppy recovery is pending: ")+error.what()+"\r\n";
            }
        }
#endif
        if(locked) {
            if(ok) Mark("Unlock volume",kImage);
            if(!api_->Unlock(handle) && ok) {failure=GetLastError();ok=false;}
        }
        api_->Close(handle);if(scratch) VirtualFree(scratch,0,MEM_RELEASE);
        error_=failure?failure:(ok?ERROR_SUCCESS:ERROR_WRITE_FAULT);Report(ok,drive);return ok;
    };
    DISK_GEOMETRY geometry={};
    Mark("Query geometry");
    if(!api_->Geometry(handle,geometry)) return finish(false);
    if(!geometry1440(geometry)) {SetLastError(ERROR_NOT_SUPPORTED);return finish(false);}
    Mark("Check initial media readiness");
    if(!api_->Verify(handle,generation)) return finish(false);
    if(expectedGeneration&&*expectedGeneration!=generation){SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
    Mark("Lock volume");
    if(!api_->Lock(handle)) return finish(false);
    locked=true;
    Mark("Dismount volume");
    if(!api_->Dismount(handle)) return finish(false);
    Mark("Check media after dismount");
    if(!api_->Verify(handle,current)) return finish(false);
    if(current!=generation) {SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
    Mark("Allocate transfer buffer");
    scratch=(BYTE*)VirtualAlloc(NULL,kTrack*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!scratch) {SetLastError(ERROR_NOT_ENOUGH_MEMORY);return finish(false);}
#ifdef HFV_APP_REMOVABLE_UNDO
    const auto key=RemovableUndo::FloppyKey(drive);
    try {
        if(RemovableUndo::Pending(key)){
            Mark("Recover interrupted floppy write");
            FloppyRecovery::Replay(*api_,handle,generation,drive);
        }
        if(RemovableUndo::Enabled()&&plannedTracks_){
            Mark("Read original floppy for recovery");
            if(progress)progress("Preparing recovery",0,160,context);
            undo=std::make_unique<RemovableUndo::Log>(key,FloppyRecovery::Range,
              [&](uint64_t at,DWORD count,void* bytes){FloppyRecovery::Read(*api_,handle,generation,at,count,bytes);});
        }
    }catch(const std::exception& e){
        diagnostics_+=std::string(e.what())+"\r\n";
        SetLastError(ERROR_READ_FAULT);
        return finish(false);
    }
#endif
    if(expectedOriginal) {
        for(DWORD offset=0;offset<kImage;offset+=kTrack) {
            Mark("Check original disk before copying",offset,kTrack);
            if(progress) progress("Checking original disk",offset/kTrack,160,context);
            if(!api_->Verify(handle,current)) return finish(false);
            if(current!=generation) {SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
            DWORD got=0;
            if(!api_->Read(handle,offset,kTrack,scratch,got)) return finish(false);
            if(got!=kTrack) {SetLastError(ERROR_READ_FAULT);return finish(false);}
            if(memcmp(scratch,expectedOriginal->data()+offset,kTrack)) {SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
        }
        if(progress) progress("Checking original disk",160,160,context);
    }
    if(!plannedTracks_) {
        Mark("Final media readiness check");
        if(!api_->Verify(handle,current)) return finish(false);
        if(current!=generation) {SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
        return finish(true);
    }
    for(DWORD offset:tracks) {
        Mark("Media readiness before write",offset,kTrack);
        if(progress) progress("Checking media before write",writtenTracks_,plannedTracks_,context);
        if(!api_->Verify(handle,current)) return finish(false);
        if(current!=generation) {SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
#ifdef HFV_APP_REMOVABLE_UNDO
        if(undo){
            try{
                FloppyRecovery::Read(*api_,handle,generation,offset,kTrack,scratch);
                undo->BeforeWrite(offset,kTrack,scratch,image.data()+offset);
            }catch(const std::exception& e){
                diagnostics_+=std::string(e.what())+"\r\n";
                SetLastError(ERROR_WRITE_FAULT);
                return finish(false);
            }
        }
#endif
        memcpy(scratch,image.data()+offset,kTrack);DWORD wrote=0;writesStarted_=true;
        Mark("WriteFile track transfer",offset,kTrack);
        if(progress) progress("Writing",writtenTracks_,plannedTracks_,context);
        bool wroteOk=api_->Write(handle,offset,kTrack,scratch,wrote);transferred_=wrote;
        if(!wroteOk) return finish(false);
        if(wrote!=kTrack) {SetLastError(ERROR_WRITE_FAULT);return finish(false);}
#if defined(HFV_TESTING) && defined(HFV_APP_REMOVABLE_UNDO)
        if(undo)RemovableUndo::TestCheckpoint(1);
#endif
        ++writtenTracks_;
        if(progress) progress("Writing",writtenTracks_,plannedTracks_,context);
    }
    Mark("Flush written data",kImage);
    if(progress) progress("Flushing written data",writtenTracks_,plannedTracks_,context);
    if(!api_->Flush(handle)) {
        flushError_=GetLastError();if(!flushError_)flushError_=ERROR_WRITE_FAULT;
        char line[160];sprintf(line,"Flush failed: error=%lu; continuing readback without treating the flush as successful\r\n",flushError_);diagnostics_+=line;
    }
    for(DWORD offset:tracks) {
        Mark("Media readiness before readback",offset,kTrack);
        if(progress) progress("Checking media before readback",verifiedTracks_,plannedTracks_,context);
        if(!api_->Verify(handle,current)) return finish(false);
        if(current!=generation) {SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
        DWORD got=0;
        Mark("ReadFile readback transfer",offset,kTrack);
        if(progress) progress("Verifying",verifiedTracks_,plannedTracks_,context);
        bool readOk=api_->Read(handle,offset,kTrack,scratch,got);transferred_=got;
        if(!readOk) return finish(false);
        stage_="Compare readback";
        if(got!=kTrack || memcmp(scratch,image.data()+offset,kTrack)) {SetLastError(ERROR_CRC);return finish(false);}
        ++verifiedTracks_;
        if(progress) progress("Verifying",verifiedTracks_,plannedTracks_,context);
    }
    Mark("Final media readiness check",kImage);
    if(!api_->Verify(handle,current)) return finish(false);
    if(current!=generation) {SetLastError(ERROR_MEDIA_CHANGED);return finish(false);}
    if(flushError_ && flushError_!=ERROR_INVALID_FUNCTION && flushError_!=ERROR_NOT_SUPPORTED) {
        Mark("Flush written data (all readbacks matched)",kImage);
        SetLastError(flushError_);return finish(false);
    }
    if(flushError_) diagnostics_+="Flush unsupported; accepted only after every write and readback matched and the final media check passed.\r\n";
#ifdef HFV_APP_REMOVABLE_UNDO
    if(undo){
        try{undo->Commit();}
        catch(const std::exception& e){
            diagnostics_+=std::string(e.what())+"\r\n";
            SetLastError(ERROR_WRITE_FAULT);
            return finish(false);
        }
    }
#endif
    return finish(true);
}
bool LoadFloppyHfs(const char* path,std::vector<BYTE>& image,std::string& error) {
    HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE) {error="Could not open the image.";return false;}
    LARGE_INTEGER size;DWORD got;bool ok=GetFileSizeEx(file,&size)&&size.QuadPart==kImage;
    if(ok) {image.resize(kImage);ok=ReadFile(file,image.data(),kImage,&got,NULL)&&got==kImage;}
    CloseHandle(file);
    if(!ok || !ValidateFloppyHfs(image)) {image.clear();error="Select a raw classic HFS image of exactly 1,474,560 bytes.";return false;}
    return true;
}
bool PrepareFloppyHfs(const char* name,std::vector<BYTE>& image,std::string& error) {
    if(!name || !*name || strlen(name)>27 || strchr(name,':')) {error="Use an HFS volume name of 1 to 27 bytes without a colon.";return false;}
    char folder[MAX_PATH],path[MAX_PATH];
    if(!GetTempPathA(MAX_PATH,folder) || !GetTempFileNameA(folder,"hfv",0,path)) {error="Could not create a temporary image.";return false;}
    HANDLE file=CreateFileA(path,GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);LARGE_INTEGER size;size.QuadPart=kImage;
    bool ok=file!=INVALID_HANDLE_VALUE && SetFilePointerEx(file,size,NULL,FILE_BEGIN)&&SetEndOfFile(file);
    if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    if(ok) ok=hfs_format(path,0,const_cast<char*>(name))==0;
    if(ok) ok=LoadFloppyHfs(path,image,error);
    DeleteFileA(path);if(!ok && error.empty()) error="Could not prepare the HFS filesystem image.";return ok;
}
