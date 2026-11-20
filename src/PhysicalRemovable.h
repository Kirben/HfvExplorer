// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include <winioctl.h>
namespace Removable {
struct Device {std::wstring path,label;DWORD number=MAXDWORD;STORAGE_BUS_TYPE bus=BusTypeUnknown;bool removable=false,hotplug=false;int floppyDrive=-1;};
struct Geometry {uint64_t bytes=0;DWORD sector=512;bool floppy=false;};
struct Partition {
 uint64_t offset=0,length=0;unsigned index=0;bool readOnly=false;
 bool operator==(const Partition& value)const {
  return offset==value.offset&&length==value.length&&index==value.index&&readOnly==value.readOnly;
 }
};
class WriteAccess {
public:
 virtual ~WriteAccess()=default;
 virtual HANDLE Handle()const=0;
 virtual bool Write(uint64_t,DWORD,const void*,DWORD&)=0;
 virtual bool Flush()=0;
};
class Api {
public:
 virtual ~Api()=default;
 virtual std::vector<Device> Enumerate()=0;
 virtual bool FromRoot(const std::wstring&,Device&)=0;
 virtual bool WindowsReadableRoot(const std::wstring&){return false;}
 virtual HANDLE Open(const Device&,DWORD access,DWORD sharing,DWORD flags)=0;
 virtual bool GetGeometry(HANDLE,Geometry&)=0;
 virtual bool Verify(HANDLE,DWORD& generation)=0;
 virtual bool Read(HANDLE,uint64_t offset,DWORD bytes,void*,DWORD& returned)=0;
 virtual void Close(HANDLE)=0;
 virtual bool CanWrite(const Device&)const{return false;}
 virtual std::unique_ptr<WriteAccess> LockForWrite(const Device&,const Partition&){
  SetLastError(ERROR_WRITE_PROTECT);
  return {};
 }
};
std::shared_ptr<Api> NativeApi();
bool Eligible(const Device&);
class Disk final:public HfsPlus::Source {
 struct Impl;std::unique_ptr<Impl> p_;
public:
 Disk(const Device&,std::shared_ptr<Api>,const Partition* writePartition=nullptr);
 void DropCache();
 bool WriteFenced()const;
 bool WasWritten()const;
 ~Disk();
 uint64_t Size()const override;
 bool Writable()const override;
 void Read(uint64_t,void*,size_t)override;
 void Write(uint64_t,const void*,size_t)override;
 void Flush()override;
 void CheckMedia()override;
 const Geometry& MediaGeometry()const;
 DWORD Generation()const;
 bool MediaGeneration(uint32_t& generation)const override{generation=Generation();return true;}
};
std::vector<Partition> Partitions(HfsPlus::Source&,DWORD logicalSector);
struct Mounted {Device device;Partition partition;std::shared_ptr<MacBrowser::Volume> volume;std::wstring label;};
struct Discovery {std::vector<Mounted> volumes;std::vector<std::string> errors;std::vector<std::wstring> failedDevices;};
std::vector<Mounted> Open(const Device&,std::shared_ptr<Api> api={});
Discovery Scan(Storage::Job&,std::shared_ptr<Api> api={});
std::shared_ptr<MacBrowser::Volume> Reopen(const std::wstring&,uint64_t offset);
std::vector<Mounted> FromDriveRoot(const std::wstring&,Storage::Job* job=nullptr,bool floppyOnly=false);
#ifdef HFV_TESTING
void SetTestApi(std::shared_ptr<Api>);
bool HasTestApi();
#endif
}
