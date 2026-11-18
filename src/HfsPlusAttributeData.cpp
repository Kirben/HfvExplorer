// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include "HfsPlus.h"
#include "AttributeReadScope.h"
#include <algorithm>
#include <mutex>
#include <cstring>
namespace HfsPlus {
namespace {thread_local std::function<void()> checkpoint;}
AttributeReadScope::AttributeReadScope(std::function<void()> value):previous_(std::move(checkpoint)){checkpoint=std::move(value);}
AttributeReadScope::~AttributeReadScope(){checkpoint=std::move(previous_);}
void AttributeCheckpoint(){if(checkpoint)checkpoint();}
namespace {constexpr size_t chunk=65536;void Check(bool ok,const char* s){if(!ok)throw Error(s);}}
struct AttributeData::Storage {
 std::shared_ptr<AttributeData> base;std::vector<std::pair<uint64_t,AttributeData>> edits;
 std::vector<uint8_t> memory;HANDLE file=INVALID_HANDLE_VALUE;bool sparse=false;uint64_t length=0;mutable std::mutex mutex;
 ~Storage(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
 void Open(){wchar_t directory[MAX_PATH+1];auto n=GetTempPathW(MAX_PATH,directory);Check(n&&n<MAX_PATH,"Cannot find attribute staging directory");static LONG serial=0;
  for(unsigned i=0;i<1000;++i){wchar_t leaf[80];swprintf_s(leaf,L"HFV-attr-%08lX-%08lX-%08lX.tmp",GetCurrentProcessId(),GetTickCount(),DWORD(InterlockedIncrement(&serial)));auto path=std::wstring(directory)+leaf;
   file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE|FILE_FLAG_RANDOM_ACCESS,nullptr);if(file!=INVALID_HANDLE_VALUE){DWORD got=0;sparse=DeviceIoControl(file,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&got,nullptr)!=FALSE;return;}Check(GetLastError()==ERROR_FILE_EXISTS,"Cannot create attribute staging file");}
  throw Error("Cannot reserve attribute staging file");
 }
};
AttributeData::AttributeData():storage_(std::make_shared<Storage>()){}
AttributeData::AttributeData(const std::vector<uint8_t>& b):AttributeData(){storage_->memory=b;}
AttributeData::AttributeData(std::vector<uint8_t>&& b):AttributeData(){storage_->memory=std::move(b);}
uint64_t AttributeData::size()const{return FileBacked()?storage_->length:storage_->memory.size();}
bool AttributeData::FileBacked()const{return storage_->base||storage_->file!=INVALID_HANDLE_VALUE;}
const std::vector<uint8_t>& AttributeData::Small()const{Check(!FileBacked(),"Stream a file-backed attribute instead of requesting its entire buffer");return storage_->memory;}
std::vector<uint8_t>& AttributeData::Small(){Check(!FileBacked(),"Stream a file-backed attribute instead of requesting its entire buffer");if(!storage_.unique()){auto old=storage_;storage_=std::make_shared<Storage>();storage_->memory=old->memory;}return storage_->memory;}
AttributeData AttributeData::Snapshot(uint64_t length,Reader read,bool fileBacked){
 AttributeCheckpoint();Check(length<=INT64_MAX,"Attribute exceeds signed 64-bit storage addressing");AttributeData result;
 if(length<=chunk&&!fileBacked){result.storage_->memory.resize(size_t(length));if(length)read(0,result.storage_->memory.data(),size_t(length));return result;}
 result.storage_->Open();result.storage_->length=length;std::vector<uint8_t> buffer(chunk);
 for(uint64_t at=0;at<length;){AttributeCheckpoint();auto n=size_t((std::min)(uint64_t(chunk),length-at));read(at,buffer.data(),n);if(result.storage_->sparse&&[&]{static const uint8_t zero[chunk]{};return memcmp(buffer.data(),zero,n)==0;}()){LARGE_INTEGER skip;skip.QuadPart=LONGLONG(n);Check(SetFilePointerEx(result.storage_->file,skip,nullptr,FILE_CURRENT)!=FALSE,"Cannot seek attribute staging hole");}else{DWORD written=0;Check(WriteFile(result.storage_->file,buffer.data(),DWORD(n),&written,nullptr)&&written==n,"Attribute staging write failed (check temporary disk space)");}at+=n;}
 AttributeCheckpoint();Check(SetEndOfFile(result.storage_->file)!=FALSE,"Cannot finalize attribute staging length");return result;
}
void AttributeData::Read(uint64_t at,void* output,size_t n)const{AttributeCheckpoint();Check(at<=size()&&uint64_t(n)<=size()-at&&(!n||output),"Attribute read range overflow");if(!n)return;if(storage_->base){storage_->base->Read(at,output,n);for(const auto& edit:storage_->edits){auto first=(std::max)(at,edit.first),last=(std::min)(at+n,edit.first+edit.second.size());if(first<last)edit.second.Read(first-edit.first,static_cast<uint8_t*>(output)+size_t(first-at),size_t(last-first));}return;}if(!FileBacked()){memcpy(output,storage_->memory.data()+size_t(at),n);return;}
 std::lock_guard<std::mutex> lock(storage_->mutex);LARGE_INTEGER offset;offset.QuadPart=LONGLONG(at);Check(SetFilePointerEx(storage_->file,offset,nullptr,FILE_BEGIN)!=FALSE,"Attribute staging seek failed");auto p=static_cast<uint8_t*>(output);while(n){AttributeCheckpoint();auto part=DWORD((std::min)(n,chunk));DWORD got=0;Check(ReadFile(storage_->file,p,part,&got,nullptr)&&got==part,"Attribute staging read failed");p+=part;n-=part;}
}
bool AttributeData::operator==(const AttributeData& other)const{if(storage_==other.storage_)return true;if(size()!=other.size())return false;std::vector<uint8_t> a(chunk),b(chunk);for(uint64_t at=0;at<size();){AttributeCheckpoint();auto n=size_t((std::min)(uint64_t(chunk),size()-at));Read(at,a.data(),n);other.Read(at,b.data(),n);if(memcmp(a.data(),b.data(),n))return false;at+=n;}return true;}
AttributeData AttributeData::Patch(uint64_t at,const std::vector<uint8_t>& patch)const{AttributeCheckpoint();Check(at<=size()&&patch.size()<=size()-at,"Attribute patch range overflow");std::vector<uint8_t> previous(patch.size());Read(at,previous.data(),previous.size());if(previous==patch)return *this;AttributeData result;result.storage_->base=storage_->base?storage_->base:std::make_shared<AttributeData>(*this);result.storage_->length=size();result.storage_->edits=storage_->edits;
 auto value=Snapshot(patch.size(),[&](uint64_t at,void* p,size_t n){memcpy(p,patch.data()+size_t(at),n);},true);
 // A later edit at the same exact page replaces that snapshot; other overlapping
 // edits retain chronological order so the last staged bytes win.
 auto& edits=result.storage_->edits;edits.erase(std::remove_if(edits.begin(),edits.end(),[&](const std::pair<uint64_t,AttributeData>& e){return e.first==at&&e.second.size()==patch.size();}),edits.end());edits.emplace_back(at,std::move(value));return result;}
}
