// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include <objidl.h>
#include <vector>
struct CdRecorder {std::wstring id;std::string name;};
struct CdBurnMedia {bool supported=false,blank=false;unsigned long long freeSectors=0;};
struct CdBurnPlan {
 std::string image;std::wstring recorder;
 unsigned long long bytes=0;BY_HANDLE_FILE_INFORMATION identity={};
};
// Injectable recorder boundary. Tests never create the native implementation.
class CdBurnBackend {
public:
 virtual ~CdBurnBackend(){}
 virtual HRESULT Open(const std::wstring& recorder)=0;
 virtual HRESULT Inspect(CdBurnMedia& media)=0;
 virtual HRESULT ConfigureVerifiedFinalizedWrite()=0;
 virtual HRESULT Write(IStream* image,Storage::Job& job)=0;
};
std::function<Storage::Result(Storage::Job&)> ListCdRecorders(std::shared_ptr<std::vector<CdRecorder>> output);
std::function<Storage::Result(Storage::Job&)> PrepareCdBurn(std::string image,std::wstring recorder,std::shared_ptr<CdBurnPlan> output,std::shared_ptr<CdBurnBackend> backend=nullptr);
std::function<Storage::Result(Storage::Job&)> MakeCdBurnJob(CdBurnPlan plan,std::shared_ptr<CdBurnBackend> backend=nullptr);
