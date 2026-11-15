// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
class CdImageSource {
public:
 virtual ~CdImageSource(){}
 virtual bool Open(int drive,std::string& reason)=0;
 virtual unsigned long long Size() const=0;
 virtual bool Verify(DWORD& generation)=0;
 virtual bool Read(unsigned long long offset,DWORD count,void* buffer,DWORD& got)=0;
 virtual DWORD Error() const=0;
};
std::function<Storage::Result(Storage::Job&)> MakeCdImageJob(int drive,std::string output,std::shared_ptr<CdImageSource> source=nullptr);
