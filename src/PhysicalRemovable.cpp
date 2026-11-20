// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "PhysicalRemovable.h"
#include "DiskPartitions.h"
#include "FloppyWriteLease.h"
#include "ApfsVolume.h"
#include "MediaDiscovery.h"
#include "StorageHandoff.h"
#include "HfsPlusDisk.h"
#include "VolumeCoordinator.h"
#include "DeviceWait.h"
#include "BrowserPresentation.h"
#include <setupapi.h>
#include <mutex>
#include <list>
#include <cwctype>
#include <cstddef>
#include <atomic>
#include <limits>
#include "StorageOutcome.h"
namespace Removable {
namespace {
using namespace HfsPlus;
void Check(bool ok,const char* message){if(!ok)throw Error(message);}
std::string Utf8(const std::wstring& s){return BrowserPresentation::Utf8(MacBrowser::U16(s));}
[[noreturn]]void IoError(const char* stage,DWORD error){if(!error)error=ERROR_IO_DEVICE;if(error==ERROR_MEDIA_CHANGED)throw Storage::MediaChanged("Removable media changed; refresh the drive");throw Error(std::string(stage)+" (Windows error "+std::to_string(error)+"). "+(error==ERROR_ACCESS_DENIED?"Windows may require running HFVExplorer as administrator for raw disk access.":error==ERROR_MEDIA_CHANGED?"The media changed; refresh the drive before continuing.":"Refresh or reconnect the removable drive before trying again."));}
bool Root(const std::wstring& path){return path.size()==3&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/')&&towupper(path[0])>=L'A'&&towupper(path[0])<=L'Z';}
class NativeWriteAccess final:public WriteAccess {
    FloppyWriteLease floppyLease_;
    HANDLE disk_=INVALID_HANDLE_VALUE;
    bool floppy_=false;
    std::vector<HANDLE> volumes_;
public:
    ~NativeWriteAccess(){
        if(disk_!=INVALID_HANDLE_VALUE)CloseHandle(disk_);
        // Closing the owning handles releases FSCTL_LOCK_VOLUME.
        for(const auto volume:volumes_)CloseHandle(volume);
    }
    HANDLE Handle()const override{return disk_;}
    void OwnDisk(HANDLE h){disk_=h;}
    bool ReserveFloppy(int drive){floppy_=true;return floppyLease_.Acquire(drive);}
    void OwnVolume(HANDLE h){volumes_.push_back(h);}
    bool Write(uint64_t offset,DWORD n,const void* data,DWORD& done)override{
        LARGE_INTEGER at;at.QuadPart=LONGLONG(offset);
        return HfvMediaSeek(disk_,at,nullptr,FILE_BEGIN)&&WriteFile(disk_,data,n,&done,nullptr);
    }
    bool Flush()override{
        if(FlushFileBuffers(disk_))return true;
        const auto error=GetLastError();
        // Write-through floppy devices still require direct readback below.
        return floppy_&&(error==ERROR_INVALID_FUNCTION||error==ERROR_NOT_SUPPORTED);
    }
};

#ifndef HFV_TESTING
struct VolumeHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~VolumeHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
bool Overlaps(HANDLE volume,const Device& disk,const Partition& part)
{
    const auto capacity=FIELD_OFFSET(VOLUME_DISK_EXTENTS,Extents)+1024*sizeof(DISK_EXTENT);
    std::vector<uint8_t> bytes(capacity);
    DWORD got=0;
    const auto extents=reinterpret_cast<const VOLUME_DISK_EXTENTS*>(bytes.data());
    if(!HfvMediaControl(volume,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,bytes.data(),
                        DWORD(bytes.size()),&got,nullptr)){
        STORAGE_DEVICE_NUMBER number{};
        if(HfvMediaControl(volume,IOCTL_STORAGE_GET_DEVICE_NUMBER,nullptr,0,&number,sizeof(number),&got,nullptr)&&
           got>=sizeof(number)&&number.DeviceType!=FILE_DEVICE_DISK)return false;
        IoError("Cannot determine the Windows volume extents before locking",GetLastError());
    }
    constexpr size_t base=offsetof(VOLUME_DISK_EXTENTS,Extents);
    Check(got>=base&&got<=bytes.size()&&extents->NumberOfDiskExtents<=(got-base)/sizeof(DISK_EXTENT),
          "Invalid Windows volume extent information");
    for(DWORD i=0;i<extents->NumberOfDiskExtents;++i){
        const auto& extent=extents->Extents[i];
        if(extent.DiskNumber!=disk.number)continue;
        const auto start=extent.StartingOffset.QuadPart,length=extent.ExtentLength.QuadPart;
        Check(start>=0&&length>0&&uint64_t(length)<=UINT64_MAX-uint64_t(start),
              "Invalid Windows partition extent");
        if(uint64_t(start)<part.offset+part.length&&part.offset<uint64_t(start)+uint64_t(length))return true;
    }
    return false;
}
#endif

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
  if(floppy&&GetDriveTypeW(path.c_str())==DRIVE_REMOVABLE){d.removable=true;d.number=MAXDWORD;d.floppyDrive=towupper(path[0])-L'A';d.label=path.substr(0,2)+L" floppy";device=std::move(d);return true;}
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
 bool CanWrite(const Device& device)const override{return Eligible(device)&&(device.number!=MAXDWORD||device.floppyDrive>=0);}
 std::unique_ptr<WriteAccess> LockForWrite(const Device& d,const Partition& part)override{
#ifdef HFV_TESTING
  (void)d;(void)part;SetLastError(ERROR_NOT_SUPPORTED);return {};
#else
  Check(CanWrite(d),"Windows cannot uniquely identify this removable disk for writing");
  auto lock=std::make_unique<NativeWriteAccess>();
  if(d.floppyDrive>=0){
      if(!lock->ReserveFloppy(d.floppyDrive))IoError("Cannot reserve the floppy for writing",GetLastError());
      auto handle=HfvDeviceWait::invoke([&]{
          return CreateFileW(d.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                             nullptr,OPEN_EXISTING,FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,nullptr);
      },INVALID_HANDLE_VALUE);
      if(handle==INVALID_HANDLE_VALUE)IoError("Cannot open the floppy for writing",GetLastError());
      lock->OwnDisk(handle);
      DWORD ignored=0,generation=0,after=0;
      if(!HfvMediaControl(handle,FSCTL_LOCK_VOLUME,nullptr,0,nullptr,0,&ignored,nullptr))
          IoError("Cannot lock the floppy volume",GetLastError());
      if(!Verify(handle,generation)||
         !HfvMediaControl(handle,FSCTL_DISMOUNT_VOLUME,nullptr,0,nullptr,0,&ignored,nullptr)||
         !Verify(handle,after))
          IoError("Cannot verify or dismount the floppy volume",GetLastError());
      if(generation!=after)IoError("Floppy media changed during locking",ERROR_MEDIA_CHANGED);
      return lock;
  }
  auto disk=HfvDeviceWait::invoke([&]{
      return CreateFileW(d.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                        FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,nullptr);
  },INVALID_HANDLE_VALUE);
  if(disk==INVALID_HANDLE_VALUE)IoError("Cannot obtain exclusive raw write access",GetLastError());
  lock->OwnDisk(disk);
  Device actual;
  Check(Describe(disk,actual)&&actual.number==d.number,"The selected removable disk was replaced");
  DWORD ignored=0;
  if(!HfvMediaControl(disk,IOCTL_DISK_IS_WRITABLE,nullptr,0,nullptr,0,&ignored,nullptr))
      IoError("Windows refused writes to the removable disk",GetLastError());
  wchar_t name[MAX_PATH];
  HANDLE scan=FindFirstVolumeW(name,ARRAYSIZE(name));
  if(scan==INVALID_HANDLE_VALUE)IoError("Cannot enumerate Windows volumes before locking the disk",GetLastError());
  struct FindGuard{HANDLE value;~FindGuard(){FindVolumeClose(value);}} done{scan};
  do{
      std::wstring root=name;
      if(GetDriveTypeW(root.c_str())==DRIVE_CDROM)continue;
      Check(!root.empty()&&root.back()==L'\\',"Invalid Windows volume name");
      root.pop_back();
      {
          VolumeHandle metadata{MetadataOpen(root)};
          if(metadata.value==INVALID_HANDLE_VALUE)
              IoError("Cannot inspect a Windows volume before locking the disk",GetLastError());
          if(!Overlaps(metadata.value,d,part))continue;
      }
      VolumeHandle volume{HfvDeviceWait::invoke([&]{
          return CreateFileW(root.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                            nullptr,OPEN_EXISTING,0,nullptr);
      },INVALID_HANDLE_VALUE)};
      if(volume.value==INVALID_HANDLE_VALUE)IoError("Cannot open the matching Windows volume for locking",GetLastError());
      Check(Overlaps(volume.value,d,part),"Windows volume changed before it could be locked");
      if(!HfvMediaControl(volume.value,FSCTL_LOCK_VOLUME,nullptr,0,nullptr,0,&ignored,nullptr))
          IoError("Cannot lock the matching Windows volume; close other programs using it",GetLastError());
      lock->OwnVolume(volume.value);
      volume.value=INVALID_HANDLE_VALUE;
  }while(FindNextVolumeW(scan,name,ARRAYSIZE(name)));
  if(GetLastError()!=ERROR_NO_MORE_FILES)IoError("Cannot finish enumerating Windows volumes",GetLastError());
  return lock;
#endif
 }
 bool Verify(HANDLE h,DWORD& generation)override{DWORD got=0;if(!HfvMediaControl(h,IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&generation,sizeof(generation),&got,nullptr))return false;if(got<sizeof(generation)){SetLastError(ERROR_NOT_SUPPORTED);return false;}return true;}
 bool Read(HANDLE h,uint64_t offset,DWORD bytes,void* p,DWORD& got)override{LARGE_INTEGER at;at.QuadPart=LONGLONG(offset);return HfvMediaSeek(h,at,nullptr,FILE_BEGIN)&&HfvMediaRead(h,p,bytes,&got,nullptr);}
 void Close(HANDLE h)override{CloseHandle(h);}
};
#ifdef HFV_TESTING
std::mutex testMutex;std::shared_ptr<Api> testApi;
#endif
using MacBrowser::Entry;
using MacBrowser::Reader;
class Slice final:public Source {
    std::shared_ptr<Disk> disk_;
    Partition part_;
    void Bound(uint64_t at,size_t n)const {
        Check(HfsPlus::Disk::Range(at,n,part_.length),"Access outside the removable partition");
    }
public:
    Slice(std::shared_ptr<Disk> disk,Partition part):disk_(std::move(disk)),part_(part){}
    uint64_t Size()const override{return part_.length;}
    bool Writable()const override{return disk_->Writable();}
    bool MediaGeneration(uint32_t& generation)const override{generation=disk_->Generation();return true;}
    void Read(uint64_t at,void* p,size_t n)override{Bound(at,n);disk_->Read(part_.offset+at,p,n);}
    void Write(uint64_t at,const void* p,size_t n)override{Bound(at,n);disk_->Write(part_.offset+at,p,n);}
    void Flush()override{disk_->Flush();}
    void CheckMedia()override{disk_->CheckMedia();}
};
class MfsMedia final:public MfsSource{
    std::unique_ptr<Slice> source_;
    bool disabled_=false;
public:
    explicit MfsMedia(std::unique_ptr<Slice> s):source_(std::move(s)){}
    uint64_t Length()const override{return source_->Size();}
    bool Writable()const override{return !disabled_&&source_->Writable();}
    void DisableWrites()override{disabled_=true;}
    bool ReadAt(uint64_t at,void* p,size_t n)override{source_->Read(at,p,n);return true;}
    bool WriteAt(uint64_t at,const void* p,size_t n)override{
        if(!Writable())return false;
        try{source_->Write(at,p,n);return true;}catch(...){return false;}
    }
    bool Flush()override{try{source_->Flush();return true;}catch(...){return false;}}
};
std::optional<DiskPartitions::Selection> SelectPartition(Slice& slice,bool floppy){
    using namespace DiskPartitions;
    if(slice.Size()<2048)return {};
    if(auto plus=FindHfsPlus(slice))return plus;
    const MacPartition part{0,slice.Size(),0,false};
    const auto legacy=LegacyFilesystem(slice,part,true);
    if(legacy==Filesystem::Classic)
        return floppy?std::nullopt:std::optional<Selection>(Selection{*legacy,part});
    if(!memcmp(HfsPlus::Disk::Read(slice,32,4).data(),"NXSB",4))
        return Selection{Filesystem::Apfs,part};
    if(legacy)return Selection{*legacy,part};
    return {};
}

std::shared_ptr<MacBrowser::Volume> OpenSelectedPartition(std::unique_ptr<Slice> slice,
        const DiskPartitions::Selection& selection,const std::string& key,
        const std::shared_ptr<MacBrowser::MfsCatalogIds>& ids){
    using DiskPartitions::Filesystem;
    const bool writable=slice->Writable();
    switch(selection.filesystem){
    case Filesystem::HfsPlus:
    case Filesystem::HfsX:
        return MacBrowser::FromHfsPlus(std::move(slice),writable,key);
    case Filesystem::Classic:
        return MacBrowser::OpenClassicSource(std::move(slice),key,writable,true);
    case Filesystem::Mfs:
        return MacBrowser::FromMfs(std::make_unique<MfsMedia>(std::move(slice)),key,ids);
    case Filesystem::Apfs: {
        std::unique_ptr<Source> apfsSource(std::move(slice));
        return Apfs::Open(apfsSource,key);
    }
    }
    throw Error("Invalid removable disk filesystem");
}
struct WriteFence {std::atomic<bool> tripped{false};};
std::shared_ptr<WriteFence> FenceFor(const std::string& device,DWORD generation){
    static std::mutex mutex;
    static std::map<std::pair<std::string,DWORD>,std::weak_ptr<WriteFence>> fences;
    std::lock_guard<std::mutex> guard(mutex);
    for(auto i=fences.begin();i!=fences.end();){
        if(i->second.expired())i=fences.erase(i);else ++i;
    }
    auto& item=fences[{device,generation}];
    auto fence=item.lock();
    if(!fence)item=fence=std::make_shared<WriteFence>();
    return fence;
}
class MediaVolume final:public MacBrowser::Volume,public MacBrowser::WorkerEndpoint {
    const Device device_;
    const Partition partition_;
    const DiskPartitions::Selection selection_;
    const std::shared_ptr<Api> api_;
    const std::string key_,diskKey_;
    std::shared_ptr<Disk> disk_;
    std::shared_ptr<MacBrowser::Volume> backend_;
    const std::shared_ptr<MacBrowser::MfsCatalogIds> ids_;
    const std::u16string name_;
    const std::string kind_,description_;
    const bool sensitive_,attributes_,stableCatalogIds_,journaled_,links_,compression_,metadata_;
    const MacBrowser::Capabilities capabilities_;
    const bool processLocal_;
    std::atomic<bool> released_{false};
    const std::shared_ptr<WriteFence> fence_;
    std::atomic<uint64_t> free_;
    uint64_t revision_;
    const DWORD generation_;
    bool active_=false;
    mutable std::recursive_mutex mutex_;

