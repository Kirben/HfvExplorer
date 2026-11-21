// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MediaDiscovery.h"
#include "BrowserMountInventory.h"
#include "CommandStorage.h"
#include "WorkerEndpoint.h"
#include <algorithm>
#include "VolumeCoordinator.h"
#include "ReadOnlyCdBackend.h"
#include "FloppyFileWrite.h"
#include "RemovableUndo.h"
#include "floppy.h"
#include <array>
#include <chrono>
#include <map>
#include <type_traits>

namespace MediaDiscovery {
namespace {
std::mutex mutex;
std::condition_variable changed;
std::array<uint64_t,26> driveEpoch{};
uint64_t externalEpoch=0;

template<class T> struct Flight {
 uint64_t epoch;
 bool done=false;
 T result{};
 std::exception_ptr error;
};
template<class Key,class T> using Probes=std::map<Key,std::shared_ptr<Flight<T>>>;
using OpticalVolume=std::shared_ptr<MacBrowser::Volume>;
using ExternalDevice=std::pair<std::wstring,uintptr_t>;
Probes<int,OpticalVolume> optical;
Probes<int,bool> floppy;
Probes<ExternalDevice,std::vector<Removable::Mounted>> external;
// Retain a weak reference so a queued browser can reuse the probe without
// keeping an optical handle open after all browsers release the volume.
std::array<std::weak_ptr<MacBrowser::Volume>,26> recentOptical;

void CheckDrive(int drive)
{
 if(drive<0||drive>=26)throw std::invalid_argument("Invalid removable drive");
}
void Cancel(Storage::Job& job){if(job.cancelled())throw Storage::Cancelled();}

template<class Key,class T,class Read>
T Probe(Probes<Key,T>& probes,const Key& key,const uint64_t& epoch,Storage::Job& job,Read read,
        std::weak_ptr<MacBrowser::Volume>* recent=nullptr)
{
 Cancel(job);
 std::shared_ptr<Flight<T>> flight;
 {
  std::unique_lock lock(mutex);
  const auto found=probes.find(key);
  if(found!=probes.end()&&found->second->epoch==epoch){
   flight=found->second;
   lock.unlock();
   Storage::Progress waiting;waiting.phase="Waiting for removable media";job.progress(waiting);
   lock.lock();
   while(!flight->done&&flight->epoch==epoch&&!job.cancelled())
    changed.wait_for(lock,std::chrono::milliseconds(25));
   Cancel(job);
   if(flight->epoch!=epoch)throw Storage::MediaChanged("Removable media changed during discovery.");
   if(flight->error)std::rethrow_exception(flight->error);
   return flight->result;
  }
  flight=std::make_shared<Flight<T>>();
  flight->epoch=epoch;
  probes[key]=flight;
 }
 try{flight->result=read();}
 catch(CException* error){
  char message[512]{};
  error->GetErrorMessage(message,ARRAYSIZE(message));error->Delete();
  flight->error=std::make_exception_ptr(std::runtime_error(*message?message:"Cannot read removable media"));
 }
 catch(...){flight->error=std::current_exception();}
 bool discarded;
 {
  std::lock_guard lock(mutex);
  discarded=flight->epoch!=epoch;
  if constexpr(std::is_same_v<T,OpticalVolume>)
   if(recent&&!discarded&&!flight->error)*recent=flight->result;
  flight->done=true;
  const auto found=probes.find(key);
  if(found!=probes.end()&&found->second==flight)probes.erase(found);
 }
 changed.notify_all();
 Cancel(job);
 if(discarded)throw Storage::MediaChanged("Removable media changed during discovery.");
 if(flight->error)std::rethrow_exception(flight->error);
 return flight->result;
}
}

std::shared_ptr<MacBrowser::Volume> Optical(int drive,Storage::Job& job,OpticalProbe probe)
{
 CheckDrive(drive);
 Cancel(job);
 OpticalVolume cached;
 uint64_t epoch;
 {
  std::lock_guard lock(mutex);
  cached=recentOptical[drive].lock();
  epoch=driveEpoch[drive];
 }
 if(cached){
  const VolumeCoord::Lease read({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}});
  bool valid=true;
  try{cached->ValidateMedia();}catch(const std::exception&){valid=false;}
  std::lock_guard lock(mutex);
  if(epoch!=driveEpoch[drive])throw Storage::MediaChanged("Optical media changed during discovery.");
  Cancel(job);
  if(valid)return cached;
  if(recentOptical[drive].lock()==cached)recentOptical[drive].reset();
 }
 return Probe(optical,drive,driveEpoch[drive],job,[&]{
  const VolumeCoord::Lease read({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}});
  const char root[]={char('A'+drive),':','\\',0};
  return probe?probe():MacBrowser::OpenCdRoot(root);
 },&recentOptical[drive]);
}

