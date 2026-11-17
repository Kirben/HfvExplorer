#include "DeviceWait.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ReadOnlyCd.h"
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stddef.h>

namespace {
const DWORD kSector = 2048, kTransfer = 64 * 1024;
WORD be16(const BYTE* p) { return (WORD)((p[0] << 8) | p[1]); }
DWORD be32(const BYTE* p) { return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3]; }
struct ScsiRequest { SCSI_PASS_THROUGH_DIRECT request; ULONG padding; BYTE sense[32]; };
bool sendReadCommand(HANDLE h, ScsiRequest& packet, void* buffer, DWORD bytes) {
    packet.request.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    packet.request.DataIn = SCSI_IOCTL_DATA_IN;
    packet.request.DataBuffer = buffer;
    packet.request.DataTransferLength = bytes;
    packet.request.TimeOutValue = 5;
    packet.request.SenseInfoLength = sizeof(packet.sense);
    packet.request.SenseInfoOffset = offsetof(ScsiRequest, sense);
    DWORD returned = 0;
    if (!HfvMediaControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &packet, sizeof(packet),
                         &packet, sizeof(packet), &returned, NULL)) return false;
    if (packet.request.ScsiStatus || packet.request.DataTransferLength != bytes) {
        SetLastError(ERROR_IO_DEVICE); return false;
    }
    return true;
}
}

ReadOnlyCd::ReadOnlyCd() : handle_(INVALID_HANDLE_VALUE), scsiHandle_(INVALID_HANDLE_VALUE),
    scratch_(NULL), mediaSize_(0), volumeOffset_(0), volumeSize_(0), error_(0),
    fixture_(false), passThroughUsed_(false) {
    cache_init(&cache_);
    declaredVolumeSize_ = 0; lengthSize_ = tocSize_ = scsiSize_ = 0; scsiCapacityError_ = 0; devicePath_[0] = 0;
}
ReadOnlyCd::~ReadOnlyCd() { Close(); }
void ReadOnlyCd::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) HfvMediaClose(handle_);
    if (scsiHandle_ != INVALID_HANDLE_VALUE) HfvMediaClose(scsiHandle_);
    if (scratch_) VirtualFree(scratch_, 0, MEM_RELEASE);
    cache_final(&cache_);generation_=0;identity_=invalid_=false;cacheable_=true;
#ifdef HFV_CD_TESTING
    test=TestState{};