    std::shared_ptr<MacBrowser::Volume> ReopenPartition(const std::shared_ptr<Disk>& disk){
        auto slice=std::make_unique<Slice>(disk,partition_);
        if(SelectPartition(*slice,disk->MediaGeometry().floppy)!=selection_)
            throw Storage::MediaChanged("Removable filesystem or partition changed; refresh the drive");
        return OpenSelectedPartition(std::move(slice),selection_,key_,ids_);
    }

    void Refresh(bool force=false){
        Check(bool(disk_),"Removable disk was released; refresh it");
        disk_->CheckMedia();
        auto revision=VolumeCoord::Revision(key_);
        if(!active_&&(force||!backend_||revision_!=revision)){
            disk_->DropCache();
            auto reopened=ReopenPartition(disk_);
            if(!reopened||kind_!=reopened->Kind())
                throw Storage::MediaChanged("Removable partition changed; refresh the drive");
            backend_=std::move(reopened);
            free_=backend_->FreeBytes();
            revision_=revision;
        }
    }
    template<class F>auto Call(F work)const->decltype(work()){
        VolumeCoord::BackendRead lease(key_);
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        const_cast<MediaVolume*>(this)->Refresh();
        auto result=work();
        disk_->CheckMedia();
        return result;
    }
    template<class F>void Change(F work){WithMediaWrite(work);}
    static bool WriteCandidate(const Disk& disk,const Device& device,const Partition& part,const Api& api,
                               const std::string& kind){
        const auto& g=disk.MediaGeometry();
        const bool supportedFloppy=kind=="MFS"&&device.floppyDrive>=0&&part.index==0&&
                                   !part.offset&&part.length==g.bytes&&g.sector==512&&g.bytes==1474560;
        return (!g.floppy||supportedFloppy)&&!part.readOnly&&part.offset%g.sector==0&&
               part.length%g.sector==0&&api.CanWrite(device);
    }
    std::string Identity(const std::string& identity)const{
        return key_+":"+std::to_string(generation_)+":"+identity;
    }
    Entry DestinationEntry(Entry entry)const{
        auto& expected=entry.expectedDestinationIdentity;
        const auto prefix=Identity("");
        if(expected.compare(0,prefix.size(),prefix)==0)expected.erase(0,prefix.size());
        return entry;
    }
public:
    MediaVolume(const Device& device,Partition partition,DiskPartitions::Selection selection,std::shared_ptr<Api> api,
                std::shared_ptr<Disk> disk,std::shared_ptr<MacBrowser::Volume> backend,
                std::string key,std::shared_ptr<MacBrowser::MfsCatalogIds> ids)
        :device_(device),partition_(partition),selection_(selection),api_(std::move(api)),key_(std::move(key)),
         diskKey_(device.floppyDrive>=0?VolumeCoord::DeviceKey(device.floppyDrive):
                  device.number==MAXDWORD?"removable-disk:"+Utf8(device.path):
                  "removable-disk:"+std::to_string(device.number)),
         disk_(std::move(disk)),backend_(std::move(backend)),ids_(std::move(ids)),name_(backend_->Name()),
         kind_(backend_->Kind()),description_(backend_->Description()),sensitive_(backend_->CaseSensitive()),
         attributes_(backend_->SupportsAttributes()),stableCatalogIds_(MacBrowser::ExecutionFor(*backend_).stableCatalogIds),
         journaled_(backend_->Journaled()),links_(backend_->SupportsHardLinks()),compression_(backend_->SupportsCompression()),
         metadata_(backend_->SupportsBasicMetadata()),capabilities_(backend_->Supports()),
         processLocal_(kind_!="APFS"&&WriteCandidate(*disk_,device_,partition_,*api_,kind_)),
         fence_(FenceFor(diskKey_,disk_->Generation())),
         free_(backend_->FreeBytes()),revision_(VolumeCoord::Revision(key_)),generation_(disk_->Generation()){}

