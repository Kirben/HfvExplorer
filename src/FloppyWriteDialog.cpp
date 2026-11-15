// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FloppyWriteDialog.h"
#include "FloppyWriter.h"
#include "StorageJobDialog.h"

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
std::function<Storage::Result(Storage::Job&)> MakeFloppyWriteJob(int drive,std::vector<BYTE> image,FloppyWriteApi* api){
 return [drive,image=std::move(image),api](Storage::Job& job)->Storage::Result {
#ifdef HFV_TESTING
  if(!api)return {1,ERROR_NOT_SUPPORTED,"No simulated floppy writer was supplied."};
#endif
  if(job.cancelled())throw Storage::Cancelled();
  FloppyWriter writer(api);
  if(!writer.WriteImage(drive,image,false,progress,&job)){
   DWORD error=writer.Error();char system[512]={};FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,error,0,system,sizeof(system),nullptr);
   std::string message="Floppy operation failed (Windows error "+std::to_string(error)+"). "+system+"\r\nStage: "+writer.Stage()+"\r\nByte offset: "+std::to_string(writer.Offset())+"\r\nCompleted: "+std::to_string(writer.WrittenTracks())+" / 160 writes; "+std::to_string(writer.VerifiedTracks())+" / 160 readbacks.\r\n";
   message+=writer.WritesStarted()?"Writing started; the disk may be incomplete. Do not treat it as a successful format.":"No data writes were issued.";
   auto report=saveDiagnostics(writer);if(!report.empty())message+="\r\nDiagnostic log: "+report;
   return {1,error,message};
  }
  Storage::Progress p;p.phase="Complete: all sectors written and verified";p.byteTransfer=true;p.scanning=false;p.bytes=p.processed=p.totalBytes=image.size();job.progress(p);return {};
 };
}
void WriteFloppyImage(CWnd* owner,int drive,const std::vector<BYTE>& image,const char* description,void(*release)(int,void*),void* context){
 if(!ValidateFloppyHfs(image)){AfxMessageBox("A validated 1.44 MB HFS image is required.");return;}
 CString question;question.Format("ERASE ALL DATA on floppy drive %c:?\r\n\r\n%s\r\n\r\nAll 1,474,560 bytes will be written and read back for verification.",'A'+drive,description);
 if(owner->MessageBox(question,"HFVExplorer",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)return;
 if(release)release(drive,context);
 auto result=ShowStorageJob(MakeFloppyWriteJob(drive,image),"Writing floppy",false,true,false);
 if(result.code)owner->MessageBox(result.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
}