bool Floppy(int drive,Storage::Job& job)
{
 CheckDrive(drive);
 return Probe(floppy,drive,driveEpoch[drive],job,[&]{
  const bool recovery=RemovableUndo::Pending(RemovableUndo::FloppyKey(drive));
  const VolumeCoord::OperationLease lease({{VolumeCoord::DeviceKey(drive),recovery?VolumeCoord::Mode::Write:VolumeCoord::Mode::Read}});
  if(recovery){
   RecoverFloppyWrite(drive);
   VolumeCoord::Changed({VolumeCoord::DeviceKey(drive)});
  }
  return is_hfs_floppy_present(drive)!=0;
 });
}

std::vector<Removable::Mounted> External(const Removable::Device& device,
    const std::shared_ptr<Removable::Api>& api,Storage::Job& job)
{
 auto path=device.path;
 if(!path.empty())CharUpperBuffW(path.data(),DWORD(path.size()));
 return Probe(external,ExternalDevice{std::move(path),reinterpret_cast<uintptr_t>(api.get())},
              externalEpoch,job,[&]{return Removable::Open(device,api);});
}

DriveMedia ProbeDrive(int drive,UINT type,bool checkFloppy,bool checkExternal,Storage::Job& job)
{
 CheckDrive(drive);
 Cancel(job);
 DriveMedia media;
 if(type==DRIVE_CDROM){
  media.cd=Optical(drive,job);
  return media;
 }
 if(type!=DRIVE_REMOVABLE&&type!=DRIVE_FIXED)return media;
 const bool recovery=checkFloppy&&type==DRIVE_REMOVABLE&&RemovableUndo::Pending(RemovableUndo::FloppyKey(drive));
 const VolumeCoord::Lease read({{VolumeCoord::DeviceKey(drive),recovery?VolumeCoord::Mode::Write:VolumeCoord::Mode::Read}});
 if(checkFloppy&&type==DRIVE_REMOVABLE&&Floppy(drive,job)){
  char image[MAX_PATH];
  get_floppy_volume_file_name(drive,image);
  try{media.floppy=MacBrowser::OpenClassic(image,":",false);}
  catch(const HfsPlus::Error&){}
 }
 if(!media.floppy&&(checkExternal||(checkFloppy&&type==DRIVE_REMOVABLE))){
  const wchar_t root[]={wchar_t(L'A'+drive),L':',L'\\',0};
  media.removable=Removable::FromDriveRoot(root,&job,!checkExternal);
 }
 Cancel(job);
 return media;
}

uint64_t Generation()
{
 std::lock_guard lock(mutex);
 return externalEpoch;
}

void Changed(DWORD driveMask)
{
 if(!driveMask)return;
 {
  std::lock_guard lock(mutex);
  for(int drive=0;drive<26;++drive)if(driveMask&(DWORD(1)<<drive)){
   ++driveEpoch[drive];
   recentOptical[drive].reset();
  }
  ++externalEpoch;
 }
 changed.notify_all();
}

void UpdateController::Cancel()
{
 for(auto& entry:driveChanges_)if(entry.second.job)entry.second.job->cancel();
 driveChanges_.clear();
 if(externalScan_)externalScan_->job->cancel();
 externalScan_.reset();
}

void UpdateController::ScanExternal(bool silent,bool startupOnly)
{
#ifdef HFV_TESTING
 if(!Removable::HasTestApi())return;
#endif
 if(startupOnly&&removableScanned_)return;
 removableScanned_=true;
 if(auto& scan=externalScan_){
  scan->silent=scan->silent&&silent;
  return;
 }
 auto media=std::make_shared<Removable::Discovery>();
 const auto generation=MediaDiscovery::Generation();
 auto job=Storage::navigationWorker().submit([media](Storage::Job& job){
  *media=Removable::Scan(job);
  return Storage::Result{};
 });
 externalScan_=ExternalScan{media,std::move(job),generation,silent};
 wake_(externalScan_->job);
}
void UpdateController::Queue(DWORD changed,DWORD arrived,bool rescan)
{
 for(int drive=0;drive<26;++drive){
  const auto bit=DWORD(1)<<drive;
  if(!(changed&bit))continue;
  const auto [position,inserted]=driveChanges_.try_emplace(drive);
  auto& pending=position->second;
  if(rescan&&!inserted&&!pending.rescan)continue;
  if(pending.job)pending.job->cancel();
  pending={bool(arrived&bit),rescan,{},{}};
 }
}

