// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "WorkerEndpoint.h"
#include "PhysicalRemovable.h"
#include "FloppyFileWrite.h"
#include "FloppyWriter.h"
#include "DirectFloppy.h"
#include "FakeFloppy.h"
#include "floppy.h"
#include "RemovableUndo.h"
#include "ClassicHfsRecovery.h"
#include "HfsPlusDisk.h"
#include "DiskSlice.h"
#include "MfsDiskSource.h"
#include "ReadOnlyCdBackend.h"
#include "ReadOnlySource.h"
#include "StorageJobDialog.h"
#include "DeviceWait.h"
#include "FileOperations.h"
#include "StorageOutcome.h"
#include "HFVExplorer.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "MediaDiscovery.h"
#include "BrowserDirectory.h"
#include "OptionsPage1.h"
#include "utils.h"
#include <cstdio>
#include <fcntl.h>

FloppyWriteApi& NativeFloppyWriteApiForTest();
extern "C" {
#include "hfs/ClassicHfs.h"
#define _FHOOK_INTERNAL_
#include "filehook.h"
}
namespace {
using HfsPlus::Bytes;namespace B=HfsPlus::Disk;
void Check(bool ok,const char* message){if(!ok)throw HfsPlus::Error(message);}
template<class F>void Reject(F f,const char* why){try{f();}catch(const std::exception&){return;}throw HfsPlus::Error(why);}
void SourceAdapters(FILE* report){
 struct Backing final:HfsPlus::Source{
  Bytes bytes=Bytes(32,0x42);
  bool local=false,changed=false;
  unsigned writes=0,flushes=0;
  uint64_t Size()const override{return bytes.size();}
  bool Writable()const override{return true;}
  bool ProcessLocal()const override{return local;}
  void CheckMedia()override{if(changed)throw Storage::MediaChanged("Injected media swap");}
  int MediaDrive()const override{return 4;}
  bool MediaGeneration(uint32_t& generation)const override{generation=7;return true;}
  std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{
   if(local)return {};
   return {{L"image",8}};
  }
  void Read(uint64_t at,void* p,size_t n)override{memcpy(p,bytes.data()+at,n);}
  void Write(uint64_t at,const void* p,size_t n)override{memcpy(bytes.data()+at,p,n);++writes;}
  void Flush()override{++flushes;}
 };
 auto backing=std::make_shared<Backing>();
 auto writable=std::make_shared<DiskImage::Slice>(backing,8,16,true);
 auto readOnly=std::make_shared<DiskImage::Slice>(backing,8,16);
 const uint8_t value=0x57;
 uint8_t got=0;
 Check(writable->Writable()&&!readOnly->Writable(),"Partition write policy was lost");
 writable->Write(15,&value,1);
 Check(backing->bytes[22]==0x42&&backing->bytes[23]==value&&backing->bytes[24]==0x42,
       "A bounded write changed data outside its partition");
 readOnly->Read(15,&got,1);
 Check(got==value,"Partition read did not translate its offset");
 Reject([&]{writable->Write(16,&value,1);},"Partition accepted an out-of-bounds write");
 Reject([&]{readOnly->Write(0,&value,1);},"Read-only partition accepted a write");
 Reject([&]{DiskImage::Slice outside(backing,16,17);},"Slice accepted invalid backing bounds");
 MfsDiskSource mfs(writable,true),locked(readOnly,true);
 Check(mfs.WriteAt(0,&value,1)&&!mfs.WriteAt(16,&value,1)&&!locked.WriteAt(0,&value,1),
       "MFS adapter ignored partition write permissions");
 Check(mfs.Flush()&&backing->flushes==1,"MFS adapter did not forward a writable flush");
 mfs.DisableWrites();
 Check(!mfs.WriteAt(0,&value,1)&&backing->writes==2,"MFS adapter ignored its write fence");
 uint32_t generation=0;
 Check(writable->MediaDrive()==4&&writable->MediaGeneration(generation)&&generation==7,
       "Partition dropped media identity");
 Check(mfs.WorkerFiles()==backing->WorkerFiles(),"MFS adapter lost disk worker handles");
 backing->local=true;
 Check(mfs.ProcessLocal()&&mfs.WorkerFiles().empty(),"MFS adapter exposed a process-local disk to workers");
 backing->changed=true;
 Reject([&]{readOnly->CheckMedia();},"Partition suppressed a media change");
 fprintf(report,"PASS: shared partition bounds, write fence, media identity and process-local MFS source.\n");
}
void Payload(unsigned f,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<uint8_t*>(p)[i]=uint8_t((at+i)*13+f*71);}
std::shared_ptr<Bytes> Load(const std::string& path){CFile f;Check(f.Open(path.c_str(),CFile::modeRead|CFile::shareDenyNone)!=FALSE,"Read simulated disk fixture");auto bytes=std::make_shared<Bytes>(size_t(f.GetLength()));Check(f.Read(bytes->data(),UINT(bytes->size()))==bytes->size(),"Read simulated disk bytes");return bytes;}
std::shared_ptr<Bytes> Plus(const std::string& path,HfsPlus::Volume::FormatKind kind){auto wide=VolumeCoord::Wide(path);{auto s=HfsPlus::CreateImage(wide.c_str(),16*1024*1024);HfsPlus::Volume::Format(*s,u"USB Mac",true,kind);HfsPlus::Volume v(*s,true);auto f=v.Put(2,u"Hello",5003,723,Payload);v.SetAttributes(f,{{u"user.keep",{1,2,3}}});v.MakeHardLink(f,2,u"Alias");v.SetCompression(v.List().back().id,true);}return Load(path);}
std::shared_ptr<Bytes> Classic(const std::string& path){CFile f;Check(f.Open(path.c_str(),CFile::modeCreate|CFile::modeWrite)!=FALSE,"Create Zip fixture");f.SetLength(4*1024*1024);f.Close();char label[]="Zip Classic";Check(hfs_format(const_cast<char*>(path.c_str()),0,label)==0,"Format Zip fixture");auto v=hfs_mount(const_cast<char*>(path.c_str()),0,O_RDWR);char name[]="Zip Classic:Hello",type[]="TEXT",creator[]="TEST";Check(v&&hfs_create(v,name,type,creator)==0,"Create classic fixture file");auto file=hfs_open(v,name);Check(file!=nullptr,"Open classic fixture file");for(unsigned fork=0;fork<2;++fork){Bytes bytes(fork?723:5003);Payload(fork,0,bytes.data(),bytes.size());Check(hfs_setfork(file,fork)==0&&hfs_write(file,bytes.data(),unsigned(bytes.size()))==bytes.size(),"Write classic fixture forks");}Check(hfs_close(file)==0&&hfs_umount(v)==0,"Close classic fixture");return Load(path);}
class MfsMemory:public MfsSource{public:Bytes bytes=Bytes(1474560);uint64_t Length()const override{return bytes.size();}bool Writable()const override{return true;}bool ReadAt(uint64_t at,void* p,size_t n)override{if(!B::Range(at,n,bytes.size()))return false;memcpy(p,bytes.data()+at,n);return true;}bool WriteAt(uint64_t at,const void* p,size_t n)override{if(!B::Range(at,n,bytes.size()))return false;memcpy(bytes.data()+at,p,n);return true;}bool Flush()override{return true;}};
std::shared_ptr<Bytes> Mfs(){MfsMemory memory;std::string error;Check(MfsImage::Format(memory,"MFS",1474560,false,error),error.c_str());MfsImage v;Check(v.Open(memory),"Open MFS fixture");MfsEntry e;e.name="Hello";e.forks[0].length=5003;e.forks[1].length=723;uint32_t id=0;Check(v.Put(e,Payload,false,id),"Populate MFS fixture");return std::make_shared<Bytes>(std::move(memory.bytes));}
struct Store{Removable::Device device;Removable::Geometry geometry;std::map<uint64_t,std::shared_ptr<Bytes>> segments;DWORD generation=7,error=0;bool shortRead=false,noIdentity=false,changeAfterRead=false,pattern=false;bool writable=false,locked=false,denyLock=false,swapAtLock=false,swapOnWrite=false;
 bool corruptReadback=false,pendingReadback=false,failFlush=false;
 std::function<void(Store&)> changeAtLock;
 std::function<void(uint64_t,DWORD,const void*)> persist;
 unsigned opens=0,closes=0,reads=0,checks=0,locks=0,writes=0,flushes=0;
 Removable::Partition target;};
class Fake:public Removable::Api {
 class Locked final:public Removable::WriteAccess {
  Fake& api_;
  HANDLE handle_;
 public:
  Locked(Fake& api,HANDLE handle):api_(api),handle_(handle){}
  ~Locked(){
   {std::lock_guard<std::mutex> guard(api_.mutex);api_.Get(handle_)->locked=false;}
   api_.Close(handle_);
  }
  HANDLE Handle()const override{return handle_;}
  bool Write(uint64_t at,DWORD n,const void* data,DWORD& got)override{
   std::lock_guard<std::mutex> guard(api_.mutex);
   auto store=api_.Get(handle_);
   Check(store->locked,"Simulated write escaped exclusive access");
   Check(at%store->geometry.sector==0&&n%store->geometry.sector==0&&
         reinterpret_cast<uintptr_t>(data)%store->geometry.sector==0,"Unaligned removable write");
   const auto& part=store->target;
   Check(at>=part.offset&&B::Range(at-part.offset,n,part.length),"Simulated write escaped its locked partition");
   if(store->swapOnWrite){
    store->swapOnWrite=false;
    ++store->generation;
    SetLastError(ERROR_MEDIA_CHANGED);
    return false;
   }
   auto segment=store->segments.at(part.offset);
   memcpy(segment->data()+at-part.offset,data,n);
   if(store->persist)store->persist(at-part.offset,n,data);
   got=n;
   ++store->writes;
   store->pendingReadback=true;
   return true;
  }
  bool Flush()override{
   std::lock_guard<std::mutex> guard(api_.mutex);
   auto store=api_.Get(handle_);
   ++store->flushes;
   return !store->failFlush;
  }
 };
 uintptr_t next=1;std::map<HANDLE,std::shared_ptr<Store>> handles;std::mutex mutex;
 std::shared_ptr<Store> Get(HANDLE h){auto i=handles.find(h);Check(i!=handles.end(),"Unknown simulated removable handle");return i->second;}
public:
 std::vector<std::shared_ptr<Store>> devices;bool workerOnly=false,wrongThread=false,windowsReadable=false;std::atomic<unsigned> enumerations{0};HANDLE entered=CreateEvent(nullptr,TRUE,FALSE,nullptr),release=CreateEvent(nullptr,TRUE,FALSE,nullptr);std::atomic<bool> gate{false};
 ~Fake(){SetEvent(release);CloseHandle(entered);CloseHandle(release);}
 void Thread(){if(workerOnly&&GetCurrentThreadId()==StorageUi::state().thread)wrongThread=true;}
 std::shared_ptr<Store> Add(const wchar_t* name,std::shared_ptr<Bytes> bytes,STORAGE_BUS_TYPE bus=BusTypeUsb,bool removable=true){auto d=std::make_shared<Store>();d->device.path=name;d->device.label=name;d->device.bus=bus;d->device.removable=removable;d->device.number=DWORD(100+devices.size());d->geometry.bytes=bytes->size();d->segments[0]=std::move(bytes);devices.push_back(d);return d;}
 std::vector<Removable::Device> Enumerate()override{Thread();++enumerations;std::vector<Removable::Device> out;for(auto& d:devices)out.push_back(d->device);return out;}
 bool FromRoot(const std::wstring& root,Removable::Device& out)override{Thread();if(root!=L"Z:\\"||devices.empty())return false;out=devices.front()->device;return true;}
 bool WindowsReadableRoot(const std::wstring&)override{Thread();return windowsReadable;}
 HANDLE Open(const Removable::Device& d,DWORD access,DWORD sharing,DWORD flags)override{Thread();Check(access==GENERIC_READ&&sharing==(FILE_SHARE_READ|FILE_SHARE_WRITE)&&flags==FILE_FLAG_RANDOM_ACCESS,"Removable transport requested writable/unsafe open");std::lock_guard<std::mutex> lock(mutex);for(auto& s:devices)if(s->device.path==d.path){if(s->error==ERROR_ACCESS_DENIED){SetLastError(s->error);return INVALID_HANDLE_VALUE;}++s->opens;auto h=reinterpret_cast<HANDLE>(next++);handles[h]=s;return h;}SetLastError(ERROR_DEVICE_NOT_CONNECTED);return INVALID_HANDLE_VALUE;}
 bool GetGeometry(HANDLE h,Removable::Geometry& g)override{Thread();std::lock_guard<std::mutex> lock(mutex);g=Get(h)->geometry;return true;}
 bool Verify(HANDLE h,DWORD& generation)override{Thread();if(gate.exchange(false)){SetEvent(entered);Check(WaitForSingleObject(release,15000)==WAIT_OBJECT_0,"Release simulated removable spin-up");}std::lock_guard<std::mutex> lock(mutex);auto s=Get(h);++s->checks;if(s->error||s->noIdentity){SetLastError(s->error?s->error:ERROR_NOT_SUPPORTED);return false;}generation=s->generation;return true;}
 bool Read(HANDLE h,uint64_t at,DWORD bytes,void* p,DWORD& got)override{Thread();std::lock_guard<std::mutex> lock(mutex);auto s=Get(h);Check(at%s->geometry.sector==0&&bytes%s->geometry.sector==0&&reinterpret_cast<uintptr_t>(p)%s->geometry.sector==0,"Unaligned physical removable read");Check(B::Range(at,bytes,s->geometry.bytes),"Read outside simulated media");++s->reads;memset(p,0,bytes);if(s->pattern)for(DWORD i=0;i<bytes;++i)static_cast<uint8_t*>(p)[i]=uint8_t((at+i)*13+((at+i)>>32));for(auto& part:s->segments){auto begin=(std::max)(at,part.first),end=(std::min)(at+bytes,part.first+part.second->size());if(begin<end)memcpy(static_cast<uint8_t*>(p)+begin-at,part.second->data()+begin-part.first,size_t(end-begin));}got=s->shortRead?bytes-1:bytes;
 if(s->pendingReadback&&s->locked){
  s->pendingReadback=false;
  if(s->corruptReadback){s->corruptReadback=false;static_cast<uint8_t*>(p)[0]^=1;}
 }
 if(s->changeAfterRead){s->changeAfterRead=false;++s->generation;}return true;}
 void Close(HANDLE h)override{std::lock_guard<std::mutex> lock(mutex);auto s=Get(h);++s->closes;handles.erase(h);}
 bool CanWrite(const Removable::Device& device)const override{
  for(auto& store:devices)if(store->device.path==device.path)return store->writable;
  return false;
 }
 std::unique_ptr<Removable::WriteAccess> LockForWrite(const Removable::Device& device,
                                                       const Removable::Partition& part)override{
  std::shared_ptr<Store> target;
  {
   std::lock_guard<std::mutex> guard(mutex);
   for(auto& store:devices)if(store->device.path==device.path)target=store;
   Check(bool(target)&&target->writable,"Invalid simulated writable disk");
   if(target->locked||target->denyLock||
      (target->device.floppyDrive>=0&&target->opens!=target->closes)){SetLastError(ERROR_SHARING_VIOLATION);return {};}
   target->locked=true;
   target->target=part;
   ++target->locks;
   if(target->swapAtLock){target->swapAtLock=false;++target->generation;}
   if(target->changeAtLock)std::exchange(target->changeAtLock,{})(*target);
  }
  return std::make_unique<Locked>(*this,Open(device,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_FLAG_RANDOM_ACCESS));
 }
 unsigned Active(){std::lock_guard<std::mutex> lock(mutex);return unsigned(handles.size());}
};
uint32_t Find(MacBrowser::Volume& v,uint32_t p,const std::u16string& n){MacBrowser::Entry e;Check(v.FindChild(p,n,e),"Missing removable fixture entry");return e.id;}
void Verify(MacBrowser::Volume& v){auto id=Find(v,2,u"Hello");auto e=v.Stat(id);Check(e.forks[0].size==5003&&e.forks[1].size==723,"Physical Mac fork sizes");for(unsigned f=0;f<2;++f){Bytes actual(size_t(e.forks[f].size)),wanted(actual.size());Payload(f,0,wanted.data(),wanted.size());v.Read(id,f,0,actual.data(),actual.size());Check(actual==wanted,"Physical Mac fork contents");}}
void Little(uint8_t* p,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i)p[i]=uint8_t(n>>(i*8));}
uint32_t Crc(const Bytes& b){uint32_t crc=~0u;for(auto x:b){crc^=x;for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&uint32_t(-int(crc&1)));}return ~crc;}
std::shared_ptr<Store> Apm(Fake& api,std::shared_ptr<Bytes> classic,std::shared_ptr<Bytes> plus){auto d=api.Add(L"Zip APM",std::make_shared<Bytes>(2048),BusTypeScsi,true);uint64_t base=4ULL*1024*1024*1024+65536,next=base+classic->size()+65536;d->geometry.bytes=next+plus->size();auto& head=*d->segments[0];B::P16(head.data(),0x4552);B::P16(head.data()+2,512);B::P32(head.data()+4,uint32_t(d->geometry.bytes/512));for(unsigned i=0;i<3;++i){auto p=head.data()+(i+1)*512;B::P16(p,0x504d);B::P32(p+4,3);B::P32(p+8,uint32_t(i==0?1:(i==1?base:next)/512));B::P32(p+12,uint32_t(i==0?3:(i==1?classic->size():plus->size())/512));strcpy(reinterpret_cast<char*>(p+48),i==0?"Apple_partition_map":"Apple_HFS");}d->segments[base]=classic;d->segments[next]=plus;return d;}
std::shared_ptr<Store> Gpt(Fake& api,std::shared_ptr<Bytes> plus,DWORD sector,uint64_t base=1024*1024){auto d=api.Add((L"USB GPT "+std::to_wstring(sector)).c_str(),std::make_shared<Bytes>(512),BusTypeUsb,false);d->geometry.sector=sector;d->geometry.bytes=base+plus->size()+1024*1024;auto sectors=d->geometry.bytes/sector;auto& boot=*d->segments[0];boot[510]=0x55;boot[511]=0xaa;boot[450]=0xee;Little(boot.data()+454,1,4);Little(boot.data()+458,sectors-1,4);auto table=std::make_shared<Bytes>(512);static const uint8_t type[]={0,0x53,0x46,0x48,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};memcpy(table->data(),type,16);(*table)[16]=1;auto start=base/sector;Little(table->data()+32,start,8);Little(table->data()+40,start+plus->size()/sector-1,8);for(bool backup:{false,true}){auto h=std::make_shared<Bytes>(sector);memcpy(h->data(),"EFI PART",8);Little(h->data()+8,0x10000,4);Little(h->data()+12,92,4);Little(h->data()+24,backup?sectors-1:1,8);Little(h->data()+32,backup?1:sectors-1,8);Little(h->data()+40,34,8);Little(h->data()+48,sectors-34,8);(*h)[56]=9;Little(h->data()+72,backup?sectors-2:2,8);Little(h->data()+80,4,4);Little(h->data()+84,128,4);Little(h->data()+88,Crc(*table),4);Little(h->data()+16,Crc(Bytes(h->begin(),h->begin()+92)),4);d->segments[(backup?sectors-1:1)*sector]=h;d->segments[(backup?sectors-2:2)*sector]=std::make_shared<Bytes>(*table);}d->segments[base]=plus;return d;}
void UpdateGptTable(Store& image,const Bytes& table)
{
 const auto sector=image.geometry.sector;
 const auto sectors=image.geometry.bytes/sector;
 for(bool backup:{false,true}){
  const auto lba=backup?sectors-1:1;
  auto header=std::make_shared<Bytes>(*image.segments.at(lba*sector));
  Little(header->data()+88,Crc(table),4);
  Little(header->data()+16,0,4);
  Little(header->data()+16,Crc(Bytes(header->begin(),header->begin()+92)),4);
  image.segments[lba*sector]=std::move(header);
  image.segments[(backup?sectors-2:2)*sector]=std::make_shared<Bytes>(table);
 }
}