    void WithMediaWrite(const std::function<void()>& work)override{
        Check(Writable(),"Physical removable partition is read-only");
        VolumeCoord::OperationLease lease({{key_,VolumeCoord::Mode::Write},{diskKey_,VolumeCoord::Mode::Write}});
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        Check(Writable(),"Physical removable partition is read-only");
        Refresh();
        if(active_){work();return;}

        auto writer=std::make_shared<Disk>(device_,api_,&partition_);
        const auto& old=disk_->MediaGeometry();
        const auto& locked=writer->MediaGeometry();
        if(writer->Generation()!=generation_||old.sector!=locked.sector||old.bytes!=locked.bytes)
            throw Storage::MediaChanged("Removable media changed before writing; refresh the drive");
        const auto partitions=Partitions(*writer,locked.sector);
        if(std::find(partitions.begin(),partitions.end(),partition_)==partitions.end())
            throw Storage::MediaChanged("Removable partition changed before writing; refresh the drive");
        if(!old.floppy)disk_->CheckMedia();
        writer->CheckMedia();

        auto reader=disk_;
        auto readBackend=backend_;
        std::exception_ptr failure;
        try{
            auto writable=ReopenPartition(writer);
            Check(writable&&kind_==writable->Kind()&&writable->Writable(),
                  "The removable filesystem could not be opened for writing");
            disk_=writer;
            backend_=std::move(writable);
            active_=true;
            work();
        }catch(...){failure=std::current_exception();}
        if(active_){
            try{backend_->Close();}catch(...){if(!failure)failure=std::current_exception();}
            backend_=std::move(readBackend);
            disk_=std::move(reader);
            active_=false;
        }
        try{writer->CheckMedia();}catch(...){if(!failure)failure=std::current_exception();}
        if(writer->WriteFenced())fence_->tripped=true;
        const bool changed=writer->WasWritten();
        writer.reset();
        if(changed){
            backend_.reset();
            disk_->DropCache();
            VolumeCoord::Changed({key_});
            if(!failure)Refresh();
        }
        if(failure)std::rethrow_exception(failure);
    }