#endif
    handle_ = scsiHandle_ = INVALID_HANDLE_VALUE; scratch_ = NULL;
    mediaSize_ = volumeOffset_ = readBegin_ = readEnd_ = 0;tracks_.clear(); volumeSize_ = 0; error_ = 0;
    declaredVolumeSize_ = 0;
    lengthSize_ = tocSize_ = scsiSize_ = 0; scsiCapacityError_ = 0;
    fixture_ = passThroughUsed_ = false; devicePath_[0] = 0;
}
bool ReadOnlyCd::EnsureScsiHandle() {
    if (fixture_) return false;
    if (scsiHandle_ != INVALID_HANDLE_VALUE) return true;
    // Windows requires both access rights for the pass-through IOCTL. The only
    // commands issued on this handle are READ CAPACITY(10) and READ(10).
    scsiHandle_ = HfvMediaOpen(devicePath_, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    return scsiHandle_ != INVALID_HANDLE_VALUE;
}
bool ReadOnlyCd::ScsiCapacity() {
    if (!EnsureScsiHandle()) { scsiCapacityError_ = GetLastError(); return false; }
    ScsiRequest p = {}; p.request.CdbLength = 10; p.request.Cdb[0] = 0x25;
    if (!sendReadCommand(scsiHandle_, p, scratch_, 8)) { scsiCapacityError_ = GetLastError(); return false; }
    DWORD last = be32(scratch_), block = be32(scratch_ + 4);
    if (block != kSector || last == MAXDWORD) { scsiCapacityError_ = ERROR_NOT_SUPPORTED; SetLastError(ERROR_NOT_SUPPORTED); return false; }
    scsiSize_ = ((ULONGLONG)last + 1) * block; scsiCapacityError_ = 0;
    SelectCapacity();
    return true;
}
void ReadOnlyCd::SelectCapacity() {
    // The volume/disk-length IOCTL may describe only the mounted data extent.
    // Prefer whole-medium capacity from the CD TOC or READ CAPACITY instead.
    mediaSize_ = scsiSize_ ? scsiSize_ : (tocSize_ ? tocSize_ : lengthSize_);
    if(readEnd_>mediaSize_)readEnd_=mediaSize_;
}
bool ReadOnlyCd::ScsiRead(DWORD sector, WORD sectors, void* buffer) {
    if (!EnsureScsiHandle()) return false;
    ScsiRequest p = {}; p.request.CdbLength = 10; p.request.Cdb[0] = 0x28;
    p.request.Cdb[2] = (BYTE)(sector >> 24); p.request.Cdb[3] = (BYTE)(sector >> 16);
    p.request.Cdb[4] = (BYTE)(sector >> 8); p.request.Cdb[5] = (BYTE)sector;
    p.request.Cdb[7] = (BYTE)(sectors >> 8); p.request.Cdb[8] = (BYTE)sectors;
    if (!sendReadCommand(scsiHandle_, p, buffer, (DWORD)sectors * kSector)) return false;
    passThroughUsed_ = true; return true;
}
bool ReadOnlyCd::OpenDrive(int drive) {
    Close();
    if (drive < 0 || drive > 25) { error_ = ERROR_INVALID_PARAMETER; return false; }
    char root[] = "A:\\"; root[0] += (char)drive;
    if (GetDriveTypeA(root) != DRIVE_CDROM) { error_ = ERROR_BAD_DEVICE; return false; }
    sprintf(devicePath_, "\\\\.\\%c:", 'A' + drive);
    handle_ = HfvMediaOpen(devicePath_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (handle_ == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return false; }
    scratch_ = (BYTE*)VirtualAlloc(NULL, kTransfer, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!scratch_) { error_ = ERROR_NOT_ENOUGH_MEMORY; return false; }
    DWORD returned = 0; GET_LENGTH_INFORMATION length = {};
    if (HfvMediaControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length, sizeof(length), &returned, NULL)
        && length.Length.QuadPart > 0) lengthSize_ = (ULONGLONG)length.Length.QuadPart;
    if(!CheckIdentity()){DWORD saved=error_;Close();error_=saved;return false;}
    if(!LoadTracks()){DWORD saved=error_;Close();error_=saved?saved:ERROR_BAD_FORMAT;return false;}
    if(!CheckIdentity()){DWORD saved=error_;Close();error_=saved;return false;}
    SelectCapacity();
    if (!mediaSize_ && !ScsiCapacity()) { error_ = GetLastError(); return false; }
    readBegin_=0;readEnd_=mediaSize_;
    error_ = ERROR_SUCCESS; return true;
}

bool ReadOnlyCd::LoadTracks(){
 std::string reason;auto query=[&](CDROM_READ_TOC_EX& request,void* output,DWORD size,DWORD& returned,DWORD& error){if(!HfvMediaControl(handle_,IOCTL_CDROM_READ_TOC_EX,&request,sizeof(request),output,size,&returned,nullptr)){error=GetLastError();return false;}error=0;return true;};
 if(!CdMedia::ReadLayout(query,tracks_,error_,reason))return false;
 tocSize_=0;for(auto& t:tracks_)tocSize_=(std::max)(tocSize_,ULONGLONG(t.start+t.sectors)*2048);return !tracks_.empty();
}
bool ReadOnlyCd::SetReadRange(ULONGLONG begin,ULONGLONG size){
 if(invalid_||begin%2048||size%2048||!size||begin>mediaSize_||size>mediaSize_-begin){error_=ERROR_INVALID_PARAMETER;return false;}
 readBegin_=begin;readEnd_=begin+size;cache_clear(&cache_);return true;
}
bool ReadOnlyCd::SelectDataTrack(unsigned number){for(auto& t:tracks_)if(t.number==number&&t.data)return SetReadRange(t.start*2048,t.sectors*2048);error_=ERROR_NOT_SUPPORTED;return false;}
#ifdef HFV_CD_TESTING
bool ReadOnlyCd::OpenFixture(const char* path, ULONGLONG reportedLength) {
    Close(); fixture_ = true;
    handle_ = HfvMediaOpen(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (handle_ == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return false; }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_, &size)) { error_ = GetLastError(); return false; }
    lengthSize_ = reportedLength ? reportedLength : size.QuadPart;
    tocSize_ = size.QuadPart;
    SelectCapacity();
    scratch_ = (BYTE*)VirtualAlloc(NULL, kTransfer, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!scratch_) { error_ = ERROR_NOT_ENOUGH_MEMORY; return false; }
    readBegin_=0;readEnd_=mediaSize_;CdMedia::Track t;t.sectors=mediaSize_/2048;tracks_={t};return true;
}
bool ReadOnlyCd::SetFixtureTracks(const std::vector<CdMedia::Track>& tracks){tracks_=tracks;return !tracks.empty();}
#endif
bool ReadOnlyCd::ReadSectors(ULONGLONG offset, DWORD count, void* buffer) {
#ifdef HFV_CD_TESTING
    if(fixture_){++test.reads;test.bytes+=count;test.minOffset=(std::min)(test.minOffset,offset);test.maxEnd=(std::max)(test.maxEnd,offset+count);if(test.changeAfterRead==test.reads)++test.generation;
        if(test.readError||test.shortRead||(test.failAbove&&count>test.failAbove)){error_=test.readError?test.readError:test.shortRead?ERROR_READ_FAULT:ERROR_CRC;return false;}}
#endif
    LARGE_INTEGER pos; pos.QuadPart = offset;
    DWORD got = 0;
    if (HfvMediaSeek(handle_, pos, NULL, FILE_BEGIN) && HfvMediaRead(handle_, buffer, count, &got, NULL) && got == count)
        return true;
    DWORD fileError = GetLastError();
    if (!fixture_ && (offset / kSector) <= MAXDWORD && ScsiRead((DWORD)(offset / kSector), (WORD)(count / kSector), buffer))
        return true;
    error_ = fixture_ ? (got < count ? ERROR_HANDLE_EOF : fileError) : GetLastError();
    return false;
}
bool ReadOnlyCd::ReadFailure(DWORD error){cache_clear(&cache_);invalid_=true;error_=error?error:ERROR_READ_FAULT;SetLastError(error_);return false;}
bool ReadOnlyCd::CheckIdentity(){
    if(invalid_)return false;if(!cacheable_)return true;
    DWORD current=0,returned=0;
#ifdef HFV_CD_TESTING
    if(fixture_){++test.checks;if(test.noIdentity){if(identity_)return ReadFailure(ERROR_NOT_SUPPORTED);cacheable_=false;cache_clear(&cache_);return true;}current=test.generation;returned=sizeof(current);}
    else
#endif
    {
        if(!HfvMediaControl(handle_,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&current,sizeof(current),&returned,nullptr)){
            DWORD e=GetLastError();if(!identity_&&(e==ERROR_NOT_SUPPORTED||e==ERROR_INVALID_FUNCTION)){cacheable_=false;cache_clear(&cache_);return true;}
            return ReadFailure(e);
        }
    }
    if(returned<sizeof(current)){if(identity_)return ReadFailure(ERROR_NOT_SUPPORTED);cacheable_=false;cache_clear(&cache_);return true;}
    if(identity_&&current!=generation_)return ReadFailure(ERROR_MEDIA_CHANGED);
    generation_=current;identity_=true;return true;
}
bool ReadOnlyCd::ReadBytes(ULONGLONG offset,DWORD count,void* buffer){
    if(handle_==INVALID_HANDLE_VALUE||!scratch_||(!buffer&&count)){error_=ERROR_INVALID_HANDLE;return false;}
    if(invalid_)return false;
    if(offset<readBegin_||offset>readEnd_||count>readEnd_-offset){error_=ERROR_HANDLE_EOF;return false;}
    if(!count)return true;if(!CheckIdentity())return false;
    BYTE* out=(BYTE*)buffer;BYTE sector[kSector];
    while(count){
        ULONGLONG aligned=offset&~ULONGLONG(kSector-1),block=aligned/kSector;
        DWORD skip=DWORD(offset-aligned),chunk=min(count,kTransfer-skip);
        bool cached=cacheable_&&block<=INT_MAX;
        if(cached&&cache_get(&cache_,int(block),(char*)sector)){
            DWORD n=min(count,kSector-skip);memcpy(out,sector+skip,n);out+=n;offset+=n;count-=n;continue;
        }
        DWORD required=(skip+chunk+kSector-1)&~(kSector-1);
        if(required>readEnd_-aligned){error_=ERROR_HANDLE_EOF;return false;}
        // Original CD_READ_AHEAD_SECTORS=32. Clip speculation at real media EOF.
        DWORD transfer=cached?DWORD(min(ULONGLONG(kTransfer),(readEnd_-aligned)&~ULONGLONG(kSector-1))):required;
        if(!ReadSectors(aligned,transfer,scratch_)){
            DWORD error=error_;if(!CheckIdentity())return false;
            if(transfer==required||!ReadSectors(aligned,required,scratch_))return ReadFailure(transfer==required?error:error_);
            transfer=required;
        }
        if(!CheckIdentity())return false;
        if(cacheable_)for(DWORD n=0;n<transfer/kSector;++n)if(block+n<=INT_MAX)cache_put(&cache_,int(block+n),(char*)scratch_+n*kSector,kSector);
        memcpy(out,scratch_+skip,chunk);out+=chunk;offset+=chunk;count-=chunk;
    }
    error_=ERROR_SUCCESS;return true;
}
bool ReadOnlyCd::CheckVolume(ULONGLONG offset, ULONGLONG length, bool allowShortened) {
    BYTE mdb[512];
    if (length < 1536 || length > LONG_MAX || offset < readBegin_ || offset > readEnd_ || readEnd_ - offset < 1536 ||
        (!allowShortened && length > readEnd_ - offset) ||
        !ReadBytes(offset + 1024, sizeof(mdb), mdb)) return false;
    ULONGLONG available = min(length, readEnd_ - offset);
    DWORD allocation = be32(mdb + 20); WORD blocks = be16(mdb + 18), first = be16(mdb + 28);
    if (be16(mdb) != 0x4244 || !blocks || allocation < 512 || allocation % 512 ||
        mdb[36] == 0 || mdb[36] > 27 || (ULONGLONG)first * 512 + (ULONGLONG)blocks * allocation > length)
        return false;
    if (available < length) {
        // Only the read-only CD adapter accepts a shortened APM partition.
        // Require its bitmap and initial catalog/extents trees to be present;
        // never manufacture missing bytes or expand the transport's bounds.
        ULONGLONG bitmapEnd = (ULONGLONG)be16(mdb + 14) * 512 + ((blocks + 4095) / 4096) * 512;
        if (bitmapEnd > available) return false;
        const unsigned records[] = {134,150};
        for (unsigned r = 0; r < ARRAYSIZE(records); ++r) {
            for (unsigned n = 0; n < 3; ++n) {
                const BYTE* extent = mdb + records[r] + n * 4;
                DWORD start = be16(extent), count = be16(extent + 2);
                if (count && (start + count > blocks ||
                    (ULONGLONG)first * 512 + (ULONGLONG)(start + count) * allocation > available)) return false;
            }
        }
    }
    volumeOffset_ = offset; volumeSize_ = (DWORD)available; declaredVolumeSize_ = (DWORD)length; return true;
}
bool ReadOnlyCd::FindHfs() {
 volumeOffset_=0;volumeSize_=declaredVolumeSize_=0;
 for(auto i=tracks_.rbegin();i!=tracks_.rend();++i)if(i->data){if(!SelectDataTrack(i->number))return false;if(FindHfsRange())return true;if(invalid_)return false;}
 error_=ERROR_UNRECOGNIZED_VOLUME;return false;
}
bool ReadOnlyCd::FindHfsRange() {
    volumeOffset_ = 0; volumeSize_ = declaredVolumeSize_ = 0;
    BYTE block[512];
    if (CheckVolume(readBegin_, readEnd_-readBegin_)) return true;
    if (!ReadBytes(readBegin_, sizeof(block), block)) return false;
    DWORD mapBlock = be16(block) == 0x4552 ? be16(block + 2) : 512;
    if (!fixture_ && be16(block) == 0x4552 && (mapBlock == 512 || mapBlock == 2048 || mapBlock == 4096) &&
        (ULONGLONG)be32(block + 4) * mapBlock > mediaSize_ && !scsiSize_)
        ScsiCapacity(); // Cross-check a short Windows extent; never trust disk metadata as capacity.
    if (mapBlock == 512 || mapBlock == 2048 || mapBlock == 4096) {
        if (ReadBytes(readBegin_+mapBlock, sizeof(block), block) && be16(block) == 0x504D) {
            DWORD entries = be32(block + 4);
            if (entries > 4096 || (ULONGLONG)(entries + 1) * mapBlock > readEnd_-readBegin_) {
                error_ = ERROR_BAD_FORMAT; return false;
            }
            for (DWORD n = 1; n <= entries; ++n) {
                if (!ReadBytes(readBegin_+(ULONGLONG)n * mapBlock, sizeof(block), block) || be16(block) != 0x504D) break;
                if (!memcmp(block + 48,"Apple_HFS",9)&&block[57]==0){
                    ULONGLONG start=ULONGLONG(be32(block+8))*mapBlock,length=ULONGLONG(be32(block+12))*mapBlock;
                    if(CheckVolume(readBegin_+start,length,true)||(readBegin_&&CheckVolume(start,length,true)))return true;
                }
            }
            error_ = ERROR_UNRECOGNIZED_VOLUME; return false;
        }
    }
    // Some historical hybrid CDs omit a usable map. Match the legacy search,
    // but validate MDB allocation geometry before accepting a candidate.
    ULONGLONG end = min(readEnd_,readBegin_+(ULONGLONG)1024 * 1024);
    BYTE scan[32768];
    for (ULONGLONG base = readBegin_; base < end; base += sizeof(scan)) {
        DWORD bytes = (DWORD)min((ULONGLONG)sizeof(scan), end - base);
        if (!ReadBytes(base, bytes, scan)) return false;
        for (DWORD n = 0; n + 512 <= bytes; n += 512) {
            ULONGLONG mdb = base + n;
            if (mdb >= readBegin_+1024 && be16(scan + n) == 0x4244 &&
                CheckVolume(mdb - 1024, readEnd_ - (mdb - 1024))) return true;
        }
    }
    error_ = ERROR_UNRECOGNIZED_VOLUME; return false;
}
bool ReadOnlyCd::ReadVolume(ULONGLONG offset, DWORD count, void* buffer) {
    if (!volumeSize_ || offset > volumeSize_ || count > volumeSize_ - offset) { error_ = ERROR_HANDLE_EOF; return false; }
    return ReadBytes(volumeOffset_ + offset, count, buffer);
}
