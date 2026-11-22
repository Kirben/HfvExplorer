// SPDX-License-Identifier: GPL-2.0-or-later
// Independent implementation of the MFS disk structures documented by Apple.
#include "StorageOutcome.h"
#include "MfsImage.h"
#include "ImageHandleTransfer.h"
#include "WindowsTemporaryFile.h"
#include <algorithm>
#include <set>
#include <limits>
#include <array>
#include <stdexcept>

static unsigned be16(const BYTE* p) { return unsigned(p[0])*256+p[1]; }
static uint32_t be32(const BYTE* p) { return uint32_t(be16(p))*65536+be16(p+2); }
static void put16(BYTE* p,unsigned v) {p[0]=BYTE(v>>8);p[1]=BYTE(v);}
static void put32(BYTE* p,uint32_t v) {put16(p,v>>16);put16(p+2,v);}

MfsImageSource::MfsImageSource():file_(INVALID_HANDLE_VALUE),length_(0) {}
MfsImageSource::~MfsImageSource() {if(file_!=INVALID_HANDLE_VALUE) CloseHandle(file_);}
bool MfsImageSource::Open(const wchar_t* path,std::string& error,bool write) {
    if(file_!=INVALID_HANDLE_VALUE) {CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}
    length_=0;writable_=false;
    // Regular images only. All readers/writers exclude competing write handles.
    if(!path || wcsncmp(path,L"\\\\.\\",4)==0 || wcsncmp(path,L"\\\\?\\",4)==0) {error="Choose a regular disk image file.";return false;}
    path_=path;
    try { file_=ImageHandleTransfer::Open(path,write); }
    catch(const std::exception& failure) { error=failure.what(); return false; }
    if(file_==INVALID_HANDLE_VALUE)file_=CreateFileW(path,GENERIC_READ|(write?GENERIC_WRITE:0),FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file_==INVALID_HANDLE_VALUE) {error="Could not open the image (it may be in use by another process).";return false;}
    LARGE_INTEGER size;
    if(GetFileType(file_)!=FILE_TYPE_DISK || !GetFileSizeEx(file_,&size) || size.QuadPart<0) {
        CloseHandle(file_);file_=INVALID_HANDLE_VALUE;error="Could not determine image length.";return false;
    }
    length_=size.QuadPart;writable_=write;return true;
}
bool MfsImageSource::ReadAt(uint64_t offset,void* bytes,size_t length) {
    if(file_==INVALID_HANDLE_VALUE || offset>length_ || length>length_-offset || length>MAXDWORD) return false;
    LARGE_INTEGER position;position.QuadPart=offset;DWORD got;
    return SetFilePointerEx(file_,position,NULL,FILE_BEGIN) && ReadFile(file_,bytes,DWORD(length),&got,NULL) && got==length;
}
#ifdef HFV_TESTING
thread_local bool MfsImageSource::crashAfterSectorWrite=false;
#endif
bool MfsImageSource::WriteAt(uint64_t offset,const void* bytes,size_t length) {
    if(!writable_||file_==INVALID_HANDLE_VALUE||offset>length_||length>length_-offset||length>MAXDWORD)return false;
    LARGE_INTEGER position;position.QuadPart=offset;DWORD written=0;
    const bool ok=SetFilePointerEx(file_,position,NULL,FILE_BEGIN)&&WriteFile(file_,bytes,DWORD(length),&written,NULL)&&written==length;
#ifdef HFV_TESTING
    if(ok && length==512 && crashAfterSectorWrite && FlushFileBuffers(file_))ExitProcess(81);
#endif
    return ok;
}
bool MfsImageSource::Flush(){return writable_&&FlushFileBuffers(file_)!=FALSE;}

