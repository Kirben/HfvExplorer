// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "ClassicFormatCommand.h"
#include "BackgroundCopies.h"
#include "BrowserSession.h"
#include "CommandStorage.h"
#include "HFVExplorerDoc.h"
#include "LegacyEngine.h"
extern "C" {
#include "hfs/interface.h"
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
  return CommandStorage::Guard(keys,[&](Storage::Job&){
   LegacyEngine::Scope engine;
   struct Silence {
    int previous=HFSIFACE_silencer(1);
    ~Silence(){HFSIFACE_silencer(previous);}
   } silence;
   if(HFSIFACE_format(request.path.data(),request.label.data(),request.bytes,TRUE)!=0)
    return Storage::Result{1,ERROR_WRITE_FAULT,"Could not format the selected image"};
   return Storage::Result{};
  },{keys[0].key,keys[1].key})(job);
 },session.Owner(),"Formatting HFS image","Format image",std::move(complete));
}
