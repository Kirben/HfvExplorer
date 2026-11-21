// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FloppyWriteDialog.h"
#include "FloppyWriter.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
#include "LegacyEngine.h"
#include "MediaCommands.h"
#include "ReadOnlyFloppy.h"
#include "ReadOnlyCdBackend.h"

namespace {
std::string saveDiagnostics(const FloppyWriter& writer){
#ifdef HFV_TESTING
 return {}; // Simulated jobs do not query any real drive's type.
#else
 char folder[MAX_PATH],root[MAX_PATH],path[MAX_PATH];
 if(!GetTempPathA(MAX_PATH,folder)||!GetVolumePathNameA(folder,root,MAX_PATH)||GetDriveTypeA(root)!=DRIVE_FIXED)return {};
 if(!GetTempFileNameA(folder,"hfv",0,path))return {};
 HANDLE file=CreateFileA(path,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)return {};
 const auto& text=writer.Diagnostics();DWORD written=0;
 bool ok=WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr)&&written==text.size();CloseHandle(file);
 return ok?path:"";
#endif
}
void progress(const char* phase,unsigned completed,unsigned total,void* context){
 if(!strcmp(phase,"Checking media before write")||!strcmp(phase,"Checking media before readback"))return;
 auto& job=*static_cast<Storage::Job*>(context);Storage::Progress p;
 p.phase=phase;p.byteTransfer=true;p.scanning=false;p.bytes=p.processed=completed*9216ULL;p.totalBytes=total*9216ULL;
 if(!strcmp(phase,"Flushing written data"))p.scanning=true;
 job.progress(p);
}
}
std::function<Storage::Result(Storage::Job&)> MakeFloppyWriteJob(int drive,std::vector<BYTE> image,FloppyWriteApi* api,CommandStorage::Reservation held,std::optional<DWORD> generation){
 auto work=[drive,image=std::move(image),api,generation](Storage::Job& job)->Storage::Result {
#ifdef HFV_TESTING
  if(!api)return {1,ERROR_NOT_SUPPORTED,"No simulated floppy writer was supplied."};
#endif
  if(job.cancelled())throw Storage::Cancelled();
  FloppyWriter writer(api);
  if(!writer.WriteImage(drive,image,progress,&job,nullptr,generation)){
   DWORD error=writer.Error();char system[512]={};FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,error,0,system,sizeof(system),nullptr);
   std::string message="Floppy operation failed (Windows error "+std::to_string(error)+"). "+system+"\r\nStage: "+writer.Stage()+"\r\nByte offset: "+std::to_string(writer.Offset())+"\r\nCompleted: "+std::to_string(writer.WrittenTracks())+" / 160 writes; "+std::to_string(writer.VerifiedTracks())+" / 160 readbacks.\r\n";
   message+=writer.WritesStarted()?"Writing started; the disk may be incomplete. Do not treat it as a successful format.":"No data writes were issued.";
   auto report=saveDiagnostics(writer);if(!report.empty())message+="\r\nDiagnostic log: "+report;
   return {1,error,message};
  }
  Storage::Progress p;p.phase="Complete: all sectors written and verified";p.byteTransfer=true;p.scanning=false;p.bytes=p.processed=p.totalBytes=image.size();job.progress(p);return {};
 };
 if(drive<0||drive>25)return work;auto key=VolumeCoord::DeviceKey(drive);return CommandStorage::Guard({{key,VolumeCoord::Mode::Write}},work,{key},held);
}
std::function<Storage::Result(Storage::Job&)> MakeFloppyPreparationJob(
    int drive,FloppyImageInput source,std::shared_ptr<PreparedFloppyImage> prepared)
{
 return [drive,source=std::move(source),prepared=std::move(prepared)](Storage::Job& job){
  if(drive<0||drive>=26)return Storage::Result{1,ERROR_BAD_DEVICE,"Invalid floppy drive."};
  std::vector<VolumeCoord::Access> access{{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}};
  const auto* image=std::get_if<ExistingFloppyImage>(&source);
  if(image){
   const auto path=VolumeCoord::Wide(image->path);
   access.push_back({VolumeCoord::ImageKey(path),VolumeCoord::Mode::Read});
   access.push_back({VolumeCoord::PathKey(path),VolumeCoord::Mode::Read});
  }
  return CommandStorage::Guard(std::move(access),[&](Storage::Job&){
   PreparedFloppyImage input;
   std::string error;
   if(image){
    if(!LoadFloppyHfs(image->path.c_str(),input.bytes,error))
     return Storage::Result{1,ERROR_READ_FAULT,error};
   }else{
    LegacyEngine::Scope engine;
    if(!PrepareFloppyHfs(std::get<NewFloppyImage>(source).label.c_str(),input.bytes,error))
     return Storage::Result{1,ERROR_WRITE_FAULT,error};
   }
   if(job.cancelled())throw Storage::Cancelled();
   FloppyReadApi* api=nullptr;
#ifdef HFV_FLOPPY_TESTING
   api=physical_floppy_test_api(drive);
   if(!api)return Storage::Result{1,ERROR_NOT_SUPPORTED,"No simulated floppy is attached."};
#endif
   ReadOnlyFloppy floppy(api);
   if(!floppy.OpenDrive(drive))return Storage::Result{1,floppy.Error(),
        "Could not identify the selected floppy. Insert a 1.44 MB disk and try again."};
   if(floppy.MediaSize()!=input.bytes.size())return Storage::Result{1,ERROR_NOT_SUPPORTED,
        "Insert a 1.44 MB disk and try again."};
   if(job.cancelled())throw Storage::Cancelled();
   input.generation=floppy.Generation();
   *prepared=std::move(input);
   return Storage::Result{};
  })(job);
 };
}

bool StartFloppyWrite(BrowserSession& session,int drive,FloppyImageInput source)
{
 auto* image=std::get_if<ExistingFloppyImage>(&source);
 if(image){
  try{image->path=FileOperations::Location::Image(image->path,":").ClassicImage();}
  catch(const std::exception& error){
   session.Owner()->MessageBox(error.what(),"Prepare floppy write",MB_OK|MB_ICONERROR);
   return false;
  }
 }
 const auto description=image?image->path:std::string("Format a new HFS volume");
 auto prepared=std::make_shared<PreparedFloppyImage>();
 auto complete=session.WhileOpen([drive,description,prepared](BrowserSession& origin,const Storage::Result& ready){
  if(ready.code)return;
  auto begin=origin.WhileOpen([drive,prepared](BrowserSession& browser){
   auto& owner=*browser.Owner();
   const auto started=MediaCommands::Start(owner,drive,"Writing floppy","Floppy write",
    [&](CommandStorage::Reservation held){
     return MakeFloppyWriteJob(drive,std::move(prepared->bytes),nullptr,std::move(held),prepared->generation);
    });
   if(started.code)owner.MessageBox(started.message.c_str(),"HFVExplorer",
       MB_OK|(started.error==ERROR_BUSY?MB_ICONINFORMATION:MB_ICONERROR));
  });
  CString question;
  question.Format("ERASE ALL DATA on floppy drive %c:?\r\n\r\n%s\r\n\r\nAll 1,474,560 bytes will be written and read back for verification.",
       'A'+drive,description.c_str());
  if(origin.Owner()->MessageBox(question,"HFVExplorer",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES)
   begin();
 });
 return BackgroundCopies::StartStorageResult(MakeFloppyPreparationJob(drive,std::move(source),prepared),
    session.Owner(),"Preparing floppy write","Prepare floppy write",std::move(complete),{true});
}