namespace {
uint32_t MacNow(){FILETIME time;GetSystemTimeAsFileTime(&time);ULARGE_INTEGER value;value.LowPart=time.dwLowDateTime;value.HighPart=time.dwHighDateTime;auto seconds=value.QuadPart/10000000ULL;return seconds>=9561628800ULL&&seconds-9561628800ULL<=UINT32_MAX?uint32_t(seconds-9561628800ULL):0;}
uint32_t Checksum(const BYTE* p,size_t length){uint32_t result=0;for(size_t at=0;at+1<length;at+=2){result+=be16(p+at);result=(result>>1)|(result<<31);}return result;}
struct VectorSource:MfsSource {
 const std::vector<BYTE>& bytes;explicit VectorSource(const std::vector<BYTE>& b):bytes(b){}
 uint64_t Length()const override{return bytes.size();}
 bool ReadAt(uint64_t at,void* p,size_t n)override{if(at>bytes.size()||n>bytes.size()-at)return false;if(n)memcpy(p,bytes.data()+size_t(at),n);return true;}
};
std::string RecoveryFile(const std::vector<BYTE>& original){
 if(original.size()>MAXDWORD)return {};
 wchar_t path[MAX_PATH];
 const auto file=CreateWindowsTemporaryFile(L"MFS",path,GENERIC_WRITE|DELETE,FILE_ATTRIBUTE_NORMAL);
 if(file==INVALID_HANDLE_VALUE)return {};
 struct Close{HANDLE file;~Close(){CloseHandle(file);}} close{file};
 char utf8[4*MAX_PATH];
 const auto bytes=WideCharToMultiByte(CP_UTF8,0,path,-1,utf8,ARRAYSIZE(utf8),nullptr,nullptr);
 DWORD written=0;
 if(bytes>0&&WriteFile(file,original.data(),DWORD(original.size()),&written,nullptr)&&
    written==original.size()&&FlushFileBuffers(file))
  return std::string(utf8,bytes-1);
 FILE_DISPOSITION_INFO remove{TRUE};
 SetFileInformationByHandle(file,FileDispositionInfo,&remove,sizeof(remove));
 return {};
}
bool Apply(MfsSource& source,const std::vector<BYTE>& original,const std::vector<BYTE>& desired,
           uint64_t metadataEnd,uint64_t directoryStart,uint64_t directoryEnd,uint64_t dataEnd,
           std::string& error,bool& recoveryRequired,size_t lockOffset=SIZE_MAX){
 std::vector<std::pair<size_t,size_t>> payload,metadata,touched;
 for(size_t at=0;at<desired.size();at+=512){auto n=(std::min)(size_t(512),desired.size()-at);if(!memcmp(original.data()+at,desired.data()+at,n))continue;
  bool meta=at<metadataEnd||(at<directoryEnd&&at+n>directoryStart)||at+n>dataEnd;(meta?metadata:payload).push_back({at,n});}
 if(payload.empty()&&metadata.empty())return true;
 auto write=[&](const std::vector<std::pair<size_t,size_t>>& chunks){for(auto chunk:chunks){touched.push_back(chunk);if(!source.WriteAt(chunk.first,desired.data()+chunk.first,chunk.second))return false;}return true;};
 try{if(write(payload)&&source.Flush()&&write(metadata)&&source.Flush())return true;}catch(...){}
 bool restored=true;for(auto i=touched.rbegin();i!=touched.rend();++i){try{if(!source.WriteAt(i->first,original.data()+i->first,i->second))restored=false;}catch(...){restored=false;}}
 try{if(!source.Flush())restored=false;}catch(...){restored=false;}
 if(restored) {
  BYTE verify[512];
  for(const auto& part:touched) {
   try { if(!source.ReadAt(part.first,verify,part.second) || memcmp(verify,original.data()+part.first,part.second)) restored=false; }
   catch(...) { restored=false; }
  }
 }
 if(!touched.empty())Storage::OutcomeEvidence::NoteRollback(restored?Storage::Rollback::Restored:Storage::Rollback::Failed);
 error="MFS write or flush failed; original bytes were restored.";
 if(!restored){
  recoveryRequired=true;source.DisableWrites();
  // The recovery copy preserves the pre-operation bytes and checksum.
  auto backup=original;
  if(lockOffset<backup.size())backup[lockOffset]&=0x7f;
  auto file=RecoveryFile(backup);
  error="MFS rollback failed; further writes are disabled.";
  if(!file.empty())error+=" Original complete image saved at: "+file+". Verify the source before manual recovery.";
  else error+=" Could not save the original image; preserve the source for recovery.";
 }
 return false;
}
void PackedMap(std::vector<BYTE>& bytes,uint64_t base,const std::vector<uint16_t>& map){
 size_t length=(map.size()*12+7)/8;memset(bytes.data()+size_t(base)+1088,0,length);
 for(size_t i=0;i<map.size();++i)for(unsigned bit=0;bit<12;++bit)if(map[i]&(1u<<(11-bit))){size_t at=i*12+bit;bytes[size_t(base)+1088+at/8]|=BYTE(1u<<(7-at%8));}
}
void ContainerChecksums(std::vector<BYTE>& bytes,uint64_t base,uint64_t length,uint32_t tags,const std::vector<BYTE>* old=nullptr){
 if(!base)return;
 // Tags are scavenger hints, not file data. Clear hints for changed sectors so
 // deleted or reallocated blocks never retain a misleading old file identity.
 if(tags&&old)for(uint64_t block=0;block<length/512;++block)if(memcmp(bytes.data()+size_t(base+block*512),old->data()+size_t(base+block*512),512))memset(bytes.data()+size_t(base+length+block*12),0,12);
 put32(bytes.data()+72,Checksum(bytes.data()+size_t(base),size_t(length)));
 put32(bytes.data()+76,tags>12?Checksum(bytes.data()+size_t(base+length+12),tags-12):0);
}
}
bool MfsImage::EncodeName(const std::wstring& name,std::string& bytes,std::string& error,unsigned maximum){
 if(name.empty()||name.find(L':')!=name.npos||name.find(L'\0')!=name.npos){error="MFS names must be nonempty and contain neither colon nor NUL.";return false;}
 BOOL lost=FALSE;int n=WideCharToMultiByte(10000,WC_NO_BEST_FIT_CHARS|WC_COMPOSITECHECK,name.data(),int(name.size()),nullptr,0,nullptr,&lost);
 if(!n||lost||unsigned(n)>maximum){error="Name cannot be represented within the MFS MacRoman name limit.";return false;}
 bytes.assign(size_t(n),0);lost=FALSE;if(!WideCharToMultiByte(10000,WC_NO_BEST_FIT_CHARS|WC_COMPOSITECHECK,name.data(),int(name.size()),&bytes[0],n,nullptr,&lost)||lost){error="Name is not losslessly representable in MacRoman.";return false;}return true;
}
namespace {
const std::array<BYTE,256>& MfsNameOrder(){
 static const std::array<BYTE,256> upper=[](){std::array<BYTE,256> table{};for(unsigned i=0;i<256;++i){table[i]=BYTE(i);std::string byte(1,char(i));auto wide=MfsDisplayName(byte);wchar_t capital[4]{};if(!wide.empty()&&LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,wide.data(),int(wide.size()),capital,4)==1){BOOL lost=FALSE;char encoded=0;if(WideCharToMultiByte(10000,WC_NO_BEST_FIT_CHARS,capital,1,&encoded,1,nullptr,&lost)==1&&!lost)table[i]=BYTE(encoded);}}return table;}();
 return upper;
}
}
std::string MfsImage::NameKey(std::string name){
 const auto& order=MfsNameOrder();
 for(auto& byte:name)byte=char(order[BYTE(byte)]);
 return name;
}
bool MfsImage::EqualNames(const std::string& a,const std::string& b){
 const auto& order=MfsNameOrder();
 return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),
     [&](char left,char right){return order[BYTE(left)]==order[BYTE(right)];});
}
void MfsImage::AuditWrites(const std::vector<bool>& claimed){
 if(volumeFlags_&0x8080)writeError_="MFS volume is locked.";
 else if(length_>20*1024*1024)writeError_="MFS writing is limited to 20 MiB volumes.";
 else {
  unsigned free=0;for(size_t i=0;i<map_.size();++i){if(!map_[i])++free;else if(!claimed[i])writeError_="MFS has unowned allocated blocks; repair it before writing.";}
  if(free!=freeBlocks_)writeError_="MFS free-block count disagrees with the allocation map.";
  for(size_t i=0;i<entries_.size();++i)for(size_t j=0;j<i;++j)if(entries_[i].version==entries_[j].version&&EqualNames(entries_[i].name,entries_[j].name))writeError_="MFS contains ambiguous same-version names.";
 }
 if(base_&&source_->Writable()){
  std::vector<BYTE> bytes;if(source_->Length()>22*1024*1024||source_->Length()!=base_+length_+tagBytes_||(tagBytes_&&tagBytes_!=length_/512*12))writeError_="Unsupported Disk Copy 4.2 tag/container layout for writing.";
  else {bytes.resize(size_t(source_->Length()));if(!source_->ReadAt(0,bytes.data(),bytes.size()))writeError_="Cannot verify Disk Copy 4.2 checksums.";
   else if(be32(bytes.data()+72)!=Checksum(bytes.data()+84,size_t(length_))||be32(bytes.data()+76)!=(tagBytes_>12?Checksum(bytes.data()+size_t(84+length_+12),tagBytes_-12):0))writeError_="Disk Copy 4.2 checksum mismatch; writing is disabled.";}
 }
}
bool MfsImage::Guard(){if(!Writable()){error_=writeError_.empty()?"MFS image is read-only or must be reopened after an error.":writeError_;return false;}return true;}
size_t MfsImage::Index(uint32_t id)const{for(size_t i=0;i<entries_.size();++i)if(entries_[i].id==id)return i;return entries_.size();}
bool MfsImage::Snapshot(std::vector<BYTE>& bytes){if(!source_||source_->Length()>22*1024*1024)return Fail("MFS image exceeds the bounded write snapshot limit.");bytes.resize(size_t(source_->Length()));return source_->ReadAt(0,bytes.data(),bytes.size())||Fail("Cannot read original MFS image before writing.");}
bool MfsImage::Serialize(std::vector<BYTE>& bytes,const std::vector<MfsEntry>& entries,const std::vector<uint16_t>& map,uint32_t next){
 if(entries.size()>UINT16_MAX)return Fail("MFS file-count field is full.");
 uint64_t begin=base_+uint64_t(directoryStart_)*512,end=begin+uint64_t(directoryBlocks_)*512;memset(bytes.data()+size_t(begin),0,size_t(end-begin));uint64_t at=begin;
 for(const auto& e:entries){if(e.name.empty()||e.name.size()>255||e.name.find(':')!=e.name.npos||e.name.find('\0')!=e.name.npos)return Fail("Invalid MFS filename.");size_t n=(51+e.name.size()+1)&~size_t(1);if((at-begin)%512+n>512)at+=512-(at-begin)%512;if(at+n>end)return Fail("The fixed MFS file directory is full.");
  auto p=bytes.data()+size_t(at);p[0]=e.flags|0x80;p[1]=e.version;memcpy(p+2,e.finderInfo,16);put32(p+18,e.id);
  for(unsigned f=0;f<2;++f){auto& fork=e.forks[f];put16(p+22+f*10,fork.blocks.empty()?0:fork.blocks.front());put32(p+24+f*10,fork.length);put32(p+28+f*10,fork.physicalLength);}put32(p+42,e.created);put32(p+46,e.modified);p[50]=BYTE(e.name.size());memcpy(p+51,e.name.data(),e.name.size());at+=n;
 }
 PackedMap(bytes,base_,map);unsigned free=0;for(auto v:map)free+=v==0;put16(bytes.data()+size_t(base_)+1036,unsigned(entries.size()));put16(bytes.data()+size_t(base_)+1058,free);put32(bytes.data()+size_t(base_)+1054,next);return true;
}
bool MfsImage::Commit(std::vector<BYTE>& desired){
 std::vector<BYTE> original;
 if(!Snapshot(original))return false;
 ContainerChecksums(desired,base_,length_,tagBytes_,&original);
 VectorSource verify(desired);
 MfsImage check;
 if(!check.Open(verify))return Fail("Internal MFS mutation produced an invalid filesystem.");

 const auto flags=size_t(base_)+1034;
 const bool fileImage=dynamic_cast<MfsImageSource*>(source_)!=nullptr;
 auto store=[&](const std::vector<BYTE>& bytes,size_t offset,size_t count){
  return source_->WriteAt(offset,bytes.data()+offset,count)&&source_->Flush();
 };
 auto fence=[&](const char* error){
  recoveryRequired_=true;usable_=false;source_->DisableWrites();
  Storage::OutcomeEvidence::NoteRollback(Storage::Rollback::Failed);
  return Fail(error);
 };
 std::unique_ptr<MfsSource::CatalogUndo> undo;
 try{undo=source_->StageCatalogUndo(original);}
 catch(const std::exception& failure){return Fail(failure.what());}
 if(fileImage){
  // Keep the on-disk MFS software lock set throughout writes and rollback.
  original[flags]|=0x80;
  desired[flags]|=0x80;
  if(!store(original,flags,2))return fence("Cannot flush the MFS write lock.");
 }
 const bool applied=Apply(*source_,original,desired,base_+1088+(map_.size()*12+7)/8,
    base_+uint64_t(directoryStart_)*512,base_+uint64_t(directoryStart_+directoryBlocks_)*512,
    base_+length_,error_,recoveryRequired_,fileImage?flags:SIZE_MAX);
 if(fileImage&&!recoveryRequired_){
  auto& finalBytes=applied?desired:original;
  finalBytes[flags]&=0x7f;
  // Apply has already flushed the checksum for the unlocked image.
  // The transient mismatch while locked also fences Disk Copy writes.
  if(!store(finalBytes,flags,2)){
   if(applied)Storage::OutcomeEvidence::NoteRetainedChange();
   return fence("Cannot confirm the MFS write lock was cleared.");
  }
 }
 if(undo&&!recoveryRequired_){
  // Both a successful update and a verified rollback leave a complete image.
  try{undo->Complete();}
  catch(...){return fence("Cannot confirm the MFS catalog update. Reopen the image for recovery.");}
 }
 if(!applied){usable_=false;return false;}
 auto source=source_;return Open(*source);
}
bool MfsImage::Put(const MfsEntry& input,Reader read,bool overwrite,uint32_t& id){
 if(!Guard())return false;MfsEntry e=input;size_t found=entries_.size();
 for(size_t i=0;i<entries_.size();++i)if(entries_[i].version==e.version&&EqualNames(entries_[i].name,e.name)){found=i;break;}
 if(found<entries_.size()&&(!overwrite||(entries_[found].flags&1)))return Fail("MFS destination exists or is locked.");
 auto entries=entries_;auto map=map_;uint32_t next=nextId_;
 if(found<entries.size()){e.id=entries[found].id;if(!e.created)e.created=entries[found].created;}
 else {uint64_t candidate=(std::max)(uint64_t(next),uint64_t(1));for(auto& item:entries)candidate=(std::max)(candidate,uint64_t(item.id)+1);if(candidate>=UINT32_MAX)return Fail("MFS file numbers are exhausted.");e.id=uint32_t(candidate);next=e.id+1;}
 if(!e.datesValid&&!e.created)e.created=MacNow();if(!e.datesValid&&!e.modified)e.modified=MacNow();e.flags=BYTE((found<entries_.size()?entries_[found].flags&~1:0x80)|(e.flags&1));
 // Keep the original forks allocated until both replacement forks have space.
 // Payload writes must leave the data referenced by the old catalog intact.
 for(unsigned f=0;f<2;++f){uint64_t blocks=(uint64_t(e.forks[f].length)+allocationSize_-1)/allocationSize_;if(blocks>map.size())return Fail("File exceeds MFS volume capacity.");e.forks[f].blocks.clear();for(size_t block=0;block<map.size()&&e.forks[f].blocks.size()<blocks;++block)if(!map[block]){e.forks[f].blocks.push_back(uint16_t(block+2));map[block]=1;}
  if(e.forks[f].blocks.size()!=blocks)
   return Fail(found<entries.size()?"Not enough free space to overwrite without reusing the original forks.":"MFS disk is full.");
  if(blocks*allocationSize_>UINT32_MAX)return Fail("MFS physical fork length overflow.");e.forks[f].physicalLength=uint32_t(blocks*allocationSize_);for(size_t i=0;i<e.forks[f].blocks.size();++i)map[e.forks[f].blocks[i]-2]=i+1<e.forks[f].blocks.size()?e.forks[f].blocks[i+1]:1;}
 if(found<entries.size()){
  for(const auto& original:entries[found].forks)
   for(const auto block:original.blocks)map[block-2]=0;
  entries[found]=e;
 }else entries.push_back(e);
 std::vector<BYTE> desired;if(!Snapshot(desired)||!Serialize(desired,entries,map,next))return false;
 for(unsigned f=0;f<2;++f){uint64_t logical=0;for(auto block:e.forks[f].blocks){auto p=desired.data()+size_t(base_+allocationStart_+uint64_t(block-2)*allocationSize_);memset(p,0,allocationSize_);size_t n=size_t((std::min)(uint64_t(allocationSize_),uint64_t(e.forks[f].length)-logical));if(n)read(f,logical,p,n);logical+=n;}}
 if(!Commit(desired))return false;id=e.id;return true;
}
bool MfsImage::SetMetadata(uint32_t id,const MfsEntry& desired){
 if(!Guard())return false;size_t index=Index(id);if(index==entries_.size())return Fail("MFS file no longer exists.");
 auto e=entries_[index];for(auto& item:entries_)if(item.id!=id&&item.version==desired.version&&EqualNames(item.name,desired.name))return Fail("An MFS file with this name and version already exists.");
 e.name=desired.name;e.version=desired.version;e.flags=BYTE((e.flags&~1)|(desired.flags&1));e.created=desired.created;e.modified=desired.modified;memcpy(e.finderInfo,desired.finderInfo,16);auto entries=entries_;entries[index]=e;
 std::vector<BYTE> bytes;if(!Snapshot(bytes)||!Serialize(bytes,entries,map_,nextId_))return false;return Commit(bytes);
}
bool MfsImage::Rename(uint32_t id,const std::string& name){auto index=Index(id);if(index==entries_.size())return Fail("MFS file no longer exists.");if(entries_[index].flags&1)return Fail("MFS file is locked.");auto e=entries_[index];e.name=name;return SetMetadata(id,e);}
bool MfsImage::Remove(uint32_t id){
 if(!Guard())return false;size_t index=Index(id);if(index==entries_.size())return Fail("MFS file no longer exists.");if(entries_[index].flags&1)return Fail("MFS file is locked.");auto map=map_;auto entries=entries_;for(auto& f:entries[index].forks)for(auto block:f.blocks)map[block-2]=0;entries.erase(entries.begin()+index);std::vector<BYTE> bytes;if(!Snapshot(bytes)||!Serialize(bytes,entries,map,nextId_))return false;return Commit(bytes);
}
bool MfsImage::RenameVolume(const std::string& name){if(!Guard())return false;if(name.empty()||name.size()>27||name.find(':')!=name.npos||name.find('\0')!=name.npos)return Fail("MFS volume names must have 1-27 MacRoman bytes.");std::vector<BYTE> bytes;if(!Snapshot(bytes))return false;memset(bytes.data()+size_t(base_)+1060,0,28);bytes[size_t(base_)+1060]=BYTE(name.size());memcpy(bytes.data()+size_t(base_)+1061,name.data(),name.size());if(base_){memset(bytes.data(),0,64);bytes[0]=BYTE(name.size());memcpy(bytes.data()+1,name.data(),name.size());}return Commit(bytes);}
bool MfsImage::Format(MfsSource& source,const std::string& name,uint64_t dataBytes,bool dc42,std::string& error){
 if(!source.Writable()||dataBytes<128*1024||dataBytes>20*1024*1024||dataBytes%512||name.empty()||name.size()>27||name.find(':')!=name.npos||name.find('\0')!=name.npos){error="Use a writable 128 KiB-20 MiB image (512-byte multiple) and a 1-27 byte MacRoman volume name.";return false;}
 uint64_t base=dc42?84:0,tags=dc42&&(dataBytes==400*1024||dataBytes==800*1024)?dataBytes/512*12:0;
 if(dc42&&dataBytes!=400*1024&&dataBytes!=800*1024&&dataBytes!=720*1024&&dataBytes!=1440*1024){error="Disk Copy 4.2 formatting requires a standard 400/800/720/1440 KiB size.";return false;}
 if(source.Length()!=base+dataBytes+tags){error="Image length does not match the chosen MFS container/size.";return false;}
 std::vector<BYTE> old(size_t(source.Length()),0),bytes(old.size(),0);if(!source.ReadAt(0,old.data(),old.size())){error="Cannot snapshot the image before formatting.";return false;}
 unsigned directory=4,directoryBlocks=unsigned((std::max)(uint64_t(12),(std::min)(uint64_t(256),dataBytes/32768))),first=directory+directoryBlocks;
 uint64_t allocation=((dataBytes-uint64_t(first)*512+639)/640+511)/512*512;allocation=(std::max)(uint64_t(1024),allocation);unsigned count=unsigned((dataBytes-uint64_t(first)*512)/allocation);
 auto mdb=bytes.data()+size_t(base)+1024;put16(mdb,0xd2d7);put32(mdb+2,MacNow());put16(mdb+14,directory);put16(mdb+16,directoryBlocks);put16(mdb+18,count);put32(mdb+20,uint32_t(allocation));put32(mdb+24,uint32_t(allocation));put16(mdb+28,first);put32(mdb+30,1);put16(mdb+34,count);mdb[36]=BYTE(name.size());memcpy(mdb+37,name.data(),name.size());
 if(dc42){bytes[0]=BYTE(name.size());memcpy(bytes.data()+1,name.data(),name.size());put32(bytes.data()+64,uint32_t(dataBytes));put32(bytes.data()+68,uint32_t(tags));bytes[80]=dataBytes==400*1024?0:dataBytes==800*1024?1:dataBytes==720*1024?2:3;bytes[81]=dataBytes==400*1024?0x12:0x22;put16(bytes.data()+82,0x0100);ContainerChecksums(bytes,base,dataBytes,uint32_t(tags));}
 VectorSource verify(bytes);MfsImage image;if(!image.Open(verify)){error=image.Error();return false;}bool recovery=false;return Apply(source,old,bytes,base+1088+(count*12+7)/8,base+directory*512,base+first*512,base+dataBytes,error,recovery);
}
bool MfsImage::Recognizes(MfsSource& source) {
    BYTE signature[2],header[84];
    if(source.ReadAt(1024,signature,2) && be16(signature)==0xd2d7) return true;
    return source.ReadAt(0,header,84) && header[0]<=63 && be16(header+82)==0x0100 &&
        source.ReadAt(84+1024,signature,2) && be16(signature)==0xd2d7;
}
bool MfsImage::Fail(const char* error) {error_=error;return false;}
bool MfsImage::Read(uint64_t offset,void* bytes,size_t length) {
    if(offset>length_ || length>length_-offset || !source_->ReadAt(base_+offset,bytes,length)) return Fail("Image is truncated or unreadable.");
    return true;
}
bool MfsImage::Chain(MfsFork& fork,uint16_t first,std::vector<bool>& claimed) {
    if(fork.physicalLength%allocationSize_ || fork.length>fork.physicalLength ||
       fork.physicalLength/allocationSize_>map_.size()) return Fail("Invalid fork length.");
    unsigned blocks=fork.physicalLength/allocationSize_;
    if(!blocks) return first==0 || Fail("Empty fork has an allocation chain.");
    for(unsigned i=0;i<blocks;++i) {
        if(first<2 || first>=map_.size()+2 || claimed[first-2])
            return Fail("Invalid, cyclic or cross-linked allocation chain.");
        claimed[first-2]=true;fork.blocks.push_back(first);
        first=map_[first-2];
    }
    return first==1 || Fail("Allocation chain does not match the fork length.");
}
bool MfsImage::Open(MfsSource& source) {
    source_=nullptr;entries_.clear();map_.clear();name_.clear();error_.clear();
    allocationSize_=0;freeBlocks_=0;base_=0;length_=source.Length();writeError_.clear();usable_=true;recoveryRequired_=false;tagBytes_=0;
    BYTE mdb[64],header[84];
    bool raw=source.ReadAt(1024,mdb,sizeof(mdb)) && be16(mdb)==0xd2d7;
    if(!raw) {
        if(!source.ReadAt(0,header,sizeof(header)) || header[0]>63 || be16(header+82)!=0x0100 ||
           !source.ReadAt(84+1024,mdb,sizeof(mdb)) || be16(mdb)!=0xd2d7) return Fail("Not a supported MFS disk image.");
        uint64_t data=be32(header+64),tags=be32(header+68);
        if(data%512 || data<1536 || data>source.Length()-84 || tags>source.Length()-84-data)
            return Fail("Invalid Disk Copy 4.2 container lengths.");
        base_=84;length_=data;tagBytes_=uint32_t(tags);
    }
    source_=&source;
    unsigned count=be16(mdb+18),directory=be16(mdb+14),directoryBlocks=be16(mdb+16);
    unsigned fileCount=be16(mdb+12),nameLength=mdb[36];
    allocationSize_=be32(mdb+20);allocationStart_=uint64_t(be16(mdb+28))*512;
    freeBlocks_=be16(mdb+34);
    size_t mapBytes=(count*12+7)/8;
    uint64_t directoryStart=uint64_t(directory)*512,directoryEnd=directoryStart+uint64_t(directoryBlocks)*512;
    if(!count || count>4094 || !allocationSize_ || allocationSize_%512 || freeBlocks_>count ||
       !nameLength || nameLength>27 || !directoryBlocks || directoryStart<1024+64+mapBytes ||
       directoryEnd>length_ || allocationStart_<1024+64+mapBytes ||
       allocationStart_>length_ || uint64_t(count)*allocationSize_>length_-allocationStart_)
        return Fail("Invalid MFS volume layout.");
    volumeFlags_=uint16_t(be16(mdb+10));nextId_=be32(mdb+30);directoryStart_=directory;directoryBlocks_=directoryBlocks;
    name_.assign(reinterpret_cast<char*>(mdb+37),nameLength);
    if(name_.find('\0')!=std::string::npos || name_.find(':')!=std::string::npos) return Fail("Invalid volume name.");
    std::vector<BYTE> packed(mapBytes);if(!Read(1088,packed.data(),packed.size())) return false;
    map_.resize(count);
    for(unsigned i=0;i<count;++i) {
        unsigned offset=i*3/2;
        map_[i]=uint16_t(i%2?((packed[offset]&15)<<8)|packed[offset+1]:(packed[offset]<<4)|(packed[offset+1]>>4));
        // Directory sectors can reside in the allocation area, but must then
        // be marked with the directory-reserved value, never exposed as data.
        uint64_t start=allocationStart_+uint64_t(i)*allocationSize_,end=start+allocationSize_;
        if(start<directoryEnd && end>directoryStart && map_[i]!=0xfff) return Fail("Directory overlaps an unreserved allocation block.");
    }
    std::vector<bool> claimed(count,false);std::set<uint32_t> ids;
    for(unsigned i=0;i<count;++i){uint64_t start=allocationStart_+uint64_t(i)*allocationSize_;if(start<directoryEnd&&start+allocationSize_>directoryStart)claimed[i]=true;}
    for(unsigned sector=0;sector<directoryBlocks;++sector) {
        BYTE bytes[512];if(!Read(directoryStart+uint64_t(sector)*512,bytes,sizeof(bytes))) return false;
        for(unsigned offset=0;offset<512;) {
            if(bytes[offset]==0) break; // End of this sector, not the entire directory.
            if(512-offset<52) return Fail("Truncated directory entry.");
            BYTE* p=bytes+offset;unsigned n=p[50],size=(51+n+1)&~1;
            if(!n || size>512-offset) return Fail("Directory name crosses a sector boundary.");
            if(!(p[0]&0x80) || (p[0]&0x3e)) return Fail("Invalid directory flags.");
            MfsEntry entry;entry.flags=p[0];entry.version=p[1];entry.id=be32(p+18);
            entry.name.assign(reinterpret_cast<char*>(p+51),n);memcpy(entry.finderInfo,p+2,16);
            entry.created=be32(p+42);entry.modified=be32(p+46);entry.datesValid=true;
            if(!entry.id || !ids.insert(entry.id).second || entry.name.find('\0')!=std::string::npos ||
               entry.name.find(':')!=std::string::npos) return Fail("Invalid file ID or name.");
            for(unsigned fork=0;fork<2;++fork) {
                unsigned f=22+10*fork;
                entry.forks[fork].length=be32(p+f+2);entry.forks[fork].physicalLength=be32(p+f+6);
                if(!Chain(entry.forks[fork],uint16_t(be16(p+f)),claimed)) {entries_.clear();return false;}
            }
            entries_.push_back(entry);offset+=size;
        }
    }
    if(entries_.size()!=fileCount) {entries_.clear();return Fail("Directory file count does not match the volume header.");}
    AuditWrites(claimed);return true;
}
bool MfsImage::ReadFork(size_t entry,unsigned fork,uint64_t offset,void* bytes,size_t requested,size_t& read) {
    read=0;
    if(!source_ || !error_.empty() || entry>=entries_.size() || fork>1) return Fail("Invalid MFS fork selection.");
    const MfsFork& f=entries_[entry].forks[fork];if(offset>=f.length) return true;
    size_t length=static_cast<size_t>((std::min)(uint64_t(requested),uint64_t(f.length)-offset));
    while(read<length) {
        size_t block=static_cast<size_t>(offset/allocationSize_),inside=static_cast<size_t>(offset%allocationSize_);
        size_t chunk=(std::min)(length-read,size_t(allocationSize_)-inside);
        if(block>=f.blocks.size() || !Read(allocationStart_+uint64_t(f.blocks[block]-2)*allocationSize_+inside,static_cast<BYTE*>(bytes)+read,chunk)) return false;
        offset+=chunk;read+=chunk;
    }
    return true;
}
std::wstring MfsDisplayName(const std::string& name) {
    if(name.empty()) return {};
    int size=MultiByteToWideChar(10000,0,name.data(),int(name.size()),NULL,0);
    if(size<=0) return L"(name conversion failed)";
    std::wstring result(size,L'\0');MultiByteToWideChar(10000,0,name.data(),int(name.size()),&result[0],size);return result;
}