void WritableMedia(const std::string& root,const std::shared_ptr<Bytes>& classic,
                   const std::shared_ptr<Bytes>& plus,const std::shared_ptr<Bytes>& hfsx,
                   const std::shared_ptr<Bytes>& mfs){
 auto api=std::make_shared<Fake>();
 auto zip=Apm(*api,std::make_shared<Bytes>(*classic),std::make_shared<Bytes>(*plus));
 auto external=Gpt(*api,std::make_shared<Bytes>(*hfsx),4096);
 auto raw=api->Add(L"MFS removable",std::make_shared<Bytes>(*mfs));
 auto floppy=api->Add(L"MFS floppy",std::make_shared<Bytes>(*mfs));
 floppy->geometry.floppy=true;
 floppy->device.floppyDrive=0;
 for(const auto& drive:{zip,external,raw,floppy})drive->writable=true;
 auto zipMounts=Removable::Open(zip->device,api);
 auto gptMounts=Removable::Open(external->device,api);
 auto mfsMounts=Removable::Open(raw->device,api);
 Check(zipMounts.size()==2&&gptMounts.size()==1&&mfsMounts.size()==1,"Discover writable test partitions");
 auto verifyWrite=[&](const Removable::Mounted& selected,bool flat){
  auto volume=selected.volume;
  Check(volume->Writable()&&MacBrowser::ExecutionFor(*volume).processLocal,"Writable disk did not use retained local backend");
  auto drive=*std::find_if(api->devices.begin(),api->devices.end(),
      [&](const auto& device){return device->device.path==selected.device.path;});
  auto others=Removable::Open(selected.device,api);
  auto observer=std::find_if(others.begin(),others.end(),[&](const auto& mount){return mount.partition==selected.partition;})->volume;
  const auto id=Find(*volume,2,u"Hello");
  const auto observerId=Find(*observer,2,u"Hello");
  std::map<uint64_t,Bytes> neighbors;
  for(const auto& [offset,bytes]:drive->segments)
   if(offset!=selected.partition.offset)neighbors.emplace(offset,*bytes);
   volume->Rename(id,2,u"Original");
  Check(Find(*observer,2,u"Original")==observerId,"Second mount retained its old catalog after a write");
  auto descriptor=volume->Stat(id);
  descriptor.name=u"Hello";
  auto written=volume->Put(2,descriptor,Payload,false);
  Verify(*volume);
  volume->Remove(id);
  if(!flat){auto folder=volume->MakeDirectory(2,u"New folder");volume->Remove(folder);}
  Verify(*observer);
  Check(Find(*volume,2,u"Hello")==written,"Writable filesystem IDs did not survive reopening");
  for(const auto& [offset,bytes]:neighbors)
   Check(*drive->segments.at(offset)==bytes,"Written filesystem modified an adjacent partition or disk map");
  Check(drive->writes>0&&drive->flushes>=drive->writes&&!drive->locked,"Write session skipped flush or leaked its lock");
 };
 verifyWrite(zipMounts[0],false);
 verifyWrite(zipMounts[1],false);
 verifyWrite(gptMounts[0],false);
 verifyWrite(mfsMounts[0],true);

 auto floppyMounts=Removable::Open(floppy->device,api);
 Check(floppyMounts.size()==1,"Discover writable MFS floppy");
 verifyWrite(floppyMounts.front(),true);
 auto floppyVolume=floppyMounts.front().volume;
 auto floppyFile=floppyVolume->Stat(Find(*floppyVolume,2,u"Hello"));
 floppyFile.version=10;
 floppyFile.finder[3]=0x64;
 floppyVolume->SetBasicMetadata(floppyFile.id,floppyFile);
 auto updated=floppyVolume->Stat(floppyFile.id);
 Check(updated.version==10&&updated.finder[3]==0x64,"MFS floppy metadata changed incorrectly");
 const Bytes floppyBefore=*floppy->segments.at(0);
 floppy->corruptReadback=true;
 Reject([&]{floppyVolume->Rename(floppyFile.id,2,u"Bad rename");},"MFS floppy accepted mismatched readback");
 Check(*floppy->segments.at(0)==floppyBefore&&!floppy->locked,"MFS floppy rollback changed the disk or leaked its lock");
 floppy->swapAtLock=true;
 const auto floppyWrites=floppy->writes;
 Reject([&]{floppyVolume->Rename(floppyFile.id,2,u"Wrong disc");},"MFS floppy accepted a changed generation");
 Check(floppy->writes==floppyWrites&&!floppy->locked,"A changed floppy was written or left locked");

 auto otherFloppy=api->Add(L"Unsupported floppy",std::make_shared<Bytes>(*plus));
 otherFloppy->geometry.floppy=true;
 otherFloppy->device.floppyDrive=1;
 otherFloppy->writable=true;
 Check(!Removable::Open(otherFloppy->device,api).front().volume->Writable(),"Non-MFS floppy was made writable here");

 // A second raw writer must fail, and an intervening swap must precede every write.
 auto selected=gptMounts[0];
 const auto before=external->writes;
 auto table=*external->segments.at(2*external->geometry.sector);
 Little(table.data()+48,1ULL<<60,8);
 UpdateGptTable(*external,table);
 Reject([&]{selected.volume->MakeDirectory(2,u"Changed map");},"An edited partition table was accepted without reopening");
 auto readOnly=Removable::Open(external->device,api).front();
 const auto readOnlyFeatures=readOnly.volume->Supports();
 Check(!readOnlyFeatures.writable&&readOnlyFeatures.attributes&&!readOnlyFeatures.CanEditAttributes()&&
       !readOnlyFeatures.CanEditCompression()&&!readOnlyFeatures.CanCreateHardLinks()&&external->writes==before,
       "A physical GPT read-only flag did not restrict the backend capabilities");
 Little(table.data()+48,0,8);
 UpdateGptTable(*external,table);
 {
  Removable::Disk writer(external->device,api,&selected.partition);
  Reject([&]{selected.volume->MakeDirectory(2,u"Locked");},"A conflicting raw writer was accepted");
  Check(external->writes==before,"Lock failure modified sectors");
  const uint8_t byte=0x5a;
  Reject([&]{writer.Write(selected.partition.offset-1,&byte,1);},"Cross-partition write was accepted");
 }
 external->swapAtLock=true;
 Reject([&]{selected.volume->MakeDirectory(2,u"Swapped");},"A media swap during locking was accepted");
 Check(!external->locked&&external->writes==before,"Media swap before writing modified sectors");

 auto fixture=api->Add(L"Readback fault",std::make_shared<Bytes>(*plus));
 fixture->writable=true;
 auto target=Removable::Open(fixture->device,api).front();
 const Bytes old=*fixture->segments.at(0);
 {
  Removable::Disk writer(fixture->device,api,&target.partition);
  const uint8_t byte=0x5a;
  Storage::OutcomeEvidence evidence;
  fixture->corruptReadback=true;
  Reject([&]{writer.Write(29,&byte,1);},"Mismatching readback was reported as success");
  Check(*fixture->segments.at(0)==old&&evidence.RollbackState()==Storage::Rollback::Restored,
        "Readback mismatch did not verify restoration of the original sectors");
  fixture->swapOnWrite=true;
  const auto attempts=fixture->writes;
  Reject([&]{writer.Write(29,&byte,1);},"A swap during a write was accepted");
  Check(writer.WriteFenced()&&fixture->writes==attempts,"Recovery wrote to swapped media");
 }
 target=Removable::Open(fixture->device,api).front();
 auto observer=Removable::Open(fixture->device,api).front();
 const auto host=root+"\\cut-source";
 Check(CreateDirectoryA(host.c_str(),nullptr),"Create simulated Cut source folder");
 auto source=MacBrowser::OpenWindows(VolumeCoord::Wide(host));
 MacBrowser::Entry file;file.name=u"Retain me";file.forks[0].size=2049;
 auto id=source->Put(2,file,Payload,false);
 FileOperations::Request request;
 request.Copy().sources={FileOperations::Location::Mounted(source,id)};
 request.Copy().sources.front().cut=true;
 request.Copy().destination=FileOperations::Location::Mounted(target.volume,2);
 request.Copy().sources.front().cut=false;
 auto copied=FileOperations::Submit(request);
 copied.Job()->wait();
 Check(!copied.Job()->result().code,copied.Job()->result().message.c_str());
 const auto received=Find(*target.volume,2,file.name);
 Bytes readback(file.forks[0].size),expected(readback.size());
 Payload(0,0,expected.data(),expected.size());
 target.volume->Read(received,0,0,readback.data(),readback.size());
 Check(readback==expected,"File operation did not write the removable destination");
 target.volume->Remove(received);
 request.Copy().sources.front().cut=true;
 fixture->failFlush=true;
 auto operation=FileOperations::Submit(request);
 operation.Job()->wait();
 Check(operation.Job()->result().code&&operation.Job()->result().rollback!=Storage::Rollback::None&&
       Find(*source,2,file.name)==id&&!target.volume->Writable()&&!observer.volume->Writable()&&!fixture->locked,"An unverified Cut deleted its source, left writes enabled, or leaked the lock");

 auto changing=api->Add(L"MFS Cut source",std::make_shared<Bytes>(*mfs));
 changing->writable=true;
 auto changingVolume=Removable::Open(changing->device,api).front().volume;
 auto copiedId=Find(*changingVolume,2,u"Hello");
 changing->changeAtLock=[](Store& disk){
  MfsMemory edited;
  edited.bytes=*disk.segments.at(0);
  MfsImage filesystem;
  Check(filesystem.Open(edited)&&filesystem.Entries().size()==1,"Open simulated media for external rename");
  Check(filesystem.Rename(filesystem.Entries().front().id,"Changed externally"),"Simulated external rename");
  *disk.segments.at(0)=std::move(edited.bytes);
 };
 FileOperations::OrdinaryCopy move{
  {FileOperations::Location::Mounted(changingVolume,copiedId)}, FileOperations::Location::Mounted(source,2)};
 move.sources.front().cut=true;
 auto changed=FileOperations::Submit(std::move(move));
 changed.Job()->wait();
 Check(changed.Job()->result().error==ERROR_MEDIA_CHANGED&&changing->locks==1&&
       !changing->writes&&!changing->locked,"Cut removed source data changed while taking the media lock");
 auto fresh=Removable::Open(changing->device,api).front().volume;
 Check(Find(*fresh,2,u"Changed externally")!=0,"The externally changed source was not retained");
 Verify(*source);
}

