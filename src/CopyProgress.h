// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
struct LegacyCopyOptions {char output='a',input='a';bool appledouble=true;};
LegacyCopyOptions CaptureLegacyCopyOptions(char output);
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string,std::string,std::string,std::string,int,int,LegacyCopyOptions);
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string sourceVolume,std::string source,std::string targetVolume,std::string target,int directory,int move,char mode);
int CopyWithProgress(char*,char*,char*,char*,int,int,int*,CWnd* owner=nullptr);
