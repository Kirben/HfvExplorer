// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FloppyWriteDialog.h"
#include "FloppyWriter.h"
#include "StorageJobDialog.h"
#include "BackgroundCopies.h"
#include "HFVExplorer.h"
#include "MainFrm.h"
#include "BrowserSession.h"
#include "MainMacBrowser.h"
#include "MediaDiscovery.h"
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
  if(!writer.WriteImage(drive,image,false,progress,&job,nullptr,generation)){
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
void WriteFloppyImage(CWnd* owner,int drive,const std::vector<BYTE>& image,const char* description,void(*release)(int,void*),void* context){
 if(!ValidateFloppyHfs(image)){AfxMessageBox("A validated 1.44 MB HFS image is required.");return;}
 DWORD generation=0;
 auto ready=ShowStorageWaitJob(CommandStorage::Guard({{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Read}},
  [&](Storage::Job&){
   FloppyReadApi* api=nullptr;
#ifdef HFV_FLOPPY_TESTING
   api=physical_floppy_test_api(drive);
   if(!api)return Storage::Result{1,ERROR_NOT_SUPPORTED,"No simulated floppy is attached."};
#endif
   ReadOnlyFloppy floppy(api);
   if(!floppy.OpenDrive(drive))return Storage::Result{1,floppy.Error(),"Could not identify the selected floppy. Insert a 1.44 MB disk and try again."};
   generation=floppy.Generation();
   return Storage::Result{};
  }),!owner->IsWindowVisible(),owner);
 if(ready.code){owner->MessageBox(ready.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);return;}
 CString question;question.Format("ERASE ALL DATA on floppy drive %c:?\r\n\r\n%s\r\n\r\nAll 1,474,560 bytes will be written and read back for verification.",'A'+drive,description);
 if(owner->MessageBox(question,"HFVExplorer",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
 CommandStorage::Reservation held;try{held=std::make_shared<VolumeCoord::Lease>(std::vector<VolumeCoord::Access>{{VolumeCoord::DeviceKey(drive),VolumeCoord::Mode::Write}});}catch(const VolumeCoord::Busy&){owner->MessageBox(CommandStorage::Busy().message.c_str(),"HFVExplorer",MB_OK|MB_ICONINFORMATION);return;}
 if(release)release(drive,context);
 BackgroundCopies::StartStorage(MakeFloppyWriteJob(drive,image,nullptr,std::move(held),generation),owner,
  "Writing floppy","Floppy write",[drive](bool){
   const auto mask=DWORD(1)<<drive;
   MediaDiscovery::Changed(mask);
   for(auto frame:static_cast<CHFVExplorerApp*>(AfxGetApp())->m_browsers){
    auto& session=frame->GetBrowserSession();
    session.NavigationMediaChanged(mask);
    session.MacBrowser().QueueMediaChanges(mask,mask);
   }
  });
}