std::wstring SaveGptImage(const std::string& path,const Store& image)
{
 const auto wide=VolumeCoord::Wide(path);
 auto file=HfsPlus::CreateFormatOutput(wide.c_str(),image.geometry.bytes);
 for(const auto& [offset,bytes]:image.segments)
  file->Write(offset,bytes->data(),bytes->size());
 file->Flush();
 file->Keep();
 return wide;
}

std::wstring GptImages(const std::string& root,const Store& gpt,const std::shared_ptr<Bytes>& hfsPlus,const std::shared_ptr<Bytes>& hfsx)
{
 auto layout=gpt;
 layout.segments[1024*1024]=hfsPlus;
 const auto sector=layout.geometry.sector;
 auto table=*layout.segments.at(2*sector);
 // Put an unrelated partition before the HFS+ entry.
 memcpy(table.data()+128,table.data(),128);
 table[0]=0xda;
 table[16]=2;
 const auto neighbor=1024*1024-2*sector;
 Little(table.data()+32,neighbor/sector,8);
 Little(table.data()+40,neighbor/sector+1,8);
 layout.segments[neighbor]=std::make_shared<Bytes>(2*sector,0x5a);
 UpdateGptTable(layout,table);
 const auto path=SaveGptImage(root+"\\GPT 512.img",layout);
 {
  const auto volume=MacBrowser::OpenBrowserImage(path);
  Check(volume&&volume->Writable()&&!strcmp(volume->Kind(),"HFS+")&&volume->Journaled()&&
        MacBrowser::ExecutionFor(*volume).Persistent(),"Open journaled HFS+ from GPT on its persistent worker");
  Verify(*volume);
  const auto id=volume->MakeDirectory(2,u"GPT folder");
  volume->Rename(id,2,u"GPT edited");
  Check(Find(*volume,2,u"GPT edited")==id,"GPT rename did not update the worker catalog");
 }
 {
  const auto volume=MacBrowser::OpenImage(path);
  Check(volume&&volume->Writable(),"Reopen the modified GPT volume");
  Find(*volume,2,u"GPT edited");
  Verify(*volume);
 }
 {
  auto image=HfsPlus::OpenImage(path.c_str(),false);
  Check(image->Size()==layout.geometry.bytes,"GPT write changed the disk image length");
  for(const auto& [offset,bytes]:layout.segments)
   if(offset!=1024*1024)
    Check(B::Read(*image,offset,bytes->size())==*bytes,"GPT write changed partition tables or neighboring data");
 }
 auto bad=layout;
 const auto last=bad.geometry.bytes-sector;
 bad.segments[last]=std::make_shared<Bytes>(*bad.segments.at(last));
 (*bad.segments[last])[16]^=1;
 const auto corrupt=SaveGptImage(root+"\\bad backup GPT.img",bad);
 Reject([&]{MacBrowser::OpenImage(corrupt);},"GPT image accepted a corrupt backup header");

 bad=layout;
 auto overlapping=table;
 Little(overlapping.data()+40,1024*1024/sector,8);
 UpdateGptTable(bad,overlapping);
 const auto overlap=SaveGptImage(root+"\\overlapping GPT.img",bad);
 Reject([&]{MacBrowser::OpenImage(overlap);},"GPT image accepted overlapping partitions");

 bad=layout;
 auto undersized=table;
 Little(undersized.data()+128+40,1024*1024/sector+7,8);
 UpdateGptTable(bad,undersized);
 const auto shortPartition=SaveGptImage(root+"\\short partition GPT.img",bad);
 Reject([&]{MacBrowser::OpenImage(shortPartition);},"HFS+ geometry escaped the declared partition");

 auto locked=table;
 Little(locked.data()+128+48,1ULL<<60,8);
 bad=layout;
 UpdateGptTable(bad,locked);
 const auto readOnly=SaveGptImage(root+"\\read-only GPT.img",bad);
 {
  const auto volume=MacBrowser::OpenBrowserImage(readOnly);
  Check(volume&&!volume->Writable(),"GPT read-only partition flag was ignored");
  Verify(*volume);
  Reject([&]{volume->MakeDirectory(2,u"Forbidden");},"Read-only partition allowed a catalog write");
 }
 Fake large;
 const auto gpt4=Gpt(large,hfsx,4096,(4ULL<<30)+1024*1024);
 const auto largePath=SaveGptImage(root+"\\GPT 4096 large.img",*gpt4);
 {
  const auto volume=MacBrowser::OpenBrowserImage(largePath);
  Check(volume&&volume->Writable()&&volume->CaseSensitive()&&!strcmp(volume->Kind(),"HFSX"),
        "4096-byte GPT partition beyond 4 GiB did not retain HFSX case mode");
  Verify(*volume);
  volume->MakeDirectory(2,u"Above 4 GiB");
 }
 {
  const auto volume=MacBrowser::OpenImage(largePath);
  Check(volume!=nullptr,"Reopen the large-sector GPT image");
  Find(*volume,2,u"Above 4 GiB");
 }
 return path;
}
void Pump(){MSG m;while(PeekMessage(&m,nullptr,0,0,PM_REMOVE))if(m.message!=WM_QUIT){TranslateMessage(&m);DispatchMessage(&m);}}
FileOperations::Operation Copy(std::shared_ptr<MacBrowser::Volume> source,std::shared_ptr<MacBrowser::Volume> target,bool success=true){FileOperations::Request r;r.background=true;FileOperations::Location from,to;from.SetMounted(source,Find(*source,2,u"Hello"));to.SetMounted(target,2);r.Copy().sources={from};r.Copy().destination=to;auto op=FileOperations::Submit(r);auto start=GetTickCount();while(!op.Job()->done()){if(!op.Job()->conflict().empty())op.Job()->resolve(Storage::ConflictChoice::Replace);Check(GetTickCount()-start<15000,"Removable copy timed out");Pump();Sleep(1);}Check((op.Job()->result().code==0)==success,op.Job()->result().message.c_str());return op;}
CString browserHost;
class RemovableDoc:public CHFVExplorerDoc{DECLARE_DYNCREATE(RemovableDoc)
public:RemovableDoc():CHFVExplorerDoc(false){Mounts().AddWindows(browserHost[0],DRIVE_FIXED,browserHost);}};
IMPLEMENT_DYNCREATE(RemovableDoc,CHFVExplorerDoc)
class RemovableFrame:public CMainFrame{DECLARE_DYNCREATE(RemovableFrame)
public:void LoadWindowPosition()override{}void SaveWindowPosition()override{}void ActivateFrame(int)override{}};
IMPLEMENT_DYNCREATE(RemovableFrame,CMainFrame)
unsigned MacRoots(BrowserSession& session){unsigned count=0;auto& tree=session.Tree()->GetTreeCtrl();for(auto item=tree.GetRootItem();item;item=tree.GetNextSiblingItem(item)){const auto* node=session.Tree()->BrowserNodeAt(item);if(node&&node->mount.kind!=BrowserMountInventory::Kind::WindowsDrive)++count;}return count;}
int Row(BrowserSession& session,const wchar_t* name){auto& list=session.List()->GetListCtrl();for(int row=0;row<list.GetItemCount();++row){wchar_t text[512]{};LVITEMW item{};item.pszText=text;item.cchTextMax=ARRAYSIZE(text);SendMessageW(list.m_hWnd,LVM_GETITEMTEXTW,row,reinterpret_cast<LPARAM>(&item));if(!wcscmp(text,name))return row;}throw HfsPlus::Error("Missing removable browser row");}
template<class Predicate>
void Until(Predicate ready,const char* error)
{
 const auto start=GetTickCount();
 while(!ready()){
  Check(GetTickCount()-start<10000,error);
  Pump();Sleep(1);
 }
}
unsigned timerTicks=0;
void CALLBACK Tick(HWND,UINT,UINT_PTR,DWORD){++timerTicks;}
void OnWorker(std::function<void()> work){auto job=Storage::worker().submit([&](Storage::Job&){work();return Storage::Result{};});job->wait();Check(!job->result().code,job->result().message.c_str());}
void BrowserChecks(const std::string& root,std::shared_ptr<Bytes> plus,std::shared_ptr<Bytes> classic,const std::wstring& gptImage){
 auto api=std::make_shared<Fake>();auto device=api->Add(L"UI USB",plus,BusTypeUsb,false);api->Add(L"UI Zip",classic,BusTypeScsi,true);api->workerOnly=true;Removable::SetTestApi(api);browserHost=root.c_str();auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());app.m_startup_hds=FALSE;app.m_startup_floppies=app.m_startup_cds=FALSE;app.m_dosicons=DI_NO_ICONS;app.m_mac_window_mode=FALSE;app.m_show_invisible_mac=app.m_show_invisible_fat=TRUE;app.InstallBrowserTemplate(RUNTIME_CLASS(RemovableDoc),RUNTIME_CLASS(RemovableFrame));auto a=app.CreateBrowser(FALSE),b=app.CreateBrowser(FALSE);Check(a&&b&&!a->IsWindowVisible()&&!b->IsWindowVisible(),"Hidden removable browser creation");auto& sa=a->GetBrowserSession();auto& sb=b->GetBrowserSession();Check(api->enumerations==0&&MacRoots(sa)==0,"Disabled removable startup option still scanned disks");
 {COptionsPage1 page;Check(page.Create(IDD_OPTIONS_PAGE_1,a)!=FALSE&&page.GetDlgItem(IDC_OPTIONS_STARTUP_HDS)->IsWindowEnabled(),"Other Mac media startup control disabled");page.m_startup_hds=TRUE;page.UpdateData(FALSE);Check(page.IsDlgButtonChecked(IDC_OPTIONS_STARTUP_HDS)==BST_CHECKED,"Other Mac media checkbox binding");page.DestroyWindow();}
 Check(sa.AwaitNavigationForTest()&&sa.MacBrowser().TargetFolder().IsWindows(),"load initial Windows folder before testing scan overlap");
 api->gate=true;
 a->SendMessage(WM_COMMAND,ID_FILE_SCAN_REMOVABLE);
 Until([&]{return WaitForSingleObject(api->entered,0)==WAIT_OBJECT_0;},"manual scan did not reach the worker");
 Check(a->IsWindowEnabled()&&MacRoots(sa)==0&&MacRoots(sb)==0,"slow scan blocked the browser or published early");
 Check(sa.RefreshSelected(),"queue refresh of the unrelated folder during discovery");
 const bool refreshed=sa.AwaitNavigationForTest();
 Check(refreshed,sa.navigationErrorForTest.empty()?"browse an unrelated folder while detection is suspended":sa.navigationErrorForTest.c_str());
 const auto hostSelection=sa.Tree()->GetTreeCtrl().GetSelectedItem();
 MediaDiscovery::Changed(DWORD(1)<<25);
 SetEvent(api->release);
 Until([&]{return MacRoots(sa)==2;},"scan did not recover after a media-change notification");
 Check(api->enumerations==2&&MacRoots(sb)==0&&sa.Tree()->GetTreeCtrl().GetSelectedItem()==hostSelection,
       "stale scan was published, or discovery changed the active listing");
 ResetEvent(api->entered);ResetEvent(api->release);
 bool windowsRead=false;
 BrowserDirectory::testObserveRead=[&](const char* path){if(!_strnicmp(path,"Z:",2))windowsRead=true;};
 auto doc=sa.Document();
 const auto index=doc->Mounts().AddWindows('Z',DRIVE_REMOVABLE,"Z:\\");
 const auto& drive=doc->Mounts().WindowsAt(index);
 doc->GetBrowserSession().SetMountRoot({BrowserMountInventory::Kind::WindowsDrive,index},
     sa.Tree()->initialize_fat_volume(index,&drive.m_volume_name,drive.m_type,&drive.m_volume_name,FALSE));
 sa.Tree()->GetTreeCtrl().SelectItem(sa.MountRoot({BrowserMountInventory::Kind::WindowsDrive,index}));
 Check(sa.AwaitNavigationForTest(),"complete removable drive selection");
 BrowserDirectory::testObserveRead={};
 Check(!windowsRead&&sa.MacBrowser().Active()&&Row(sa,L"Hello")>=0&&!sa.MacBrowser().Writable(),
       "Explicit Mac removable drive selection fell through to Windows enumeration");
 const auto selected=sa.Tree()->GetTreeCtrl().GetSelectedItem();
 a->SendMessage(WM_COMMAND,ID_FILE_SCAN_REMOVABLE);
 Until([&]{return api->enumerations==3&&!sa.MacBrowser().MediaUpdatesPending();},"manual rescan did not finish");
 Check(sa.Tree()->GetTreeCtrl().GetSelectedItem()==selected&&
       MacRoots(sa)==2&&MacRoots(sb)==0&&Row(sa,L"Hello")>=0,
       "Manual scan replaced stable removable mounts or their selection");
 auto captured=sa.MacBrowser().CaptureItem(Row(sa,L"Hello"));OnWorker([&]{Verify(*captured.Volume());++device->generation;Reject([&]{captured.Volume()->List();},"Cached physical catalog survived media swap");});Check(sa.MacBrowser().Refresh()&&sa.AwaitNavigationForTest()&&Row(sa,L"Hello")>=0,"Physical refresh did not reopen changed media");OnWorker([&]{Reject([&]{captured.Volume()->Stat(captured.Id());},"Old clipboard source rebound to replaced media");});captured={};
 app.m_startup_hds=TRUE;
 auto c=app.CreateBrowser(FALSE),d=app.CreateBrowser(FALSE);
 Check(c&&d,"create two browsers with startup scans");
 auto& sc=c->GetBrowserSession();
 auto& sd=d->GetBrowserSession();
 Until([&]{return MacRoots(sc)==2&&MacRoots(sd)==2;},"startup scans did not discover the same media in both browsers");
 Check(sc.AwaitNavigationForTest()&&sd.AwaitNavigationForTest()&&
       sc.MacBrowser().TargetFolder().IsWindows()&&sd.MacBrowser().TargetFolder().IsWindows(),
       "startup discovery switched one of the browsers to a Mac root");
 c->DestroyWindow();d->DestroyWindow();app.m_startup_hds=FALSE;
 auto path=VolumeCoord::Wide(root+"\\ui-target.dsk");{auto s=HfsPlus::CreateImage(path.c_str(),16*1024*1024);HfsPlus::Volume::Format(*s,u"Copy target",true);}auto target=MacBrowser::OpenImage(path);int targetIndex=sb.MacBrowser().AttachTestVolume(target,"target");Check(sb.MacBrowser().ShowTestFolder(targetIndex,2),"Show target window");auto unrelatedPath=VolumeCoord::Wide(root+"\\unrelated.dsk");{auto s=HfsPlus::CreateImage(unrelatedPath.c_str(),8*1024*1024);HfsPlus::Volume::Format(*s,u"Unrelated",true);}auto unrelated=MacBrowser::OpenImage(unrelatedPath);auto unrelatedIndex=sb.MacBrowser().AttachTestVolume(unrelated,"unrelated");Check(sb.MacBrowser().ShowTestFolder(unrelatedIndex,2),"Show unrelated browser volume");
 {auto from=sa.MacBrowser().CaptureItem(Row(sa,L"Hello"));FileOperations::Request r;r.background=true;r.Copy().sources={from};r.Copy().destination.SetMounted(target,2);api->gate=true;auto op=FileOperations::Submit(r);auto start=GetTickCount();while(WaitForSingleObject(api->entered,0)!=WAIT_OBJECT_0){Pump();Sleep(1);Check(GetTickCount()-start<10000,"Wait for simulated removable spin-up");}a->DestroyWindow();a=nullptr;timerTicks=0;Check(SetTimer(b->m_hWnd,917,10,Tick)!=0,"Create responsiveness timer");start=GetTickCount();while(timerTicks<2){Pump();Sleep(1);Check(GetTickCount()-start<2000,"UI stopped during removable read");}KillTimer(b->m_hWnd,917);Check(sb.MacBrowser().Refresh(),"Unrelated volume could not browse during removable read");SetEvent(api->release);start=GetTickCount();while(!op.Job()->done()){Pump();Sleep(1);Check(GetTickCount()-start<15000,"Finish detached removable copy");}Check(!op.Job()->result().code,op.Job()->result().message.c_str());Verify(*target);}
 Check(sb.MacBrowser().OpenImage(gptImage)==1&&sb.AwaitNavigationForTest()&&
       Row(sb,L"GPT edited")>=0&&sb.MacBrowser().Writable(),"Normal browser image opening missed the GPT partition");
 auto exportPath=root+"\\gpt-export";
 Check(CreateDirectoryA(exportPath.c_str(),nullptr),"Create GPT export folder");
 const auto exportTarget=MacBrowser::OpenWindows(VolumeCoord::Wide(exportPath));
 Copy(sb.MacBrowser().TargetFolder().Volume(),exportTarget);
 Verify(*exportTarget);
 Check(!api->wrongThread,"Native removable access ran on UI thread");b->DestroyWindow();BrowserTransfers::Clear();Removable::SetTestApi({});auto start=GetTickCount();while(api->Active()){Pump();Sleep(1);Check(GetTickCount()-start<3000,"Removable handles outlived browsers and jobs");}
}

}

