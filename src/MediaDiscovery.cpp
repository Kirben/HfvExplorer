// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "MediaDiscovery.h"
#include "VolumeCoordinator.h"
#include "ReadOnlyCdBackend.h"
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
  const VolumeCoord::OperationLease read({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}});
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
}
