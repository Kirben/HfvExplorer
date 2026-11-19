// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"

namespace TransferFixture {
inline FileOperations::Request Request(const std::string& fromVolume,const std::string& from,
    const std::string& toVolume,const std::string& to,bool directory,bool move,char mode)
{
    auto source=FileOperations::Location::FromLegacy(fromVolume,from);
    auto destination=FileOperations::Location::FromLegacy(toVolume,to);
    return FileOperations::Request(FileOperations::LegacyConversion{
        std::move(source),std::move(destination),directory,move,false,CaptureLegacyCopyOptions(mode)});
}

template<class... Args>
std::shared_ptr<Storage::Job> Submit(Args&&... args)
{
    return FileOperations::Submit(Request(std::forward<Args>(args)...)).Job();
}

// Device and fault fixtures must stay in-process to share injected APIs.
template<class... Args>
auto Work(Args&&... args)
{
    return [request=Request(std::forward<Args>(args)...)](Storage::Job& job) {
        FileOperations::Completion result;
        return FileOperations::ExecuteInWorker(request,job,result);
    };
}
}