HfsPlus::Bytes ApfsRemovableFixture(unsigned sector);
void ApfsRemovableTest(FILE* report)
{
    auto api=std::make_shared<Fake>();
    for(unsigned sector:{0,512,4096}){
        const auto label=L"APFS "+std::to_wstring(sector);
        auto disk=api->Add(label.c_str(),std::make_shared<Bytes>(ApfsRemovableFixture(sector)));
        disk->geometry.sector=sector?sector:512;
        disk->writable=true;
        auto mounted=Removable::Open(disk->device,api);
        Check(mounted.size()==1,"APFS removable partition not discovered");
        auto& volume=*mounted.front().volume;
        Check(std::string(volume.Kind())=="APFS"&&!volume.Writable(),
              "APFS on a write-capable USB disk was not read-only");
        const auto folder=volume.List().at(0);
        const auto entry=volume.List(folder.id).at(0);
        char value=0;
        volume.Read(entry.id,0,0,&value,1);
        Check(value=='A',"APFS removable file read returned wrong data");
        Reject([&]{volume.Remove(entry.id);},"APFS removable file removal succeeded");
        Check(!disk->writes&&!disk->locks,"APFS tried to open a writable device");
        ++disk->generation;
        Reject([&]{volume.List();},"APFS accepted changed physical media");
    }
    fprintf(report,"PASS: raw and GPT APFS on 512/4096-byte fake removable transports, read-only and media replacement.\n");
}
namespace {
constexpr int UndoDrive=25;
void WriteFixture(const std::string& path,uint64_t offset,const void* data,DWORD size,bool create=false)
{
    CFile file;
    Check(file.Open(path.c_str(),CFile::modeWrite|(create?CFile::modeCreate:0))!=FALSE,"open simulated media backing");
    file.Seek(offset,CFile::begin);
    file.Write(data,size);
    file.Flush();
}
void PersistPhysicalWrite(const std::string& path,uint64_t at,DWORD size,const void* data)
{
    wchar_t crash[12]{};
    if(GetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_CRASH",crash,12)&&!wcscmp(crash,L"5")){
        const auto original=Load(path);
        const auto intended=static_cast<const BYTE*>(data);
        for(DWORD i=0;i<size;++i){
            if((*original)[size_t(at+i)]==intended[i])continue;
            // Persist through the first changed byte, leaving the rest of the
            // device write incomplete when the child disappears.
            WriteFixture(path,at,data,i+1);
            RemovableUndo::TestCheckpoint(5);
        }
    }
    WriteFixture(path,at,data,size);
}
class UndoFloppy final : public FloppyWriteApi {
    std::string path_;
    FakeFloppy disk_;
    bool locked_=false;
    const int drive_;
public:
    explicit UndoFloppy(const std::string& path,int drive=UndoDrive):path_(path),disk_(*Load(path)),drive_(drive){
        Check(readonly_floppy_attach_api(drive_,this),"attach recovery floppy fixture");
    }
    ~UndoFloppy(){physical_floppy_suspend(drive_);readonly_floppy_attach_api(drive_,nullptr);}
    UINT DriveType(int drive)override{return disk_.DriveType(drive);}
    HANDLE Open(const char* p,DWORD a,DWORD s,DWORD f)override{return disk_.Open(p,a,s,f);}
    bool Geometry(HANDLE h,DISK_GEOMETRY& g)override{return disk_.Geometry(h,g);}
    bool Verify(HANDLE h,DWORD& n)override{return disk_.Verify(h,n);}
    bool Read(HANDLE h,ULONGLONG at,DWORD size,void* data,DWORD& got)override{return disk_.Read(h,at,size,data,got);}
    void Close(HANDLE h)override{disk_.Close(h);}
    bool Lock(HANDLE)override{locked_=true;return true;}
    bool Dismount(HANDLE)override{return locked_;}
    bool Unlock(HANDLE)override{locked_=false;return true;}
    bool Write(HANDLE,ULONGLONG at,DWORD size,const void* bytes,DWORD& got)override{
        Check(locked_&&B::Range(at,size,disk_.media.size())&&uintptr_t(bytes)%512==0,"unlocked or unaligned floppy write");
        PersistPhysicalWrite(path_,at,size,bytes);
        memcpy(disk_.media.data()+at,bytes,size);
        got=size;
        return true;
    }
    bool Flush(HANDLE)override{SetLastError(ERROR_INVALID_FUNCTION);return false;}
};
std::shared_ptr<MacBrowser::Volume> OpenUndoDisk(const std::string& path,bool zip)
{
    auto api=std::make_shared<Fake>();
    auto sibling=std::make_shared<Bytes>(4096,0x47);
    auto device=zip?Apm(*api,Load(path),sibling):api->Add(L"USB raw HFS",Load(path));
    const auto map=zip?*device->segments.at(0):Bytes{};
    if(zip)device->device.number=101;
    device->writable=true;
    device->persist=[path](uint64_t at,DWORD count,const void* data){PersistPhysicalWrite(path,at,count,data);};
    auto volumes=Removable::Open(device->device,api);
    if(zip)Check(*device->segments.at(0)==map&&std::all_of(sibling->begin(),sibling->end(),[](auto b){return b==0x47;}),
                 "recovery altered a neighboring partition or the partition map");
    Check(volumes.size()==1&&!strcmp(volumes.front().volume->Kind(),"HFS"),"reopen simulated removable HFS partition");
    return volumes.front().volume;
}
void DiscoverUndo(const std::string& path,const std::string& adapter,bool committed)
{
    MacBrowser::Entry entry{};
    if(adapter=="zip"||adapter=="usb"){
        Check(OpenUndoDisk(path,adapter=="zip")->FindChild(2,u"Undo folder",entry)==committed,"removable catalog recovery result differs");
        return;
    }
    UndoFloppy floppy(path);
    Storage::Job job;
    auto result=MediaDiscovery::ProbeDrive(UndoDrive,DRIVE_REMOVABLE,true,false,job);
    Check(bool(result.floppy),"floppy discovery did not reopen classic HFS");
    const bool edited=adapter=="image"?result.floppy->Name()==u"New floppy":
        result.floppy->FindChild(2,u"Undo folder",entry);
    Check(edited==committed,"floppy recovery result differs");
}
void CrashMedia(const std::string& path,const std::string& adapter,unsigned phase)
{
    wchar_t exe[MAX_PATH]{};
    const auto length=GetModuleFileNameW(nullptr,exe,MAX_PATH);
    Check(length&&length<MAX_PATH,"locate physical recovery test executable");
    auto command=L"\""+std::wstring(exe)+L"\" --removable-recovery-child \""+VolumeCoord::Wide(path)+
        L"\" "+VolumeCoord::Wide(adapter)+L" "+std::to_wstring(phase);
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child)!=FALSE,
          "start media crash subprocess");
    CloseHandle(child.hThread);
    const auto wait=WaitForSingleObject(child.hProcess,adapter.find("move-native-")==0?300000:45000);
    if(wait!=WAIT_OBJECT_0){TerminateProcess(child.hProcess,2);WaitForSingleObject(child.hProcess,5000);}
    DWORD code=0;
    GetExitCodeProcess(child.hProcess,&code);
    CloseHandle(child.hProcess);
    const unsigned expected=(adapter.find("move-")==0?90u:100u)+phase;
    if(wait!=WAIT_OBJECT_0||code!=expected)
        throw HfsPlus::Error(adapter+" missed crash phase "+std::to_string(phase)+
                             " (exit "+std::to_string(code)+")");
}
Storage::Result MoveToOrFromDisk(const std::string& path,const std::string& adapter,bool persistent)
{
    const auto imagePath=VolumeCoord::Wide(path+".image.dsk");
    if(adapter=="move-to-windows"||adapter=="move-from-windows"||adapter=="move-disk-pair"){
        const bool fromWindows=adapter=="move-from-windows",pair=adapter=="move-disk-pair";
        auto disk=OpenUndoDisk(path,false);
        const auto folder=VolumeCoord::Wide(path+".windows");
        auto from=fromWindows?FileOperations::Location::Windows(folder+L"\\Hello"):
                              FileOperations::Location::Mounted(disk,Find(*disk,2,u"Hello"));
        auto target=pair?OpenUndoDisk(path+".peer",true):fromWindows?disk:nullptr;
        auto to=target?FileOperations::Location::Mounted(target,2):FileOperations::Location::Windows(folder);
        from.cut=true;
        Storage::Job job;
        FileOperations::Completion completion;
        return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{{from},to},job,completion);
    }
    auto disk=OpenUndoDisk(path,adapter.find("zip")!=std::string::npos);
    auto image=persistent?MacBrowser::OpenBrowserImage(imagePath):MacBrowser::OpenImage(imagePath);
    Check(image&&image->Writable(),"open writable image for a coordinated removable move");
    const bool toDisk=adapter.find("move-to-")==0;
    auto source=toDisk?image:disk;
    auto target=toDisk?disk:image;
    MacBrowser::Entry entry{};
    Check(source->FindChild(2,u"Hello",entry),"coordinated removable move source is missing");
    auto from=FileOperations::Location::Mounted(source,entry.id);
    from.cut=true;
    Storage::Job job;
    FileOperations::Completion completion;
    return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{
        {from},FileOperations::Location::Mounted(target,2)},job,completion);
}

