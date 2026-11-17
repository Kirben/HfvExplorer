// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "VolumeCoordinator.h"
#include "StorageWorker.h"
namespace CommandStorage {
inline Storage::Result Busy(){return {1,ERROR_BUSY,"This volume is in use by another storage operation. Choose an unrelated volume or wait for it to finish."};}
using Reservation=std::shared_ptr<VolumeCoord::Lease>;
inline std::function<Storage::Result(Storage::Job&)> Guard(std::vector<VolumeCoord::Access> access,std::function<Storage::Result(Storage::Job&)> work,std::vector<std::string> changed={},Reservation held={}){
 return [=](Storage::Job& job){
  try{std::unique_ptr<VolumeCoord::OperationLease> lease;if(!held)lease.reset(new VolumeCoord::OperationLease(access));if(job.cancelled())throw Storage::Cancelled();
   struct Notify{const std::vector<std::string>& keys;~Notify(){VolumeCoord::Changed(keys);}}notify{changed};return work(job);
  }catch(const VolumeCoord::Busy&){return Busy();}
 };
}
inline std::vector<VolumeCoord::Access> ImageWrite(const std::wstring& path){return {{VolumeCoord::ImageKey(path),VolumeCoord::Mode::Write},{VolumeCoord::PathKey(path),VolumeCoord::Mode::Write}};}
}