bool MfsExportFile(MfsImage& image,size_t index,const wchar_t* path,bool appleSingle,std::string& error) {
    if(index>=image.Entries().size() || !image.Error().empty()) {error="Invalid file selection.";return false;}
    const MfsEntry& entry=image.Entries()[index];
    HANDLE out=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(out==INVALID_HANDLE_VALUE) {error="Could not create the export. Choose a new filename; existing files are not overwritten.";return false;}
    bool ok=true;
    auto write=[&](const void* data,size_t length) {DWORD written;if(length && (!WriteFile(out,data,DWORD(length),&written,NULL)||written!=length)) ok=false;};
    if(appleSingle) {
        // AppleSingle v2: data, resource, original MacRoman name, Finder info,
        // and file dates. The extended Finder information is absent in MFS.
        BYTE header[26+5*12]={};put32(header,0x00051600);put32(header+4,0x00020000);put16(header+24,5);
        uint32_t ids[]={1,2,3,9,8},lengths[]={entry.forks[0].length,entry.forks[1].length,uint32_t(entry.name.size()),32,16};
        uint64_t offset=sizeof(header);
        for(unsigned i=0;i<5;++i) {
            if(offset+lengths[i]>0xffffffffULL) ok=false;
            put32(header+26+12*i,ids[i]);put32(header+30+12*i,uint32_t(offset));put32(header+34+12*i,lengths[i]);offset+=lengths[i];
        }
        if(ok) write(header,sizeof(header));
    }
    std::vector<BYTE> buffer(64*1024);
    for(unsigned fork=0;fork<(appleSingle?2u:1u) && ok;++fork) {
        uint64_t offset=0;
        while(offset<entry.forks[fork].length && ok) {
            size_t read=0;
            if(!image.ReadFork(index,fork,offset,buffer.data(),buffer.size(),read) || !read) {ok=false;break;}
            write(buffer.data(),read);offset+=read;
        }
    }
    if(appleSingle && ok) {
        write(entry.name.data(),entry.name.size());BYTE finder[32]={},dates[16];memcpy(finder,entry.finderInfo,16);write(finder,sizeof(finder));
        // AppleSingle dates use seconds relative to 2000-01-01; MFS uses 1904.
        // Out-of-range/unknown dates use Apple's reserved unknown value.
        auto date=[](uint32_t v)->uint32_t {int64_t seconds=int64_t(v)-3029529600LL;return v && seconds>INT32_MIN && seconds<=INT32_MAX?uint32_t(seconds):0x80000000u;};
        put32(dates,date(entry.created));put32(dates+4,date(entry.modified));put32(dates+8,0x80000000u);put32(dates+12,0x80000000u);write(dates,sizeof(dates));
    }
    if(ok && !FlushFileBuffers(out)) ok=false;
    CloseHandle(out);
    if(!ok) {DeleteFileW(path);error="Export failed; the incomplete output was removed.";return false;}
    return true;
}