void VerifyMovedPayload(MacBrowser::Volume& volume,const MacBrowser::Entry& entry)
{
    for(unsigned fork:{0u,1u}){
        Bytes bytes(fork?723:5003),expected(bytes.size());
        Check(entry.forks[fork].size==bytes.size(),"coordinated removable move changed a fork size");
        Payload(fork,0,expected.data(),expected.size());
        volume.Read(entry.id,fork,0,bytes.data(),bytes.size());
        Check(bytes==expected,"coordinated removable move changed a fork");
    }
}

void CheckCoordinatedDiskMoves(const std::string& root,const Bytes& populated,FILE* report)
{
    Check(SetEnvironmentVariableW(L"HFV_TEST_CLASSIC_MOVE_DIR",VolumeCoord::Wide(root).c_str())!=FALSE,
          "set isolated shared decision directory for removable moves");
    const auto seed=root+"\\move-empty.dsk";
    WriteFixture(seed,0,populated.data(),DWORD(populated.size()),true);
    {
        auto volume=MacBrowser::OpenImage(VolumeCoord::Wide(seed));
        MacBrowser::Entry entry{};
        Check(volume&&volume->FindChild(2,u"Hello",entry),"open removable-move fixture");
        volume->Remove(entry.id);
        volume->Close();
    }
    const auto empty=Load(seed);
    for(const std::string adapter:{"move-to-zip","move-from-usb"}){
        const bool toDisk=adapter=="move-to-zip";
        const auto key=toDisk?L"Zip APM":L"USB raw HFS";
        const auto& oldDisk=toDisk?*empty:populated;
        const auto& oldImage=toDisk?populated:*empty;
        for(unsigned phase:{8u,9u,0u}){
            const auto path=root+"\\"+adapter+"-"+std::to_string(phase)+".dsk";
            const auto imageFile=path+".image.dsk";
            const auto imagePath=VolumeCoord::Wide(imageFile);
            WriteFixture(path,0,oldDisk.data(),DWORD(oldDisk.size()),true);
            WriteFixture(imageFile,0,oldImage.data(),DWORD(oldImage.size()),true);
            if(phase){
                CrashMedia(path,adapter,phase);
                Check(RemovableUndo::Pending(key)&&ClassicHfsRecovery::Pending(imagePath),
                      "interrupted removable move did not keep both participant logs");
                if(phase==8){
                    const auto interrupted=Load(path);
                    auto wrong=oldDisk;
                    wrong.back()^=0x5a;
                    WriteFixture(path,0,wrong.data(),DWORD(wrong.size()),true);
                    Reject([&]{OpenUndoDisk(path,toDisk);},"coordinated move replay accepted a replacement disk");
                    Check(*Load(path)==wrong&&RemovableUndo::Pending(key)&&ClassicHfsRecovery::Pending(imagePath),
                          "replacement disk or pending participant was modified");
                    WriteFixture(path,0,interrupted->data(),DWORD(interrupted->size()),true);
                }
            }else{
                const auto result=MoveToOrFromDisk(path,adapter,true);
                if(result.code||!result.moved)throw HfsPlus::Error("Persistent removable move: "+result.message);
            }
            {
                auto disk=OpenUndoDisk(path,toDisk);
                auto image=MacBrowser::OpenBrowserImage(imagePath);
                Check(image&&disk->Writable()&&image->Writable(),"both move participants must reopen writable");
                const bool onDisk=(phase==8)?!toDisk:toDisk;
                MacBrowser::Entry diskEntry{},imageEntry{};
                Check(disk->FindChild(2,u"Hello",diskEntry)==onDisk&&
                      image->FindChild(2,u"Hello",imageEntry)!=onDisk,
                      "removable disk and image chose different move outcomes");
                VerifyMovedPayload(onDisk?*disk:*image,onDisk?diskEntry:imageEntry);
            }
            Check(!RemovableUndo::Pending(key)&&!ClassicHfsRecovery::Pending(imagePath),
                  "reopening a coordinated move left a pending participant");
            if(phase==8)Check(*Load(path)==oldDisk&&*Load(imageFile)==oldImage,
                              "aborted removable move changed the exact image bytes");
        }
    }
    fprintf(report,"PASS: image-to-Zip and USB-to-image moves agree before and after the shared decision, preserve both forks, and complete through a persistent image worker.\n");
    for(const std::string adapter:{"move-to-windows","move-from-windows","move-disk-pair"}){
        const bool fromWindows=adapter=="move-from-windows",pair=adapter=="move-disk-pair";
        const auto& initial=fromWindows?*empty:populated;
        for(const unsigned phase:{8U,9U,0U}){
            const auto path=root+"\\"+adapter+"-"+std::to_string(phase)+".dsk";
            WriteFixture(path,0,initial.data(),DWORD(initial.size()),true);
            if(pair)WriteFixture(path+".peer",0,empty->data(),DWORD(empty->size()),true);
            const auto folder=VolumeCoord::Wide(path+".windows");
            if(!pair){
                Check(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE,"create NTFS removable move fixture");
                if(fromWindows){
                    auto host=MacBrowser::OpenWindows(folder);
                    MacBrowser::Entry entry;
                    entry.name=u"Hello";entry.forks[0].size=5003;entry.forks[1].size=723;
                    host->Put(2,entry,[](unsigned fork,uint64_t at,void* data,size_t n){Payload(fork,at,data,n);},false);
                }
            }
            if(phase){
                CrashMedia(path,adapter,phase);
                Check(RemovableUndo::Pending(L"USB raw HFS"),"interrupted move lost the disk recovery journal");
                const auto interrupted=Load(path);
                auto foreign=initial;
                foreign.back()^=0x5a;
                WriteFixture(path,0,foreign.data(),DWORD(foreign.size()),true);
                Reject([&]{OpenUndoDisk(path,false);},"paired recovery accepted the wrong removable disk");
                Check(*Load(path)==foreign&&RemovableUndo::Pending(L"USB raw HFS"),"wrong disk was changed or recovery was lost");
                if(!pair){
                    MacBrowser::Entry hostEntry;
                    auto host=MacBrowser::OpenWindows(folder);
                    Check(host->FindChild(2,u"Hello",hostEntry)==!fromWindows,
                          "rejecting a replacement disk changed the Windows side");
                }
                WriteFixture(path,0,interrupted->data(),DWORD(interrupted->size()),true);
            }else{
                const auto result=MoveToOrFromDisk(path,adapter,false);
                if(result.code||!result.moved)throw HfsPlus::Error("Paired removable move: "+result.message);
            }
            auto disk=OpenUndoDisk(path,false);
            auto peer=pair?OpenUndoDisk(path+".peer",true):MacBrowser::OpenWindows(folder);
            const bool onDisk=(phase==8)?!fromWindows:fromWindows;
            MacBrowser::Entry actual;
            Check(disk->FindChild(2,u"Hello",actual)==onDisk&&peer->FindChild(2,u"Hello",actual)!=onDisk,
                  "paired removable recovery retained the file on the wrong endpoint");
            auto& owner=onDisk?*disk:*peer;
            VerifyMovedPayload(owner,owner.Stat(Find(owner,2,u"Hello")));
            Check(!RemovableUndo::Pending(L"USB raw HFS")&&(!pair||!RemovableUndo::Pending(L"Zip APM")),
                  "paired recovery did not clear both device journals");
        }
    }
    fprintf(report,"PASS: NTFS and separate removable disks share a verified move decision; swapped media is refused before and after commit.\n");
}

