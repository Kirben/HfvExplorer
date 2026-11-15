// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include <vector>
class CWnd;
class FloppyWriteApi;
std::function<Storage::Result(Storage::Job&)> MakeFloppyWriteJob(int drive,std::vector<BYTE> image,FloppyWriteApi* api=nullptr);
void WriteFloppyImage(CWnd* owner,int drive,const std::vector<BYTE>& image,const char* description,void (*releaseDrive)(int,void*),void* context);
