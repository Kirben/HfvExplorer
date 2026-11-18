// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "PhysicalRemovable.h"
#include "StorageHandoff.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include "DeviceWait.h"
#include "BrowserPresentation.h"
#include <setupapi.h>
#include <mutex>
#include <list>
#include <cwctype>
namespace Removable {
namespace {
using namespace HfsPlus;
void Check(bool ok,const char* message){if(!ok)throw Error(message);}
std::string Utf8(const std::wstring& s){return BrowserPresentation::Utf8(MacBrowser::U16(s));}
[[noreturn]]void IoError(const char* stage,DWORD error){if(!error)error=ERROR_IO_DEVICE;throw Error(std::string(stage)+" (Windows error "+std::to_string(error)+"). "+(error==ERROR_ACCESS_DENIED?"Windows may require running HFVExplorer as administrator for raw disk reads.":error==ERROR_MEDIA_CHANGED?"The media changed; refresh the drive before continuing.":"Refresh or reconnect the removable drive before trying again."));}
bool Root(const std::wstring& path){return path.size()==3&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/')&&towupper(path[0])>=L'A'&&towupper(path[0])<=L'Z';}
class WindowsApi final:public Api {
 bool Describe(HANDLE handle,Device& d){
  STORAGE_PROPERTY_QUERY query{};query.PropertyId=StorageDeviceProperty;query.QueryType=PropertyStandardQuery;STORAGE_DESCRIPTOR_HEADER header{};DWORD got=0;
  if(!HfvMediaControl(handle,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),&header,sizeof(header),&got,nullptr)||got<sizeof(header)||header.Size<sizeof(STORAGE_DEVICE_DESCRIPTOR)||header.Size>65536)return false;
  Bytes bytes(header.Size);if(!HfvMediaControl(handle,IOCTL_STORAGE_QUERY_PROPERTY,&query,sizeof(query),bytes.data(),DWORD(bytes.size()),&got,nullptr)||got<sizeof(STORAGE_DEVICE_DESCRIPTOR))return false;auto desc=reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(bytes.data());if(desc->DeviceType!=0)return false;
  d.bus=desc->BusType;d.removable=desc->RemovableMedia!=FALSE;
  STORAGE_HOTPLUG_INFO hot{};if(HfvMediaControl(handle,IOCTL_STORAGE_GET_HOTPLUG_INFO,nullptr,0,&hot,sizeof(hot),&got,nullptr)&&got>=sizeof(hot))d.hotplug=hot.DeviceHotplug!=FALSE;
  auto text=[&](DWORD offset){std::wstring out;if(offset<bytes.size()&&offset){for(size_t i=offset;i<bytes.size()&&bytes[i];++i)out+=wchar_t(bytes[i]);while(!out.empty()&&out.back()==L' ')out.pop_back();}return out;};d.label=text(desc->VendorIdOffset);auto product=text(desc->ProductIdOffset);if(!product.empty()){if(!d.label.empty())d.label+=L" ";d.label+=product;}if(d.label.empty())d.label=L"Removable disk";
  STORAGE_DEVICE_NUMBER number{};if(HfvMediaControl(handle,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),&got,nullptr)&&got>=sizeof(number)&&number.DeviceType==FILE_DEVICE_DISK)d.number=number.DeviceNumber;
  return Eligible(d);
 }
 HANDLE MetadataOpen(const std::wstring& path){return HfvDeviceWait::invoke([&]{return CreateFileW(path.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);},INVALID_HANDLE_VALUE);}
public:
 std::vector<Device> Enumerate()override{
  std::vector<Device> result;
#ifndef HFV_TESTING
  static const GUID diskInterface={0x53f56307,0xb6bf,0x11d0,{0x94,0xf2,0x00,0xa0,0xc9,0x1e,0xfb,0x8b}};
  auto set=SetupDiGetClassDevsW(&diskInterface,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);if(set==INVALID_HANDLE_VALUE)IoError("Cannot enumerate removable disks",GetLastError());struct Done{HDEVINFO set;~Done(){SetupDiDestroyDeviceInfoList(set);}}done{set};
  for(DWORD index=0;;++index){SP_DEVICE_INTERFACE_DATA item{};item.cbSize=sizeof(item);if(!SetupDiEnumDeviceInterfaces(set,nullptr,&diskInterface,index,&item)){if(GetLastError()==ERROR_NO_MORE_ITEMS)break;continue;}DWORD required=0;SetupDiGetDeviceInterfaceDetailW(set,&item,nullptr,0,&required,nullptr);if(required<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)||required>65536)continue;Bytes bytes(required);auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(bytes.data());detail->cbSize=sizeof(*detail);if(!SetupDiGetDeviceInterfaceDetailW(set,&item,detail,required,nullptr,nullptr))continue;Device device;device.path=detail->DevicePath;auto h=MetadataOpen(device.path);if(h==INVALID_HANDLE_VALUE)continue;bool eligible=Describe(h,device);Close(h);if(eligible)result.push_back(std::move(device));}
#endif
  return result;
 }
 bool FromRoot(const std::wstring& path,Device& device)override{
#ifdef HFV_TESTING
  (void)path;(void)device;return false;
#else
  if(!Root(path))return false;std::wstring raw=L"\\\\.\\"+path.substr(0,2);auto h=MetadataOpen(raw);if(h==INVALID_HANDLE_VALUE)return false;Device d;d.path=raw;bool eligible=Describe(h,d);Geometry geometry;bool floppy=GetGeometry(h,geometry)&&geometry.floppy;Close(h);
  if(floppy&&GetDriveTypeW(path.c_str())==DRIVE_REMOVABLE){d.removable=true;d.number=MAXDWORD;d.label=path.substr(0,2)+L" floppy";device=std::move(d);return true;}
  if(!eligible||d.number==MAXDWORD)return false;for(auto& candidate:Enumerate())if(candidate.number==d.number){device=std::move(candidate);return true;}return false;
#endif
 }
 bool WindowsReadableRoot(const std::wstring& root)override{
#ifdef HFV_TESTING
  (void)root;return false;
#else
  wchar_t filesystem[64]{};return GetVolumeInformationW(root.c_str(),nullptr,0,nullptr,nullptr,nullptr,filesystem,ARRAYSIZE(filesystem))&&*filesystem;
#endif
 }
 HANDLE Open(const Device& d,DWORD access,DWORD share,DWORD flags)override{
#ifdef HFV_TESTING
  (void)d;(void)access;(void)share;(void)flags;SetLastError(ERROR_NOT_SUPPORTED);return INVALID_HANDLE_VALUE;
#else
  if(access!=GENERIC_READ){SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}return HfvDeviceWait::invoke([&]{return CreateFileW(d.path.c_str(),access,share,nullptr,OPEN_EXISTING,flags,nullptr);},INVALID_HANDLE_VALUE);
#endif
 }
 bool GetGeometry(HANDLE h,Geometry& out)override{
  DISK_GEOMETRY geo{};DWORD got=0;if(!HfvMediaControl(h,IOCTL_DISK_GET_DRIVE_GEOMETRY,nullptr,0,&geo,sizeof(geo),&got,nullptr)||got<sizeof(geo))return false;out.sector=geo.BytesPerSector;out.floppy=geo.MediaType==F3_720_512||geo.MediaType==F3_1Pt44_512||geo.MediaType==F3_2Pt88_512;
  GET_LENGTH_INFORMATION length{};if(HfvMediaControl(h,IOCTL_DISK_GET_LENGTH_INFO,nullptr,0,&length,sizeof(length),&got,nullptr)&&got>=sizeof(length)&&length.Length.QuadPart>0)out.bytes=uint64_t(length.Length.QuadPart);
  else if(out.floppy&&geo.Cylinders.QuadPart==80&&geo.TracksPerCylinder==2&&geo.SectorsPerTrack<=36)out.bytes=uint64_t(geo.Cylinders.QuadPart)*geo.TracksPerCylinder*geo.SectorsPerTrack*geo.BytesPerSector;else return false;return true;
 }
 bool Verify(HANDLE h,DWORD& generation)override{DWORD got=0;if(!HfvMediaControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&generation,sizeof(generation),&got,nullptr))return false;if(got<sizeof(generation)){SetLastError(ERROR_NOT_SUPPORTED);return false;}return true;}
 bool Read(HANDLE h,uint64_t offset,DWORD bytes,void* p,DWORD& got)override{LARGE_INTEGER at;at.QuadPart=LONGLONG(offset);return HfvMediaSeek(h,at,nullptr,FILE_BEGIN)&&HfvMediaRead(h,p,bytes,&got,nullptr);}
 void Close(HANDLE h)override{CloseHandle(h);}
};
#ifdef HFV_TESTING
std::mutex testMutex;std::shared_ptr<Api> testApi;
#endif
class Slice final:public Source {
 std::shared_ptr<Source> source_;uint64_t offset_,length_;
public:Slice(std::shared_ptr<Source> source,uint64_t offset,uint64_t length):source_(std::move(source)),offset_(offset),length_(length){Check(HfsPlus::Disk::Range(offset,length,source_->Size()),"Partition lies outside removable disk");}
 uint64_t Size()const override{return length_;}bool Writable()const override{return false;}
 void Read(uint64_t at,void* p,size_t n)override{Check(HfsPlus::Disk::Range(at,n,length_),"Read outside removable partition");source_->Read(offset_+at,p,n);}
 void Write(uint64_t,const void*,size_t)override{throw Error("Physical removable disks are read-only");}void Flush()override{throw Error("Physical removable disks are read-only");}void CheckMedia()override{source_->CheckMedia();}
};
class MfsMedia final:public MfsSource{std::shared_ptr<Source> source_;public:explicit MfsMedia(std::shared_ptr<Source> s):source_(std::move(s)){}uint64_t Length()const override{return source_->Size();}bool ReadAt(uint64_t at,void* p,size_t n)override{source_->Read(at,p,n);return true;}};
class SharedSource final:public Source{std::shared_ptr<Source> source_;public:explicit SharedSource(std::shared_ptr<Source> s):source_(std::move(s)){}uint64_t Size()const override{return source_->Size();}bool Writable()const override{return false;}void Read(uint64_t at,void* p,size_t n)override{source_->Read(at,p,n);}void Write(uint64_t,const void*,size_t)override{throw Error("Physical removable disks are read-only");}void Flush()override{throw Error("Physical removable disks are read-only");}void CheckMedia()override{source_->CheckMedia();}};
class MediaVolume final:public MacBrowser::Volume {
 MacBrowser::WorkerOrigin workerOrigin_;
 std::shared_ptr<MacBrowser::Volume> backend_;std::shared_ptr<Source> source_;std::string key_,kind_,description_;std::u16string name_;uint64_t free_;bool sensitive_,attributes_;MacBrowser::Capabilities capabilities_;std::recursive_mutex mutex_;
 template<class F>auto Call(F work)->decltype(work()){VolumeCoord::BackendRead lease(key_);std::lock_guard<std::recursive_mutex> lock(mutex_);Check(bool(backend_),"Removable disk was released; refresh it");source_->CheckMedia();try{auto value=work();source_->CheckMedia();return value;}catch(...){auto failure=std::current_exception();source_->CheckMedia();std::rethrow_exception(failure);}}
public:MediaVolume(std::shared_ptr<MacBrowser::Volume> v,std::shared_ptr<Source> s,std::string key,MacBrowser::WorkerOrigin origin):workerOrigin_(std::move(origin)),backend_(std::move(v)),source_(std::move(s)),key_(std::move(key)){name_=backend_->Stat(2).name;kind_=backend_->Kind();description_=backend_->Description()+", physical removable disk (read-only)";free_=backend_->FreeBytes();sensitive_=backend_->CaseSensitive();attributes_=backend_->SupportsAttributes();capabilities_=backend_->Supports();capabilities_.create=capabilities_.rename=capabilities_.remove=false;}
 MacBrowser::WorkerOrigin WorkerDescription()const override{return workerOrigin_;}
 std::string ResourceKey()const override{return key_;}std::u16string Name()const override{return name_;}const char* Kind()const override{return kind_.c_str();}std::string Description()const override{return description_;}bool Writable()const override{return false;}bool CaseSensitive()const override{return sensitive_;}uint64_t FreeBytes()const override{return free_;}
 MacBrowser::Capabilities Supports()const override{return capabilities_;}bool SupportsAttributes()const override{return attributes_;}
 std::vector<MacBrowser::Entry> List(uint32_t id)override{return Call([&]{auto entries=backend_->List(id);for(auto& e:entries)e.identity=key_+":"+e.identity;return entries;});}MacBrowser::Entry Stat(uint32_t id)override{return Call([&]{auto e=backend_->Stat(id);e.identity=key_+":"+e.identity;return e;});}
 HfsPlus::Attributes GetAttributes(uint32_t id)override{return Call([&]{return backend_->GetAttributes(id);});}
 void Read(uint32_t id,unsigned f,uint64_t at,void* p,size_t n)override{Call([&]{backend_->Read(id,f,at,p,n);return true;});}
 uint32_t MakeDirectory(uint32_t,const std::u16string&)override{throw Error("Physical removable disks are read-only");}uint32_t Put(uint32_t,const MacBrowser::Entry&,MacBrowser::Reader,bool)override{throw Error("Physical removable disks are read-only");}void Rename(uint32_t,uint32_t,const std::u16string&)override{throw Error("Physical removable disks are read-only");}void Remove(uint32_t)override{throw Error("Physical removable disks are read-only");}
 void Reload()override{Call([&]{backend_->Reload();return true;});}void ReleaseMedia()override{std::lock_guard<std::recursive_mutex> lock(mutex_);backend_.reset();source_.reset();}
};
std::string Key(const Device& d,const Partition& p){auto path=d.path;for(auto& c:path)c=wchar_t(towupper(c));return "removable:"+Utf8(path)+"@"+std::to_string(p.offset)+"+"+std::to_string(p.length);}
}
bool Eligible(const Device& d){return d.removable||d.hotplug||d.bus==BusTypeUsb||d.bus==BusType1394||d.bus==BusTypeSd||d.bus==BusTypeMmc;}
std::shared_ptr<Api> NativeApi(){
#ifdef HFV_TESTING
 {std::lock_guard<std::mutex> lock(testMutex);if(testApi)return testApi;}
#endif
 static auto native=std::make_shared<WindowsApi>();return native;
}
#ifdef HFV_TESTING
void SetTestApi(std::shared_ptr<Api> api){std::lock_guard<std::mutex> lock(testMutex);testApi=std::move(api);}
#endif
struct Disk::Impl{std::shared_ptr<Api> api;HANDLE handle=INVALID_HANDLE_VALUE;Geometry geometry;DWORD generation=0,error=0;BYTE* buffer=nullptr;std::recursive_mutex mutex;std::list<uint64_t> ages;struct Page{Bytes bytes;std::list<uint64_t>::iterator age;};std::map<uint64_t,Page> pages;
 ~Impl(){if(handle!=INVALID_HANDLE_VALUE)api->Close(handle);if(buffer)VirtualFree(buffer,0,MEM_RELEASE);}
 [[noreturn]]void Fail(const char* stage,DWORD e){error=e?e:ERROR_IO_DEVICE;pages.clear();ages.clear();IoError(stage,error);}
 void Verify(){if(error)IoError("Removable disk is no longer valid",error);DWORD now=0;if(!api->Verify(handle,now))Fail("Cannot verify removable media",GetLastError());if(now!=generation)Fail("Removable media changed",ERROR_MEDIA_CHANGED);}
};
Disk::Disk(const Device& d,std::shared_ptr<Api> api):p_(new Impl){Check(Eligible(d),"This device is not external/removable");p_->api=std::move(api);p_->handle=p_->api->Open(d,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_FLAG_RANDOM_ACCESS);if(p_->handle==INVALID_HANDLE_VALUE)IoError("Cannot open removable disk for reading",GetLastError());if(!p_->api->GetGeometry(p_->handle,p_->geometry))IoError("Cannot determine removable disk geometry",GetLastError());auto& g=p_->geometry;Check(g.bytes>=2048&&g.bytes<=INT64_MAX&&g.sector>=512&&g.sector<=4096&&(g.sector&(g.sector-1))==0&&g.bytes%g.sector==0,"Unsupported removable disk geometry");if(!p_->api->Verify(p_->handle,p_->generation))IoError("Cannot establish removable media identity",GetLastError());p_->buffer=static_cast<BYTE*>(VirtualAlloc(nullptr,65536,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Check(p_->buffer!=nullptr,"Cannot allocate removable read buffer");}
Disk::~Disk()=default;uint64_t Disk::Size()const{return p_->geometry.bytes;}const Geometry& Disk::MediaGeometry()const{return p_->geometry;}DWORD Disk::Generation()const{return p_->generation;}
void Disk::CheckMedia(){std::lock_guard<std::recursive_mutex> lock(p_->mutex);p_->Verify();}
void Disk::Write(uint64_t,const void*,size_t){throw Error("Physical removable disks are read-only");}void Disk::Flush(){throw Error("Physical removable disks are read-only");}
void Disk::Read(uint64_t at,void* output,size_t size){std::lock_guard<std::recursive_mutex> lock(p_->mutex);p_->Verify();Check(Disk::Size()>=at&&size<=Disk::Size()-at&&(!size||output),"Read outside removable disk");auto out=static_cast<BYTE*>(output);while(size){uint64_t page=at/65536*65536;auto found=p_->pages.find(page);if(found==p_->pages.end()){DWORD bytes=DWORD((std::min)(uint64_t(65536),Size()-page)),got=0;if(!p_->api->Read(p_->handle,page,bytes,p_->buffer,got))p_->Fail("Removable disk read failed",GetLastError());if(got!=bytes)p_->Fail("Short removable disk read",ERROR_READ_FAULT);p_->Verify();if(p_->pages.size()>=64){p_->pages.erase(p_->ages.back());p_->ages.pop_back();}p_->ages.push_front(page);Impl::Page value;value.bytes.assign(p_->buffer,p_->buffer+bytes);value.age=p_->ages.begin();found=p_->pages.emplace(page,std::move(value)).first;}else{p_->ages.splice(p_->ages.begin(),p_->ages,found->second.age);found->second.age=p_->ages.begin();}auto skip=size_t(at-page),n=(std::min)(size,found->second.bytes.size()-skip);memcpy(out,found->second.bytes.data()+skip,n);out+=n;at+=n;size-=n;}p_->Verify();}
std::vector<Mounted> Open(const Device& device,std::shared_ptr<Api> api){if(!api)api=NativeApi();auto disk=std::make_shared<Disk>(device,api);std::vector<Mounted> result;for(auto partition:Partitions(*disk,disk->MediaGeometry().sector)){auto slice=std::make_shared<Slice>(disk,partition.offset,partition.length);if(slice->Size()<2048)continue;auto signature=HfsPlus::Disk::Read(*slice,1024,512);auto key=Key(device,partition);std::shared_ptr<MacBrowser::Volume> backend;
  if(Volume::Recognizes(*slice))backend=MacBrowser::FromHfsPlus(std::unique_ptr<Source>(new SharedSource(slice)),false,key);
  else if(HfsPlus::Disk::U16(signature.data())==0x4244){if(disk->MediaGeometry().floppy)continue;backend=MacBrowser::OpenClassicSource(slice,key);}
  else {std::unique_ptr<MfsSource> source(new MfsMedia(slice));if(MfsImage::Recognizes(*source))backend=MacBrowser::FromMfs(std::move(source),key);}
  if(!backend)continue;Mounted mounted;mounted.device=device;mounted.partition=partition;MacBrowser::WorkerOrigin origin;origin.kind=MacBrowser::WorkerOrigin::Removable;origin.path=device.path;origin.offset=partition.offset;origin.generationKnown=true;origin.generation=disk->Generation();mounted.volume=MacBrowser::TrackWorkerVolume(std::make_shared<MediaVolume>(std::move(backend),slice,key,origin));mounted.label=device.label;if(partition.index)mounted.label+=L", partition "+std::to_wstring(partition.index);result.push_back(std::move(mounted));}
 disk->CheckMedia();return result;
}
Discovery Scan(Storage::Job& job,std::shared_ptr<Api> api){if(!api)api=NativeApi();Discovery result;auto devices=api->Enumerate();std::set<std::wstring> seen;for(auto& device:devices){if(job.cancelled())throw Storage::Cancelled();if(!Eligible(device)||!seen.insert(device.path).second)continue;Storage::Progress progress;progress.phase="Checking Mac removable disks";progress.path=Utf8(device.label);progress.utf8=true;job.progress(progress);try{auto volumes=Open(device,api);result.volumes.insert(result.volumes.end(),volumes.begin(),volumes.end());}catch(const std::exception& e){result.errors.push_back(Utf8(device.label)+": "+e.what());}}return result;}
std::vector<Mounted> FromDriveRoot(const std::wstring& root){if(!Root(root))return {};auto api=NativeApi();if(api->WindowsReadableRoot(root))return {};Device device;if(!api->FromRoot(root,device))return {};return Open(device,api);}
std::shared_ptr<MacBrowser::Volume> Reopen(const std::wstring& path,uint64_t offset){auto api=NativeApi();for(auto& d:api->Enumerate())if(!_wcsicmp(d.path.c_str(),path.c_str()))for(auto& v:Open(d,api))if(v.partition.offset==offset)return v.volume;Device d;if(path.size()==6&&path.substr(0,4)==L"\\\\.\\"&&api->FromRoot(path.substr(4)+L"\\",d))for(auto& v:Open(d,api))if(v.partition.offset==offset)return v.volume;throw Error("The removable Mac partition is no longer present; scan removable disks again");}
}
