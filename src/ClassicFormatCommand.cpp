// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "ClassicFormatCommand.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
#include "CommandStorage.h"
#include "HFVExplorerDoc.h"
#include "LegacyEngine.h"
#include "HfsPlus.h"
#include "ReadOnlySource.h"
#include "charset.h"
#include <fcntl.h>
extern "C" {
#include "hfs/libhfs/hfs.h"
}

#ifdef HFV_TESTING
std::function<void(ClassicFormatStage,HfsPlus::Source&)> classicFormatTestStage;
#endif

namespace {
void VerifyFormat(HfsPlus::Source& output,std::string& path,const std::string& label)
{
 std::array<uint8_t,512> primary{},alternate{};
 output.Read(1024,primary.data(),primary.size());
 output.Read(output.Size()-1024,alternate.data(),alternate.size());
 if(primary[0]!=0x42||primary[1]!=0x44||!(primary[10]&1)||primary!=alternate)
  throw HfsPlus::Error("The new HFS image failed verification of its volume headers");

 std::unique_ptr<hfsvol,decltype(&hfs_umount)> volume(
  hfs_mount_session(path.data(),0,O_RDONLY),hfs_umount);
 hfsvolent info{};
 char rootPath[]=":";
 if(!volume||hfs_vstat(volume.get(),&info)<0||info.name!=label)
  throw HfsPlus::Error("The new HFS image could not be reopened with the requested name");
 std::unique_ptr<hfsdir,decltype(&hfs_closedir)> root(
  hfs_opendir(volume.get(),rootPath),hfs_closedir);
 hfsdirent entry{};
 if(!root||hfs_readdir(root.get(),&entry)!=-1||errno!=ENOENT)
  throw HfsPlus::Error("The new HFS image failed verification of its empty root folder");
 if(hfs_closedir(root.release())<0||hfs_umount(volume.release())<0)
  throw HfsPlus::Error("Could not close the verified HFS image");
}
}

void FormatClassicImage(const std::wstring& path,std::string label,hfv_size_t bytes,Storage::Job* job)
{
 if(label.empty()||label.size()>HFS_MAX_VLEN||label.find(':')!=std::string::npos||
    label.find('\0')!=std::string::npos)
  throw HfsPlus::Error("Use an HFS volume name of 1 to 27 bytes without a colon");
 if(bytes<800*1024||bytes%512||bytes>0xffffffffULL*512)
  throw HfsPlus::Error("Invalid classic HFS image size");
 if(job&&job->cancelled())throw Storage::Cancelled();
 pc_to_mac_charset(reinterpret_cast<unsigned char*>(label.data()));

 LegacyEngine::Scope engine;
 std::shared_ptr<HfsPlus::NewImage> output=HfsPlus::CreateStagedOutput(path,bytes);
 // The formatter and verifier use the same file handle through a bounded source.
 // No pathname reopen is needed until the verified image has been published.
 ReadOnlySources::Registration staged(output,true);
 auto stagedPath=staged.Path();
 if(hfs_format(stagedPath.data(),0,label.data())<0)
  throw HfsPlus::Error(hfs_error?hfs_error:"Could not format the selected image");
 output->Flush();
#ifdef HFV_TESTING
 if(classicFormatTestStage)classicFormatTestStage(ClassicFormatStage::Verifying,*output);
#endif
 VerifyFormat(*output,stagedPath,label);
#ifdef HFV_TESTING
 if(classicFormatTestStage)classicFormatTestStage(ClassicFormatStage::Publishing,*output);
#endif
 if(job&&job->cancelled())throw Storage::Cancelled();
 output->PublishTo(path);
 output->Keep();
#ifdef HFV_TESTING
 if(classicFormatTestStage)classicFormatTestStage(ClassicFormatStage::Published,*output);
#endif
}

bool StartClassicFormat(BrowserSession& session,ClassicFormatRequest request)
{
 try{request.path=FileOperations::Location::Image(request.path,":").ClassicImage();}
 catch(const std::exception& error){
  session.Owner()->MessageBox(error.what(),"Format HFS image",MB_OK|MB_ICONERROR);
  return false;
 }
 auto complete=session.WhileOpen([path=request.path](BrowserSession& origin,const Storage::Result& result){
  if(!result.code)origin.Document()->OnOpenDocument(path.c_str());
 });
 return BackgroundCopies::StartStorageResult([request=std::move(request)](Storage::Job& job) mutable {
  const auto path=VolumeCoord::Wide(request.path);
  const auto keys=CommandStorage::ImageWrite(path);
  return CommandStorage::Guard(keys,[&](Storage::Job& job){
   FormatClassicImage(path,request.label,request.bytes,&job);
   return Storage::Result{};
  },{keys[0].key,keys[1].key})(job);
 },session.Owner(),"Formatting HFS image","Format image",std::move(complete));
}