UpdateController::Status UpdateController::Apply(
    bool startupFloppies,const ApplyDrive& applyDrive,const ApplyScan& applyScan)
{
 bool retry=false;
 if(externalScan_&&externalScan_->job->done()){
  auto scan=std::exchange(externalScan_,std::nullopt);
  const auto result=scan->job->result();
  if(scan->generation!=MediaDiscovery::Generation()||result.error==ERROR_MEDIA_CHANGED){
   ScanExternal(scan->silent);
  }else applyScan(*scan->media,scan->silent,result);
 }
 for(auto it=driveChanges_.begin();it!=driveChanges_.end();){
  const auto drive=it->first;
  auto& pending=it->second;
  const auto key=VolumeCoord::DeviceKey(drive);
  const char root[] = {char('A'+drive),':','\\',0};
  char classicPath[MAX_PATH]{};
  get_floppy_volume_file_name(drive,classicPath);
  if(!pending.job){
   if(!VolumeCoord::Readable(key)){retry=true;++it;continue;}
   auto media=std::make_shared<DriveUpdate>(DriveUpdate{drive,pending.rescan});
   const bool arrived=pending.arrived,rescan=pending.rescan;
   const bool checkFloppy=rescan||mounts_.FindClassicVolume(VolumeCoord::Wide(classicPath)).has_value()||startupFloppies;
   std::function<UINT(int)> driveType;
#ifdef HFV_TESTING
   driveType=testDriveType;
#endif
   pending.media=media;
   pending.job=Storage::navigationWorker().submit(
    [media,drive,arrived,rescan,checkFloppy,key,root=std::string(root),driveType](Storage::Job& job){
     try{
      const VolumeCoord::Lease read({{key,VolumeCoord::Mode::Read}});
      if(job.cancelled())throw Storage::Cancelled();
      media->type=driveType?driveType(drive):GetDriveTypeA(root.c_str());
      if(!arrived)return Storage::Result{};
      auto found=MediaDiscovery::ProbeDrive(drive,media->type,checkFloppy,!rescan,job);
      media->cd=std::move(found.cd);
      media->removable=std::move(found.removable);
      if(found.floppy)media->floppy=MacBrowser::ReadClassicMountInfo(*found.floppy);
     }catch(const VolumeCoord::Busy&){return CommandStorage::Busy();}
     catch(const MacBrowser::DetectedVolumeError&){/* Explicit selection reports mount errors. */}
     catch(const HfsPlus::Error&){/* Keep the Windows drive available. */}
     return Storage::Result{};
    });
   wake_(pending.job);
   ++it;continue;
  }
  if(!pending.job->done()){++it;continue;}
  const auto result=pending.job->result();
  if(result.error==ERROR_BUSY){pending.job.reset();retry=true;++it;continue;}
  if(result.code){it=driveChanges_.erase(it);continue;}
  std::vector<VolumeCoord::Access> resources{{key,VolumeCoord::Mode::Write}};
  for(const auto& mount:mounts_.Mounted())
   if(mount.volume&&(mount.drive==drive||mount.sourceDrive==drive))
    resources.push_back({mount.volume->ResourceKey(),VolumeCoord::Mode::Write});
  std::shared_ptr<VolumeCoord::Lease> access;
  try{access=std::make_shared<VolumeCoord::Lease>(std::move(resources));}
  catch(const VolumeCoord::Busy&){retry=true;++it;continue;}

  auto media=std::move(pending.media);
  it=driveChanges_.erase(it);
  applyDrive(*media,std::move(access));
 }
 return retry?Status::Retry:Pending()?Status::Working:Status::Idle;
}

ExternalChange ClassifyExternal(const MacBrowser::Volume& current,const std::wstring& path,
    int sourceDrive,const MacBrowser::Volume* found,int scannedDrive,const std::vector<std::wstring>& failed)
{
 if(found){
  if(&current==found)return ExternalChange::Reuse;
  const auto oldEndpoint=MacBrowser::WorkerEndpoint::Find(current);
  const auto newEndpoint=MacBrowser::WorkerEndpoint::Find(*found);
  const auto before=oldEndpoint?oldEndpoint->WorkerDescription():MacBrowser::WorkerOrigin{};
  const auto after=newEndpoint?newEndpoint->WorkerDescription():MacBrowser::WorkerOrigin{};
  return before.generationKnown&&after.generationKnown&&before.generation==after.generation&&before.path==after.path
   ?ExternalChange::Reuse:ExternalChange::Remove;
 }
 const bool driveAlias=path.size()==6&&path[5]==L':';
 const bool keep=scannedDrive>=0?sourceDrive!=scannedDrive:
    driveAlias||std::find(failed.begin(),failed.end(),path)!=failed.end();
 return keep?ExternalChange::Keep:ExternalChange::Remove;
}

}
