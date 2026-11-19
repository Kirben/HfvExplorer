// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
struct LegacyCopyOptions {char output='a',input='a';bool appledouble=true,verifyFloppy=false,targetGenerationKnown=false;DWORD targetGeneration=0;};
LegacyCopyOptions CaptureLegacyCopyOptions(char output);
#ifdef HFV_TESTING
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string,std::string,std::string,std::string,int,int,LegacyCopyOptions);
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string sourceVolume,std::string source,std::string targetVolume,std::string target,int directory,int move,char mode);
#endif

class BrowserSession;
namespace FileOperations { class Location; }
bool StageWindowsLaunchDocument(BrowserSession&,const FileOperations::Location&,CString& output);
