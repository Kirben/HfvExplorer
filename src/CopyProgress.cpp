// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BrowserSession.h"
#include "CopyProgress.h"
#include "FileOperations.h"
#include "StorageJobDialog.h"
extern "C" {
#include "hfs/interface.h"
}
LegacyCopyOptions CaptureLegacyCopyOptions(char output){LegacyCopyOptions options;int ad;HFSIFACE_copy_settings(&options.output,&options.input,&ad);options.output=output;options.appledouble=ad!=0;return options;}
bool StageWindowsLaunchDocument(BrowserSession& session,const FileOperations::Location& source,CString& output)
{
 output.Empty();
 char temp[MAX_PATH], reservation[MAX_PATH];
 DWORD length=GetTempPath(ARRAYSIZE(temp),temp);
 auto owner=session.Owner();
 if(!length||length>=ARRAYSIZE(temp)||!GetTempFileName(temp,"hfv",0,reservation)){
  owner->MessageBox("Could not reserve a directory for the temporary document.","HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }
 CString folder=CString(reservation)+".dir";
 BOOL created=folder.GetLength()<MAX_PATH&&CreateDirectory(folder,nullptr);
 DeleteFile(reservation);
 if(!created){
  owner->MessageBox("Could not create the temporary document directory.","HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }

 auto target=FileOperations::Location::Windows(VolumeCoord::Wide(std::string(static_cast<LPCSTR>(folder))));
 FileOperations::LegacyConversion conversion{source,target,false,false,false,CaptureLegacyCopyOptions('r'),FileOperations::ConversionTarget::Folder};
 auto operation=FileOperations::Submit(FileOperations::Request(std::move(conversion)));
 auto result=ShowSubmittedStorageJob(operation.Job(),"Copying from Macintosh",!owner->IsWindowVisible(),true,true,owner);
 if(result.code){
  RemoveDirectory(folder);
  if(result.error!=ERROR_CANCELLED)owner->MessageBox(result.message.c_str(),"HFVExplorer",MB_OK|MB_ICONERROR);
  return false;
 }

 const auto completion=operation.Snapshot();
 if(completion.complete&&!completion.publishedWindowsExport.empty()){
  output=FileOperations::Location::Windows(completion.publishedWindowsExport).LegacyPath().c_str();
  return true;
 }
 owner->MessageBox("Cannot identify the temporary document to launch. Export directory: "+folder,"HFVExplorer",MB_OK|MB_ICONERROR);
 RemoveDirectory(folder);
 return false;
}