constexpr char NativeTestName[]="HFV Move Probe";
class NativeFloppyAttachment {
public:
    NativeFloppyAttachment(){
        Check(readonly_floppy_attach_api(0,&NativeFloppyWriteApiForTest()),"attach native A: for the diagnostic");
    }
    ~NativeFloppyAttachment(){
        physical_floppy_suspend(0);
        readonly_floppy_attach_api(0,nullptr);
    }
};

Storage::Result MoveDriveFloppy(const std::string& file,bool toFloppy,unsigned phase,
                                const std::string& peer={},bool native=false)
{
    constexpr int drive=0;
    std::unique_ptr<UndoFloppy> simulated;
    std::unique_ptr<NativeFloppyAttachment> hardware;
    if(native)hardware=std::make_unique<NativeFloppyAttachment>();
    else simulated=std::make_unique<UndoFloppy>(file,drive);
    ReadOnlyFloppy selected(physical_floppy_test_api(drive));
    Check(selected.OpenDrive(drive),"capture A: generation");
    const auto generation=selected.Generation();
    selected.Close();
    const std::string name=native?NativeTestName:"Hello";
    auto mac=FileOperations::Location::Physical(drive,FileOperations::ClassicMedia::Floppy,toFloppy?":":":"+name);
    mac.BindMediaGeneration(generation);
    const auto folder=VolumeCoord::Wide(file+".windows");
    auto windows=FileOperations::Location::Windows(toFloppy?folder+L"\\"+VolumeCoord::Wide(name):folder);
    auto from=toFloppy?windows:mac;
    auto dest=toFloppy?mac:windows;
    std::shared_ptr<MacBrowser::Volume> volume;
    if(!peer.empty()){
        volume=peer=="zip"?OpenUndoDisk(file+".peer",true):MacBrowser::OpenImage(VolumeCoord::Wide(file+".peer"));
        if(toFloppy)from=FileOperations::Location::Mounted(volume,Find(*volume,2,u"Hello"));
        else dest=FileOperations::Location::Mounted(volume,2);
    }
    from.cut=true;
    Storage::Job job;
    job.transport({},[](const std::string&){return Storage::ConflictChoice::Replace;});
    if(phase==7)MacBrowser::windowsPublicationFault=[](const char* step){
        if(!strcmp(step,"data-published"))ExitProcess(97);
    };
    FileOperations::Completion completion;
    if(toFloppy&&peer.empty())
        return FileOperations::ExecuteInWorker(FileOperations::LegacyConversion{
            from,dest,false,true,false,LegacyCopyOptions{},FileOperations::ConversionTarget::Folder},job,completion);
    return FileOperations::ExecuteInWorker(FileOperations::OrdinaryCopy{{from},dest},job,completion);
}

void CheckDriveFloppyMoves(const std::string& root,const Bytes& fixture,FILE* report)
{
    constexpr int drive=0;
    const auto key=RemovableUndo::FloppyKey(drive);
    const auto put=[](MacBrowser::Volume& volume){
        MacBrowser::Entry entry;
        entry.name=u"Hello";entry.forks[0].size=5003;entry.forks[1].size=723;
        memcpy(entry.finder.data(),"TEXTTEST",8);
        volume.Put(2,entry,[](unsigned fork,uint64_t at,void* data,size_t n){Payload(fork,at,data,n);},false);
    };
    for(bool toFloppy:{false,true}){
        const auto adapter=toFloppy?"move-to-drive-floppy":"move-from-drive-floppy";
        for(const unsigned phase:{0U,7U,8U,9U}){
            if(toFloppy&&phase==7)continue;
            const auto file=root+"\\"+adapter+"-"+std::to_string(phase)+".dsk";
            const auto folder=VolumeCoord::Wide(file+".windows");
            WriteFixture(file,0,fixture.data(),DWORD(fixture.size()),true);
            Check(CreateDirectoryW(folder.c_str(),nullptr)!=FALSE,"create A: move's NTFS folder");
            if(toFloppy)put(*MacBrowser::OpenWindows(folder));
            else put(*MacBrowser::OpenImage(VolumeCoord::Wide(file)));
            const auto original=Load(file);

            if(!phase){
                const auto result=MoveDriveFloppy(file,toFloppy,phase);
                if(result.code||!result.moved)throw HfsPlus::Error("Normal drive-letter floppy move failed: "+result.message);
            }else{
                CrashMedia(file,adapter,phase);
                Check(RemovableUndo::Pending(key),"interrupted A: move did not retain its decision");
                const auto interrupted=Load(file);
                if(phase==7)Check(*interrupted==*original,"NTFS published after the floppy had already been modified");
                const auto companion=folder+L"\\%Hello";
                const auto hostData=folder+L"\\Hello";
                const auto fileExists=[](const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;};
                const bool hasData=fileExists(hostData),hasCompanion=fileExists(companion);
                auto other=*original;
                other.back()^=0x20;
                WriteFixture(file,0,other.data(),DWORD(other.size()),true);
                {
                    UndoFloppy swapped(file,drive);
                    bool refused=false;
                    try{RecoverFloppyWrite(drive);}catch(const Storage::MediaChanged&){refused=true;}
                    Check(refused,"A: recovery did not explicitly refuse the swapped media");
                }
                Check(*Load(file)==other&&RemovableUndo::Pending(key)&&
                      fileExists(hostData)==hasData&&fileExists(companion)==hasCompanion,
                      "recovery changed the swapped disk or NTFS files");
                WriteFixture(file,0,interrupted->data(),DWORD(interrupted->size()),true);
            }
            {
                UndoFloppy restored(file,drive);
                Storage::Job job;
                const auto found=MediaDiscovery::ProbeDrive(drive,DRIVE_REMOVABLE,true,false,job);
                auto host=MacBrowser::OpenWindows(folder);
                Check(bool(found.floppy)&&!RemovableUndo::Pending(key),"A: discovery did not complete move recovery");
                const bool committed=phase==0||phase==9;
                const bool onDisk=committed==toFloppy;
                MacBrowser::Entry diskEntry,hostEntry;
                Check(found.floppy->FindChild(2,u"Hello",diskEntry)==onDisk&&
                      host->FindChild(2,u"Hello",hostEntry)!=onDisk,"A: and NTFS disagreed on the move decision");
                auto& entry=onDisk?diskEntry:hostEntry;
                VerifyMovedPayload(onDisk?*found.floppy:*host,entry);
                Check(!memcmp(entry.finder.data(),"TEXTTEST",8),"A: move did not preserve Finder type and creator");
                if(!committed)Check(*Load(file)==*original,"A: rollback did not restore exact original sectors");
            }
        }
    }
    for(const std::string peer:{"image","zip"}){
        for(bool toFloppy:{false,true}){
            const auto adapter=std::string("move-drive-")+(toFloppy?"in-":"out-")+peer;
            for(const unsigned phase:{8U,9U}){
                const auto file=root+"\\"+adapter+"-"+std::to_string(phase)+".dsk";
                WriteFixture(file,0,fixture.data(),DWORD(fixture.size()),true);
                WriteFixture(file+".peer",0,fixture.data(),DWORD(fixture.size()),true);
                put(*MacBrowser::OpenImage(VolumeCoord::Wide(toFloppy?file+".peer":file)));

                CrashMedia(file,adapter,phase);
                Check(RemovableUndo::Pending(key),"direct floppy journal was lost");
                UndoFloppy restored(file,drive);
                Storage::Job job;
                const auto found=MediaDiscovery::ProbeDrive(drive,DRIVE_REMOVABLE,true,false,job);
                auto other=peer=="zip"?OpenUndoDisk(file+".peer",true):MacBrowser::OpenImage(VolumeCoord::Wide(file+".peer"));
                Check(bool(found.floppy)&&!RemovableUndo::Pending(key),"direct floppy recovery did not complete");
                const bool onFloppy=(phase==9)==toFloppy;
                MacBrowser::Entry first,second;
                Check(found.floppy->FindChild(2,u"Hello",first)==onFloppy&&
                      other->FindChild(2,u"Hello",second)!=onFloppy,"floppy and HFS peer disagreed on commit");
                VerifyMovedPayload(onFloppy?*found.floppy:*other,onFloppy?first:second);
            }
        }
    }
    fprintf(report,"PASS: A: moves with classic HFS images and simulated Zip media recover before and after commit.\n");
    fprintf(report,"PASS: dedicated A: floppy and NTFS moves recover before and after commit, including a crash before the first floppy write; both forks and Finder metadata survive, and swapped disks leave both sides untouched.\n");
}

