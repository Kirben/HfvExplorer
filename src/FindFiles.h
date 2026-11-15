// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include <vector>
struct FindLocation {bool mac=false;std::string volume,directory,root;int volumeIndex=-1;};
struct FindRequest {FindLocation location;std::string pattern;bool recursive=true,caseSensitive=false,includeFolders=false;unsigned limit=10000;};
struct FindHit {std::string name,parent,path;unsigned long long bytes=0;bool folder=false;};
struct FindResults {std::vector<FindHit> hits;unsigned long long checked=0;unsigned unreadable=0;bool truncated=false;};
bool FindNameMatches(std::string name,std::string pattern,bool caseSensitive=false);
std::function<Storage::Result(Storage::Job&)> MakeFindFilesJob(FindRequest request,std::shared_ptr<FindResults> results);