    MacBrowser::WorkerExecution Execution()const override{
        MacBrowser::WorkerExecution out;
        out.classicConversion=kind_=="HFS";
        out.stableCatalogIds=stableCatalogIds_;
        out.processLocal=processLocal_;
        return out;
    }
    MacBrowser::WorkerOrigin WorkerDescription()const override{
        MacBrowser::WorkerOrigin out;
        out.kind=MacBrowser::WorkerOrigin::Removable;
        out.path=device_.path;
        out.offset=partition_.offset;
        out.generationKnown=true;
        out.generation=generation_;
        return out;
    }
    std::string ResourceKey()const override{return key_;}
    std::u16string Name()const override{return name_;}
    const char* Kind()const override{return kind_.c_str();}
    std::string Description()const override{return description_+(Writable()?", physical removable disk":", physical removable disk (read-only)");}
    bool Writable()const override{return processLocal_&&!released_&&!fence_->tripped;}
    bool CaseSensitive()const override{return sensitive_;}
    bool Journaled()const override{return journaled_;}
    uint64_t FreeBytes()const override{return free_;}
    MacBrowser::Capabilities Supports()const override{
        auto result=capabilities_;
        result.create=result.rename=result.remove=Writable();
        return result;
    }
    bool SupportsAttributes()const override{return attributes_;}
    bool SupportsHardLinks()const override{return links_;}
    bool SupportsCompression()const override{return compression_;}
    bool SupportsBasicMetadata()const override{return metadata_;}
    std::vector<Entry> List(uint32_t id)override{return Call([&]{
        auto entries=backend_->List(id);
        for(auto& entry:entries)entry.identity=Identity(entry.identity);
        return entries;
    });}
    Entry Stat(uint32_t id)override{return Call([&]{auto entry=backend_->Stat(id);entry.identity=Identity(entry.identity);return entry;});}
    bool FindTransferChild(uint32_t parent,const Entry& entry,Entry& found)override{return Call([&]{
        if(!backend_->FindTransferChild(parent,entry,found))return false;
        found.identity=Identity(found.identity);
        return true;
    });}
    HfsPlus::Attributes GetAttributes(uint32_t id)override{return Call([&]{return backend_->GetAttributes(id);});}
    void SetAttributes(uint32_t id,const HfsPlus::Attributes& attrs)override{Change([&]{backend_->SetAttributes(id,attrs);});}
    bool GetClassicProperties(uint32_t id,hfsdirent& e)override{return Call([&]{return backend_->GetClassicProperties(id,e);});}
    void SetClassicProperties(uint32_t id,const hfsdirent& e)override{Change([&]{backend_->SetClassicProperties(id,e);});}
    void SetBasicMetadata(uint32_t id,const Entry& e)override{Change([&]{backend_->SetBasicMetadata(id,e);});}
    void SetCompressionMethod(uint32_t id,HfsPlus::Volume::CompressionMode method,std::function<void(uint64_t,uint64_t)> progress)override{
        Change([&]{backend_->SetCompressionMethod(id,method,std::move(progress));});
    }
    void Read(uint32_t id,unsigned f,uint64_t at,void* p,size_t n)override{Call([&]{backend_->Read(id,f,at,p,n);return true;});}
    uint32_t MakeDirectory(uint32_t parent,const std::u16string& name)override{
        uint32_t id=0;Change([&]{id=backend_->MakeDirectory(parent,name);});return id;
    }
    uint32_t MakeDirectoryWithAttributes(uint32_t parent,const Entry& entry)override{
        uint32_t id=0;Change([&]{id=backend_->MakeDirectoryWithAttributes(parent,entry);});return id;
    }
    void FinalizeDirectory(uint32_t id,const Entry& source,const Entry& original,bool merge)override{
        Change([&]{backend_->FinalizeDirectory(id,source,original,merge);});
    }
    uint32_t Put(uint32_t parent,const Entry& entry,Reader read,bool replace)override{
        uint32_t id=0;Change([&]{id=backend_->Put(parent,DestinationEntry(entry),std::move(read),replace);});return id;
    }
    void Rename(uint32_t id,uint32_t parent,const std::u16string& name)override{Change([&]{backend_->Rename(id,parent,name);});}
    void Remove(uint32_t id)override{Change([&]{backend_->Remove(id);});}
    uint32_t MakeHardLink(uint32_t id,uint32_t parent,const std::u16string& name,bool replace)override{
        uint32_t result=0;Change([&]{result=backend_->MakeHardLink(id,parent,name,replace);});return result;
    }
    void ValidateEntry(const Entry& entry)const override{Call([&]{backend_->ValidateEntry(entry);return true;});}
    void ValidateCopyNames(uint32_t parent,const std::vector<Entry>& entries,bool root)override{
        Call([&]{backend_->ValidateCopyNames(parent,entries,root);return true;});
    }
    void ValidateCopyTo(uint32_t id,MacBrowser::Volume& target,uint32_t parent)override{
        if(target.ResourceKey()==key_)Call([&]{backend_->ValidateCopyTo(id,*backend_,parent);return true;});
    }
    void ValidateRemoval(uint32_t id)override{
        Check(Writable(),"Physical removable partition is read-only");
        Call([&]{if(active_)backend_->ValidateRemoval(id);else Check(id!=2&&!backend_->Stat(id).protectedFile,
              "The removable entry is locked");return true;});
    }
    bool CanMoveFrom(const MacBrowser::Volume& source)const override{return kind_!="MFS"&&source.ResourceKey()==key_;}
    void MoveHere(MacBrowser::Volume& source,uint32_t id,uint32_t parent,const Entry& entry)override{
        Check(CanMoveFrom(source),"Move must stay on the same removable partition");
        Rename(id,parent,entry.name);
    }
    void ValidateMedia()override{Call([]{return true;});}
    void Reload()override{Call([&]{Refresh(true);return true;});}
    void ReleaseMedia()override{
        VolumeCoord::OperationLease lease({{key_,VolumeCoord::Mode::Write}});
        std::lock_guard<std::recursive_mutex> guard(mutex_);
        Check(!active_,"The removable disk is being written");
        released_=true;
        backend_.reset();disk_.reset();
    }
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
bool HasTestApi(){std::lock_guard<std::mutex> lock(testMutex);return bool(testApi);}
void SetTestApi(std::shared_ptr<Api> api){std::lock_guard<std::mutex> lock(testMutex);testApi=std::move(api);}
#endif
struct Disk::Impl {
 std::shared_ptr<Api> api;
 std::optional<Device> transientReader;
 std::unique_ptr<WriteAccess> write;
 HANDLE handle=INVALID_HANDLE_VALUE;
 Geometry geometry;
 DWORD generation=0,error=0;
 BYTE *buffer=nullptr,*original=nullptr,*desired=nullptr;
 Partition allowed;
 bool attempted=false;
 std::recursive_mutex mutex;
 std::list<uint64_t> ages;
 struct Page{Bytes bytes;std::list<uint64_t>::iterator age;};
 std::map<uint64_t,Page> pages;