void CheckDurableMedia(const std::string& root,FILE* report)
{
    Check(SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_UNDO_DIR",VolumeCoord::Wide(root).c_str())!=FALSE,
          "set isolated physical-media journal directory");
    auto zip=Classic(root+"\\zip-seed.dsk");
    Bytes floppy;
    const auto fixture=_wgetenv(L"HFV_CLASSIC_FLOPPY_FIXTURE");
    if(fixture&&*fixture){
        const auto local=root+"\\floppy-seed.dsk";
        Check(CopyFileW(fixture,VolumeCoord::Wide(local).c_str(),TRUE)!=FALSE,"copy shared floppy fixture");
        floppy=*Load(local);
    }else{
        std::string error;
        Check(PrepareFloppyHfs("Original floppy",floppy,error),"create classic floppy fixture");
    }
    for(const std::string adapter:{"floppy","zip","usb","image"}){
        const bool floppyPath=adapter=="floppy"||adapter=="image";
        const auto key=floppyPath?RemovableUndo::FloppyKey(UndoDrive):
            std::wstring(adapter=="zip"?L"Zip APM":L"USB raw HFS");
        const auto& before=floppyPath?floppy:*zip;
        for(unsigned phase:{1u,5u,4u,2u}){
            const auto path=root+"\\"+adapter+"-"+std::to_string(phase)+".dsk";
            WriteFixture(path,0,before.data(),DWORD(before.size()),true);
            CrashMedia(path,adapter,phase);
            const auto interrupted=*Load(path);
            Check(RemovableUndo::Pending(key)&&interrupted!=before,"interrupted write did not retain an undo log");
            if(phase==4){
                auto wrong=before;
                wrong.back()^=0x5a;
                WriteFixture(path,0,wrong.data(),DWORD(wrong.size()),true);
                Reject([&]{DiscoverUndo(path,adapter,false);},"same-geometry replacement disk was accepted");
                Check(*Load(path)==wrong&&RemovableUndo::Pending(key),"replacement media or pending log changed");
                WriteFixture(path,0,interrupted.data(),DWORD(interrupted.size()),true);
                CrashMedia(path,adapter,3);
                Check(RemovableUndo::Pending(key),"interrupted recovery cleared the undo log");
            }
            DiscoverUndo(path,adapter,phase==2);
            Check(!RemovableUndo::Pending(key),"successful recovery retained a pending log");
            if(phase!=2)Check(*Load(path)==before,"physical recovery did not restore all original bytes");
        }
    }
    fprintf(report,"PASS: classic floppy, partitioned Zip, raw USB disk and image-to-floppy writes survive process loss, torn writes, interrupted replay and a lost commit reply; replacement disks are refused without writes.\n");
    CheckCoordinatedDiskMoves(root,*zip,report);
    CheckDriveFloppyMoves(root,floppy,report);
}
}

int RunRemovableRecoveryChild(const char* target,const char* adapter,unsigned phase)
{
    try{
        SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_CRASH",std::to_wstring(phase).c_str());
        readonly_cd_backend_init();
        if(phase==3){DiscoverUndo(target,adapter,false);return 3;}
        if(strncmp(adapter,"move-",5)==0){
            ClassicHfsRecovery::Transaction::CrashAt(phase);
            const bool toFloppy=!strcmp(adapter,"move-to-drive-floppy");
            const bool fromFloppy=!strcmp(adapter,"move-from-drive-floppy");
            const std::string route=adapter;
            const bool otherPeer=route.find("move-drive-")==0;
            const bool native=route=="move-native-in"||route=="move-native-out";
            if(native){
                const std::string endpoint=target;
                const auto root=endpoint.substr(0,endpoint.find_last_of("\\/"));
                const auto journal=_wgetenv(L"HFV_TEST_REMOVABLE_UNDO_DIR");
                Check((phase==8||phase==9)&&journal&&VolumeCoord::Wide(root)==journal&&
                      endpoint==root+"\\transfer"&&ValidateFloppyHfs(*Load(root+"\\before.dsk")),
                      "Native child requires the isolated hardware test directory and verified backup");
            }
            const auto result=native?MoveDriveFloppy(target,route=="move-native-in",phase,{},true):
                otherPeer?MoveDriveFloppy(target,route.find("move-drive-in-")==0,phase,
                                                       route.substr(route.find_last_of('-')+1)):
                toFloppy||fromFloppy?MoveDriveFloppy(target,toFloppy,phase):MoveToOrFromDisk(target,adapter,false);
            if(result.code)throw HfsPlus::Error(result.message);
            return 3;
        }
        if(!strcmp(adapter,"zip")||!strcmp(adapter,"usb"))OpenUndoDisk(target,!strcmp(adapter,"zip"))->MakeDirectory(2,u"Undo folder");
        else{
            UndoFloppy floppy(target);
            if(!strcmp(adapter,"image")){
                Bytes image;
                std::string error;
                Check(PrepareFloppyHfs("New floppy",image,error),"prepare image-to-floppy source");
                FloppyWriter writer(&floppy);
                if(!writer.WriteImage(UndoDrive,image))throw HfsPlus::Error(writer.Diagnostics());
            }else{
                RunFloppyWrite(UndoDrive,{},[]{
                    char path[MAX_PATH];
                    get_floppy_volume_file_name(UndoDrive,path);
                    auto volume=MacBrowser::OpenClassic(path,":",true);
                    volume->MakeDirectory(2,u"Undo folder");
                    volume->Close();
                });
            }
        }
        return 3;
    }catch(const std::exception& e){
        FILE* log=nullptr;
        if(!fopen_s(&log,(std::string(target)+".error.txt").c_str(),"w")&&log){
            fprintf(log,"%s\n",e.what());
            fclose(log);
        }
        return 4;
    }
}

int RunRemovableRecoveryTest(const char* directory)
{
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const std::string root=directory;
    FILE* report=nullptr;
    if(fopen_s(&report,(root+"\\removable-recovery-report.txt").c_str(),"w")||!report)return 2;
    int result=0;
    try{
        readonly_cd_backend_init();
        CheckDurableMedia(root,report);
    }catch(const std::exception& e){
        fprintf(report,"FAIL: %s\n",e.what());
        result=1;
    }
    fclose(report);
    return result;
}

