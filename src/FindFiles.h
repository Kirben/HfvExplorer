// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include "FileLocation.h"
#include "BrowserMountInventory.h"
#include <vector>
struct FindScope {
 FileOperations::Location origin,volumeRoot;
 std::vector<uint32_t> catalogRoute;
 std::optional<BrowserMountInventory::Entry> mount;
 std::wstring folderLabel,volumeLabel;
 bool checkWindowsMedia=false;
};
struct FindRequest {
 FindScope scope;
 std::string pattern;
 bool recursive=true,caseSensitive=false,includeFolders=false;
 unsigned limit=10000;
 bool wholeVolume=false;
 const FileOperations::Location& Origin() const {return wholeVolume?scope.volumeRoot:scope.origin;}
 bool UsesUnicode() const {return !Origin().IsClassic();}
};
struct FindHit {std::string name,parent,path;unsigned long long bytes=0;bool folder=false;std::vector<uint32_t> nativePath;
 FileOperations::Location destination, reveal;
 std::optional<BrowserMountInventory::Entry> mount;
};
struct FindResults {std::vector<FindHit> hits;unsigned long long checked=0;unsigned unreadable=0;bool truncated=false,utf8=false;};
bool FindNameMatches(std::wstring name,std::wstring pattern,bool caseSensitive=false);
bool FindNameMatches(std::string name,std::string pattern,bool caseSensitive=false);
std::function<Storage::Result(Storage::Job&)> MakeFindFilesJob(FindRequest request,std::shared_ptr<FindResults> results);
