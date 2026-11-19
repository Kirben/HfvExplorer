// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include <winioctl.h>
namespace Removable {
struct Device {std::wstring path,label;DWORD number=MAXDWORD;STORAGE_BUS_TYPE bus=BusTypeUnknown;bool removable=false,hotplug=false;};
struct Geometry {uint64_t bytes=0;DWORD sector=512;bool floppy=false;};
// There is intentionally no write, format or arbitrary device-control method.
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
};
std::shared_ptr<Api> NativeApi();
bool Eligible(const Device&);
class Disk final:public HfsPlus::Source {
 struct Impl;std::unique_ptr<Impl> p_;
public:
 Disk(const Device&,std::shared_ptr<Api>);
 ~Disk();
 uint64_t Size()const override;
 bool Writable()const override{return false;}
 void Read(uint64_t,void*,size_t)override;
 void Write(uint64_t,const void*,size_t)override;
 void Flush()override;
 void CheckMedia()override;
 const Geometry& MediaGeometry()const;
 DWORD Generation()const;
};
struct Partition {uint64_t offset=0,length=0;unsigned index=0;};
std::vector<Partition> Partitions(HfsPlus::Source&,DWORD logicalSector);
struct Mounted {Device device;Partition partition;std::shared_ptr<MacBrowser::Volume> volume;std::wstring label;};
struct Discovery {std::vector<Mounted> volumes;std::vector<std::string> errors;std::vector<std::wstring> failedDevices;};
std::vector<Mounted> Open(const Device&,std::shared_ptr<Api> api={});
Discovery Scan(Storage::Job&,std::shared_ptr<Api> api={});
std::shared_ptr<MacBrowser::Volume> Reopen(const std::wstring&,uint64_t offset);
std::vector<Mounted> FromDriveRoot(const std::wstring&,Storage::Job* job=nullptr);
#ifdef HFV_TESTING
void SetTestApi(std::shared_ptr<Api>);
#endif
}