// Explicit opt-in hardware diagnostic, with the backup as the write precondition.
int RunNativeFloppyRecoveryCheck(const char* directory,const char* phase)
{
    constexpr int drive=0;
    FILE* report=nullptr;
    const std::string root=directory;
    if(fopen_s(&report,(root+"\\native-"+phase+".txt").c_str(),"w")||!report)return 2;
    int result=0;
    try{
        const auto before=Load(root+"\\before.dsk");
        Check(before->size()==FloppyRecovery::Range.length,"The original A: backup must be exactly 1,474,560 bytes");
        const auto key=RemovableUndo::FloppyKey(drive);
        Check(SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_UNDO_DIR",VolumeCoord::Wide(root).c_str())!=FALSE,
              "Cannot select isolated hardware test journal");
        SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_CRASH",nullptr);
        auto& api=NativeFloppyWriteApiForTest();
        const auto read=[&](Bytes& contents){
            ReadOnlyFloppy floppy(&api);
            if(!floppy.OpenDrive(drive)||floppy.MediaSize()!=contents.size()||
               !floppy.ReadBytes(0,DWORD(contents.size()),contents.data()))
                throw HfsPlus::Error("Cannot read A: (Windows error "+std::to_string(floppy.Error())+")");
        };
        if(!strcmp(phase,"interrupt")){
            Check(!RemovableUndo::Pending(key),"An interrupted hardware test is already pending");
            readonly_cd_backend_init();
            Bytes intended;
            std::string error;
            Check(PrepareFloppyHfs("Recovery test",intended,error),"Cannot prepare the classic HFS test image");
            Check(SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_CRASH",L"1")!=FALSE,"Cannot arm hardware crash point");
            FloppyWriter writer(&api);
            if(!writer.WriteImage(drive,intended,nullptr,nullptr,before.get()))
                throw HfsPlus::Error(writer.Diagnostics());
            throw HfsPlus::Error("The write did not reach the expected crash checkpoint");
        }
        Check(!strcmp(phase,"replay"),"Unknown native floppy diagnostic phase");
        Check(RemovableUndo::Pending(key),"The interrupted write left no recovery journal");
        {
            ReadOnlyFloppy floppy(&api);
            Check(floppy.OpenDrive(drive)&&floppy.MediaSize()==before->size(),"Cannot inspect interrupted floppy");
            BYTE firstTrack[18*512]{};
            Check(floppy.ReadBytes(0,sizeof(firstTrack),firstTrack),"Cannot read the interrupted floppy track");
            Check(memcmp(firstTrack,before->data(),sizeof(firstTrack))!=0,"No interrupted track write was observed");
        }
        {
            DirectFloppy recovery(&api);
            if(!recovery.Open(drive,false,true))
                throw HfsPlus::Error("Cannot lock A: for recovery (Windows error "+std::to_string(recovery.Error())+")");
        }
        Bytes restored(before->size());
        read(restored);
        Check(restored==*before&&!RemovableUndo::Pending(key),
              "Hardware replay did not restore the original sectors or left the journal pending");
        WriteFixture(root+"\\after.dsk",0,restored.data(),DWORD(restored.size()),true);
        fprintf(report,"PASS: A: native track write was observed after process loss. A fresh process replayed its undo log and verified all 1,474,560 bytes against the original backup.\n");
    }catch(const std::exception& error){
        fprintf(report,"FAIL: %s\n",error.what());
        result=1;
    }
    fclose(report);
    return result;
}

int RunNativeFloppyMoveTest(const char* directory)
{
    // The explicit command may write A:. All test paths and journals live in
    // the newly created directory and the original image is saved first.
    if(!CreateDirectoryA(directory,nullptr))return 2;
    const std::string root=directory;
    FILE* report=nullptr;
    if(fopen_s(&report,(root+"\\native-floppy-moves.txt").c_str(),"w")||!report)return 2;
    const auto step=[&](const char* message){fprintf(report,"%s\n",message);fflush(report);};
    auto& api=NativeFloppyWriteApiForTest();
    const auto read=[&]{
        Bytes bytes(FloppyRecovery::Range.length);
        ReadOnlyFloppy reader(&api);
        Check(reader.OpenDrive(0)&&reader.MediaSize()==bytes.size()&&
              reader.ReadBytes(0,DWORD(bytes.size()),bytes.data()),"Cannot back up or read back A:");
        return bytes;
    };
    const auto key=RemovableUndo::FloppyKey(0);
    Bytes backup,lastVerified;
    bool altered=false;
    int result=0;
    try{
        // Check the application's regular journal before choosing an isolated one.
        const auto appdata=_wgetenv(L"LOCALAPPDATA");
        Check(appdata&&*appdata,"Cannot locate the application's recovery data");
        const auto appRecovery=std::wstring(appdata)+L"\\HFVExplorer";
        Check(SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_UNDO_DIR",appRecovery.c_str())!=FALSE&&
              !RemovableUndo::Pending(key),"Resolve the application's pending A: journal first");
        const auto scratch=VolumeCoord::Wide(root);
        Check(SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_UNDO_DIR",scratch.c_str())!=FALSE&&
              SetEnvironmentVariableW(L"HFV_TEST_CLASSIC_MOVE_DIR",scratch.c_str())!=FALSE,
              "Cannot isolate the native-move journals");
        SetEnvironmentVariableW(L"HFV_TEST_REMOVABLE_CRASH",nullptr);
        readonly_cd_backend_init();
        backup=read();
        WriteFixture(root+"\\before.dsk",0,backup.data(),DWORD(backup.size()),true);
        Check(*Load(root+"\\before.dsk")==backup&&ValidateFloppyHfs(backup),
              "A: must contain a classic HFS image and have a verified backup");
        lastVerified=backup;
        step("Backed up and verified all 1,474,560 original bytes from A:.");

        const auto name=MacBrowser::U16(VolumeCoord::Wide(NativeTestName));
        const auto inspect=[&](bool recover,const std::function<void(MacBrowser::Volume&)>& check){
            NativeFloppyAttachment attachment;
            if(recover)RecoverFloppyWrite(0);
            char path[MAX_PATH]{};
            get_floppy_volume_file_name(0,path);
            auto volume=MacBrowser::OpenClassic(path,":",false);
            check(*volume);
            volume->Close();
        };
        inspect(false,[&](MacBrowser::Volume& volume){
            MacBrowser::Entry file;
            Check(!volume.FindChild(2,name,file)&&volume.FreeBytes()>65536,
                  "Use a spare HFS floppy with at least 64 KiB free and no HFV Move Probe file");
        });
        const auto casePath=root+"\\transfer";
        const auto hostPath=VolumeCoord::Wide(casePath+".windows");
        Check(CreateDirectoryW(hostPath.c_str(),nullptr)!=FALSE,"Cannot create the NTFS test folder");
        {
            auto host=MacBrowser::OpenWindows(hostPath);
            MacBrowser::Entry entry;
            entry.name=name;
            entry.forks[0].size=5003;
            entry.forks[1].size=723;
            memcpy(entry.finder.data(),"TEXTTEST",8);
            host->Put(2,entry,[](unsigned fork,uint64_t at,void* data,size_t n){Payload(fork,at,data,n);},false);
        }

        for(const bool importing:{true,false}){
            for(const unsigned phase:{8u,9u}){
                const auto direction=importing?"NTFS -> A:":"A: -> NTFS";
                fprintf(report,"Starting %s crash %s commit.\n",direction,phase==8?"before":"after");fflush(report);
                altered=true;
                CrashMedia(casePath,importing?"move-native-in":"move-native-out",phase);
                Check(RemovableUndo::Pending(key),"native crash did not leave the floppy journal");
                const bool onFloppy=(phase==9)==importing;
                inspect(true,[&](MacBrowser::Volume& volume){
                    auto host=MacBrowser::OpenWindows(hostPath);
                    MacBrowser::Entry diskEntry,hostEntry;
                    Check(volume.FindChild(2,name,diskEntry)==onFloppy&&
                          host->FindChild(2,name,hostEntry)!=onFloppy,"floppy and NTFS disagreed after native recovery");
                    const auto& entry=onFloppy?diskEntry:hostEntry;
                    VerifyMovedPayload(onFloppy?volume:*host,entry);
                    Check(!memcmp(entry.finder.data(),"TEXTTEST",8),"native move changed Finder metadata");
                });
                Check(!RemovableUndo::Pending(key),"native recovery left a pending floppy journal");
                const auto bytes=read();
                if(phase==8)Check(bytes==lastVerified,"pre-commit replay changed original floppy sectors");
                lastVerified=bytes;
                fprintf(report,"PASS: %s crash %s commit; both forks and Finder information verified.\n",
                        direction,phase==8?"before":"after");fflush(report);
            }
        }
    }catch(const std::exception& e){
        fprintf(report,"FAIL: %s\n",e.what());fflush(report);
        result=1;
    }

    if(altered){
        try{
            if(RemovableUndo::Pending(key)){
                NativeFloppyAttachment attachment;
                RecoverFloppyWrite(0);
            }
            // Restore only if the current sectors match the last verified state.
            const auto current=read();
            Check(current==lastVerified,"A: differs from the last verified test state; retain before.dsk for manual recovery");
            FloppyWriter writer(&api);
            if(!writer.WriteImage(0,backup,nullptr,nullptr,&current))
                throw HfsPlus::Error(writer.Diagnostics());
            Check(read()==backup,"the restored disk does not match before.dsk");
            step("PASS: restored A: and verified every byte against before.dsk.");
        }catch(const std::exception& e){
            fprintf(report,"RESTORE PENDING: %s\n",e.what());fflush(report);
            result=1;
        }
    }
    fclose(report);
    return result;
}

int RunPhysicalRemovableTest(const char* directory){if(!CreateDirectoryA(directory,nullptr))return 2;std::string root=directory;FILE* report=fopen((root+"\\physical-removable-report.txt").c_str(),"w");if(!report)return 2;int result=0;InitializeStorageUi();readonly_cd_backend_init();try{
 SourceAdapters(report);
 ApfsRemovableTest(report);
 auto api=std::make_shared<Fake>();auto classic=Classic(root+"\\classic.dsk"),mfs=Mfs();std::vector<std::shared_ptr<Bytes>> plus;for(auto kind:{HfsPlus::Volume::FormatKind::HfsPlus,HfsPlus::Volume::FormatKind::HfsXCaseSensitive,HfsPlus::Volume::FormatKind::HfsXCaseInsensitive})plus.push_back(Plus(root+"\\plus"+std::to_string(plus.size())+".dsk",kind));auto raw=api->Add(L"USB raw",plus[0],BusTypeUsb,false);auto zip=api->Add(L"Zip",classic,BusTypeScsi,true);api->Add(L"USB MFS",mfs)->geometry.floppy=true;auto apm=Apm(*api,classic,plus[1]);auto gpt=Gpt(*api,plus[2],512),gpt4=Gpt(*api,plus[0],4096);auto internal=api->Add(L"Internal SATA",classic,BusTypeSata,false);Check(!Removable::Eligible(internal->device),"Internal fixed disk was eligible");
 for(auto& d:api->devices)if(Removable::Eligible(d->device)){auto volumes=Removable::Open(d->device,api);Check(volumes.size()==(d==apm?2:1),"Mac removable partition discovery");for(auto& mounted:volumes){auto v=mounted.volume;const auto execution=MacBrowser::ExecutionFor(*v);const std::string kind=v->Kind();Check(execution.stableCatalogIds==(kind!="MFS")&&!execution.Persistent(),"removable worker changed catalog identity policy");Check(!v->Writable()&&!v->Supports().create&&!v->Supports().rename&&!v->Supports().remove,"Physical removable media exposed writes");Verify(*v);Reject([&]{v->MakeDirectory(2,u"Unsafe");},"Physical folder creation allowed");Reject([&]{v->Remove(Find(*v,2,u"Hello"));},"Physical delete allowed");auto target=MacBrowser::OpenWindows(VolumeCoord::Wide(root));Copy(v,target);Verify(*target);if(std::string(v->Kind())=="HFS")Check(v->Name()==u"Zip Classic","Classic removable volume name");}}
 Check(internal->opens==0,"Internal fixed disk was opened");fprintf(report,"PASS: raw classic HFS/MFS/HFS+/both HFSX, compressed/shared files, multi-partition APM above 4 GiB, GPT with 512/4096-byte logical sectors, both-fork export and read-only operation enforcement; internal fixed disks excluded.\n");fflush(report);
 {auto sparse=api->Add(L"Large USB",std::make_shared<Bytes>(0));sparse->segments.clear();sparse->geometry.bytes=5ULL*1024*1024*1024;sparse->pattern=true;Removable::Disk disk(sparse->device,api);Bytes b(1003);uint64_t at=4ULL*1024*1024*1024+137;disk.Read(at,b.data(),b.size());for(size_t i=0;i<b.size();++i)Check(b[i]==uint8_t((at+i)*13+((at+i)>>32)),"Read beyond 4 GiB wrapped");auto reads=sparse->reads;disk.Read(at,b.data(),b.size());Check(sparse->reads==reads,"Removable read cache missed");Reject([&]{disk.Write(0,b.data(),b.size());},"Physical transport write allowed");Reject([&]{disk.Flush();},"Physical transport flush allowed");++sparse->generation;Reject([&]{disk.Read(at,b.data(),b.size());},"Cached media swap was not rejected");Reject([&]{disk.Read(at,b.data(),b.size());},"Changed media did not latch invalid");}
 for(unsigned mode=0;mode<4;++mode){auto d=api->Add((L"Fault "+std::to_wstring(mode)).c_str(),classic);if(mode==0)d->noIdentity=true;else if(mode==1)d->error=ERROR_ACCESS_DENIED;if(mode<2){auto closes=d->closes;Reject([&]{Removable::Open(d->device,api);},"Missing media identity/access denial accepted");Check(mode==1||d->closes==closes+1,"Failed open leaked handle");}else{Removable::Disk disk(d->device,api);if(mode==2)d->shortRead=true;else d->changeAfterRead=true;Bytes bytes(512,0x77);Reject([&]{disk.Read(0,bytes.data(),bytes.size());},"Short read/media swap during I/O accepted");Check(std::all_of(bytes.begin(),bytes.end(),[](uint8_t b){return b==0x77;}),"Unverified read reached caller");}}
 {auto d=api->Add(L"Bad GPT",plus[0]);d->geometry=gpt->geometry;d->segments=gpt->segments;auto last=d->geometry.bytes-d->geometry.sector;d->segments[last]=std::make_shared<Bytes>(*d->segments[last]);(*d->segments[last])[16]^=1;Reject([&]{Removable::Open(d->device,api);},"Corrupt backup GPT accepted");}
 {auto d=api->Add(L"Bad APM",classic);d->geometry=apm->geometry;d->segments=apm->segments;d->segments[0]=std::make_shared<Bytes>(*d->segments[0]);B::P32(d->segments[0]->data()+512+12,UINT32_MAX);Reject([&]{Removable::Open(d->device,api);},"Out-of-bounds Apple partition accepted");}
 {auto mbr=api->Add(L"Mac MBR",std::make_shared<Bytes>(512));mbr->geometry.bytes=plus[0]->size()+1024*1024;auto& boot=*mbr->segments[0];boot[510]=0x55;boot[511]=0xaa;boot[450]=0xaf;Little(boot.data()+454,2048,4);Little(boot.data()+458,plus[0]->size()/512,4);mbr->segments[1024*1024]=plus[0];auto volumes=Removable::Open(mbr->device,api);Check(volumes.size()==1,"MBR Apple partition was not discovered");Verify(*volumes[0].volume);}
 {auto bad=api->Add(L"Overlapping GPT",std::make_shared<Bytes>(512));bad->geometry=gpt->geometry;bad->segments=gpt->segments;auto table=std::make_shared<Bytes>(*bad->segments[1024]);memcpy(table->data()+128,table->data(),128);(*table)[144]=2;for(bool backup:{false,true}){auto lba=backup?bad->geometry.bytes/512-1:1;auto h=std::make_shared<Bytes>(*bad->segments[lba*512]);Little(h->data()+88,Crc(*table),4);Little(h->data()+16,0,4);Little(h->data()+16,Crc(Bytes(h->begin(),h->begin()+92)),4);bad->segments[lba*512]=h;bad->segments[(backup?bad->geometry.bytes/512-2:2)*512]=std::make_shared<Bytes>(*table);}Reject([&]{Removable::Open(bad->device,api);},"Overlapping GPT partitions accepted");}
 {auto source=std::make_shared<Removable::Disk>(raw->device,api);ReadOnlySources::Registration registration(source);Check(fhook_open(registration.Path().c_str(),O_RDWR)<0&&errno==EROFS,"Classic stream allowed a writable mount");auto fd=fhook_open(registration.Path().c_str(),O_RDONLY);Check(fd>=0&&fhook_lseek(fd,0,SEEK_END)==source->Size()&&fhook_write(fd,"x",1)<0&&errno==EROFS&&fhook_close(fd)==0,"Classic source hooks violated read-only/seek contract");}
 Removable::SetTestApi(api);api->windowsReadable=true;Check(Removable::FromDriveRoot(L"Z:\\").empty(),"Windows-readable volume was hijacked");api->windowsReadable=false;auto direct=Removable::FromDriveRoot(L"Z:\\");Check(direct.size()==1,"Explicit Windows drive selection missed Mac media");Verify(*direct[0].volume);direct.clear();Storage::Job scan;auto discovery=Removable::Scan(scan,api);Check(!discovery.errors.empty()&&discovery.volumes.size()>=7,"One failed device aborted discovery");discovery.volumes.clear();
 fprintf(report,"PASS: 64-bit reads/cache hits, cached/during-read media swaps, short reads, missing change counters, access denial, handle cleanup, GPT checksum/APM bounds checks, Windows-volume fallback and per-device discovery errors. No device writes or physical hardware.\n");
 WritableMedia(root,classic,plus[0],plus[1],mfs);
 fprintf(report,"PASS: exclusive removable writes for classic HFS and HFS+ in APM, HFSX in 4096-byte GPT and raw MFS including 1.44 MB physical floppies. Separate mounts refreshed. Partition boundaries and neighboring data were preserved. Lock conflicts, media swaps, verification mismatch, failed Cut, and source changes under the final Cut lock were handled safely.\n");
 fflush(report);
 const auto gptImage=GptImages(root,*gpt,plus[0],plus[1]);
 fprintf(report,"PASS: GPT images open on persistent workers and support writes/reopen at 512/4096-byte sector sizes and above 4 GiB. GPT metadata and neighboring data remain intact. Invalid CRCs, overlaps and partition bounds are rejected; read-only flags disable writes.\n");
 Check(api->Active()==0,"Core removable tests leaked device handles");BrowserChecks(root,plus[0],classic,gptImage);fprintf(report,"PASS: restored startup checkbox, manual scan command routing, disabled/enabled startup detection, normal drive selection, media-change refresh, stale clipboard refusal, responsive unrelated browsing and copy completion after closing its source window; all device I/O remained on workers.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());result=1;}BrowserDirectory::testObserveRead={};auto& app=*static_cast<CHFVExplorerApp*>(AfxGetApp());auto frames=app.m_browsers;for(auto frame:frames)if(IsWindow(frame->GetSafeHwnd()))frame->DestroyWindow();Removable::SetTestApi({});ShutdownStorageUi();readonly_cd_backend_final();fclose(report);return result;}
#endif