 ~Impl(){
  if(write)write.reset();else if(handle!=INVALID_HANDLE_VALUE)api->Close(handle);
  for(auto p:{buffer,original,desired})if(p)VirtualFree(p,0,MEM_RELEASE);
 }
 struct ReadHandle {
  Impl& disk;
  explicit ReadHandle(Impl& state):disk(state){
   if(!disk.transientReader)return;
   disk.handle=disk.api->Open(*disk.transientReader,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_FLAG_RANDOM_ACCESS);
   if(disk.handle==INVALID_HANDLE_VALUE)IoError("Cannot reopen the floppy",GetLastError());
  }
  ~ReadHandle(){
   if(disk.transientReader){disk.api->Close(disk.handle);disk.handle=INVALID_HANDLE_VALUE;}
  }
 };
 void Clear(){pages.clear();ages.clear();}
 [[noreturn]]void Fail(const char* stage,DWORD e){error=e?e:ERROR_IO_DEVICE;Clear();IoError(stage,error);}
 void Verify(){
  if(error==ERROR_MEDIA_CHANGED)throw Storage::MediaChanged("Removable media changed; refresh the drive");
  if(error)IoError("Removable disk is no longer valid",error);
  DWORD now=0;
  if(!api->Verify(handle,now))Fail("Cannot verify removable media",GetLastError());
  if(now!=generation){
   error=ERROR_MEDIA_CHANGED;Clear();throw Storage::MediaChanged("Removable media changed; refresh the drive");
  }
 }
 bool Replace(uint64_t at,DWORD bytes,const BYTE* value){
  Verify();
  DWORD got=0;
  if(!write->Write(at,bytes,value,got)||got!=bytes)return false;
  Verify();
  if(!write->Flush())return false;
  Verify();
  if(!api->Read(handle,at,bytes,buffer,got)||got!=bytes)return false;
  Verify();
  return memcmp(buffer,value,bytes)==0;
 }
};
Disk::Disk(const Device& d,std::shared_ptr<Api> api,const Partition* target):p_(new Impl){
 Check(Eligible(d),"This device is not external/removable");
 p_->api=std::move(api);
 if(target){
  p_->allowed=*target;
  p_->write=p_->api->LockForWrite(d,*target);
  if(!p_->write)IoError("Cannot lock the removable disk for writing",GetLastError());
  p_->handle=p_->write->Handle();
 }else p_->handle=p_->api->Open(d,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_FLAG_RANDOM_ACCESS);
 if(p_->handle==INVALID_HANDLE_VALUE)IoError("Cannot open removable disk",GetLastError());
 if(!p_->api->GetGeometry(p_->handle,p_->geometry))IoError("Cannot determine removable disk geometry",GetLastError());
 const auto& g=p_->geometry;
 Check(g.bytes>=2048&&g.bytes<=INT64_MAX&&g.sector>=512&&g.sector<=4096&&
       (g.sector&(g.sector-1))==0&&g.bytes%g.sector==0,"Unsupported removable disk geometry");
 const bool floppyGeometry=d.floppyDrive>=0&&g.floppy&&g.sector==512&&g.bytes==1474560;
 const bool writableFloppy=target&&floppyGeometry&&!target->index&&!target->offset&&target->length==g.bytes;
 if(target)Check((writableFloppy||(!g.floppy&&d.floppyDrive<0))&&!target->readOnly&&HfsPlus::Disk::Range(target->offset,target->length,g.bytes)&&
                 !(target->offset%g.sector)&&!(target->length%g.sector),
                 "The removable partition is read-only or is not aligned for verified writes");
 if(!p_->api->Verify(p_->handle,p_->generation))IoError("Cannot establish removable media identity",GetLastError());
 const auto allocate=[](){return static_cast<BYTE*>(VirtualAlloc(nullptr,65536,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));};
 p_->buffer=allocate();
 if(target){p_->original=allocate();p_->desired=allocate();}
 Check(p_->buffer&&(!target||(p_->original&&p_->desired)),"Cannot allocate removable disk buffers");
 if(writableFloppy){
  BYTE header[512];
  Read(1024,header,sizeof(header));
  Check(HfsPlus::Disk::U16(header)==0xd2d7,"Only MFS is writable through this floppy backend");
 }
 if(!target&&floppyGeometry){
  // Windows requires the other raw volume handles to close before FSCTL_LOCK_VOLUME.
  p_->transientReader=d;
  p_->api->Close(p_->handle);
  p_->handle=INVALID_HANDLE_VALUE;
 }
}
Disk::~Disk()=default;
uint64_t Disk::Size()const{return p_->geometry.bytes;}
const Geometry& Disk::MediaGeometry()const{return p_->geometry;}
DWORD Disk::Generation()const{return p_->generation;}
bool Disk::Writable()const{return p_->write!=nullptr;}
bool Disk::WriteFenced()const{return p_->error!=0;}
bool Disk::WasWritten()const{return p_->attempted;}
void Disk::DropCache(){std::lock_guard<std::recursive_mutex> lock(p_->mutex);p_->Clear();}
void Disk::CheckMedia(){std::lock_guard<std::recursive_mutex> lock(p_->mutex);Impl::ReadHandle handle(*p_);p_->Verify();}
void Disk::Flush(){
 std::lock_guard<std::recursive_mutex> lock(p_->mutex);
 Check(Writable(),"Physical removable disk is read-only");
 // Each write already completed an ordered flush and direct readback.
 p_->Verify();
}
void Disk::Write(uint64_t at,const void* input,size_t size){
 std::lock_guard<std::recursive_mutex> lock(p_->mutex);
 Check(Writable(),"Physical removable disk is read-only");
 const auto& allowed=p_->allowed;
 Check(at>=allowed.offset&&HfsPlus::Disk::Range(at-allowed.offset,size,allowed.length)&&(!size||input),
       "Write outside the locked removable partition");
 p_->Verify();
 const auto sector=p_->geometry.sector;
 auto data=static_cast<const BYTE*>(input);
 while(size){
  const auto start=at/sector*sector;
  const auto skip=DWORD(at-start);
  const auto n=DWORD((std::min)(size,size_t(65536-skip)));
  const auto bytes=(skip+n+sector-1)/sector*sector;
  DWORD got=0;
  if(!p_->api->Read(p_->handle,start,bytes,p_->original,got)||got!=bytes)
      p_->Fail("Cannot read original removable sectors before writing",ERROR_READ_FAULT);
  p_->Verify();
  memcpy(p_->desired,p_->original,bytes);
  memcpy(p_->desired+skip,data,n);
  if(memcmp(p_->desired,p_->original,bytes)!=0){
   p_->Clear();
   p_->attempted=true;
   bool good=false;
   std::exception_ptr failure;
   try{good=p_->Replace(start,bytes,p_->desired);}catch(...){failure=std::current_exception();}
   if(!good){
    bool restored=false;
    if(!p_->error)try{restored=p_->Replace(start,bytes,p_->original);}catch(...){}
    Storage::OutcomeEvidence::NoteRollback(restored?Storage::Rollback::Restored:Storage::Rollback::Unverified);
    if(failure)std::rethrow_exception(failure);
    if(!restored)p_->Fail("Removable write could not be verified or restored",ERROR_WRITE_FAULT);
    IoError("Removable write failed verification; the affected sectors were restored",ERROR_WRITE_FAULT);
   }
  }
  data+=n;at+=n;size-=n;
 }
}
void Disk::Read(uint64_t at,void* output,size_t size){std::lock_guard<std::recursive_mutex> lock(p_->mutex);Impl::ReadHandle handle(*p_);p_->Verify();Check(Disk::Size()>=at&&size<=Disk::Size()-at&&(!size||output),"Read outside removable disk");auto out=static_cast<BYTE*>(output);while(size){uint64_t page=at/65536*65536;auto found=p_->pages.find(page);if(found==p_->pages.end()){DWORD bytes=DWORD((std::min)(uint64_t(65536),Size()-page)),got=0;if(!p_->api->Read(p_->handle,page,bytes,p_->buffer,got))p_->Fail("Removable disk read failed",GetLastError());if(got!=bytes)p_->Fail("Short removable disk read",ERROR_READ_FAULT);p_->Verify();if(p_->pages.size()>=64){p_->pages.erase(p_->ages.back());p_->ages.pop_back();}p_->ages.push_front(page);Impl::Page value;value.bytes.assign(p_->buffer,p_->buffer+bytes);value.age=p_->ages.begin();found=p_->pages.emplace(page,std::move(value)).first;}else{p_->ages.splice(p_->ages.begin(),p_->ages,found->second.age);found->second.age=p_->ages.begin();}auto skip=size_t(at-page),n=(std::min)(size,found->second.bytes.size()-skip);memcpy(out,found->second.bytes.data()+skip,n);out+=n;at+=n;size-=n;}p_->Verify();}
std::vector<Mounted> Open(const Device& device,std::shared_ptr<Api> api){
    if(!api)api=NativeApi();
    auto disk=std::make_shared<Disk>(device,api);
    std::vector<Mounted> result;
    for(const auto& partition:Partitions(*disk,disk->MediaGeometry().sector)){
        auto key=Key(device,partition);
        auto ids=std::make_shared<MacBrowser::MfsCatalogIds>();
        auto slice=std::make_unique<Slice>(disk,partition);
        const auto selection=SelectPartition(*slice,disk->MediaGeometry().floppy);
        if(!selection)continue;
        auto backend=OpenSelectedPartition(std::move(slice),*selection,key,ids);
        if(!backend)continue;
        Mounted mounted;
        mounted.device=device;
        mounted.partition=partition;
        mounted.volume=MacBrowser::TrackWorkerVolume(std::make_shared<MediaVolume>(
            device,partition,*selection,api,disk,std::move(backend),key,std::move(ids)));
        mounted.label=device.label;
        if(partition.index)mounted.label+=L", partition "+std::to_wstring(partition.index);
        result.push_back(std::move(mounted));
    }
    disk->CheckMedia();
    return result;
}
Discovery Scan(Storage::Job& job,std::shared_ptr<Api> api){if(!api)api=NativeApi();Discovery result;auto devices=api->Enumerate();std::set<std::wstring> seen;for(auto& device:devices){if(job.cancelled())throw Storage::Cancelled();if(!Eligible(device)||!seen.insert(device.path).second)continue;Storage::Progress progress;progress.phase="Checking Mac removable disks";progress.path=Utf8(device.label);progress.utf8=true;job.progress(progress);try{auto volumes=MediaDiscovery::External(device,api,job);result.volumes.insert(result.volumes.end(),volumes.begin(),volumes.end());}catch(const Storage::MediaChanged&){throw;}catch(const Storage::Cancelled&){throw;}catch(const std::exception& e){result.failedDevices.push_back(device.path);result.errors.push_back(Utf8(device.label)+": "+e.what());}}return result;}
std::vector<Mounted> FromDriveRoot(const std::wstring& root,Storage::Job* job,bool floppyOnly){if(!Root(root))return {};auto api=NativeApi();if(api->WindowsReadableRoot(root))return {};Device device;if(!api->FromRoot(root,device)||(floppyOnly&&device.floppyDrive<0))return {};return job?MediaDiscovery::External(device,api,*job):Open(device,api);}
std::shared_ptr<MacBrowser::Volume> Reopen(const std::wstring& path,uint64_t offset){auto api=NativeApi();for(auto& d:api->Enumerate())if(!_wcsicmp(d.path.c_str(),path.c_str()))for(auto& v:Open(d,api))if(v.partition.offset==offset)return v.volume;Device d;if(path.size()==6&&path.substr(0,4)==L"\\\\.\\"&&api->FromRoot(path.substr(4)+L"\\",d))for(auto& v:Open(d,api))if(v.partition.offset==offset)return v.volume;throw Error("The removable Mac partition is no longer present; scan removable disks again");}
}
