// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
std::function<Storage::Result(Storage::Job&)> MakeCopyJob(std::string sourceVolume,std::string source,std::string targetVolume,std::string target,int directory,int move,char mode);
int CopyWithProgress(char*,char*,char*,char*,int,int,int*);