// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include "FileLocation.h"
#include <vector>
namespace MacBrowser {class Volume;}
struct FindLocation {bool mac=false;std::string volume,directory,root;int volumeIndex=-1;std::shared_ptr<MacBrowser::Volume> native;uint32_t nativeFolder=2;std::vector<uint32_t> nativePath;FileOperations::Location origin;bool checkWindowsMedia=false; std::wstring windowsRoot; bool UsesUnicode() const { return native || !mac; }};
struct FindRequest {FindLocation location;std::string pattern;bool recursive=true,caseSensitive=false,includeFolders=false;unsigned limit=10000;};
struct FindHit {std::string name,parent,path;unsigned long long bytes=0;bool folder=false;std::vector<uint32_t> nativePath;
 FileOperations::Location destination, reveal;
};
struct FindResults {std::vector<FindHit> hits;unsigned long long checked=0;unsigned unreadable=0;bool truncated=false,utf8=false;};
bool FindNameMatches(std::wstring name,std::wstring pattern,bool caseSensitive=false);
bool FindNameMatches(std::string name,std::string pattern,bool caseSensitive=false);
std::function<Storage::Result(Storage::Job&)> MakeFindFilesJob(FindRequest request,std::shared_ptr<FindResults> results);
