// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "ReadOnlySource.h"
#include <atomic>
#include <mutex>
namespace ReadOnlySources {
namespace {std::mutex mutex;std::map<std::string,std::shared_ptr<HfsPlus::Source>> sources;std::atomic<uint64_t> next{1};}
Registration::Registration(std::shared_ptr<HfsPlus::Source> source,bool writable){if(!source||source->Writable()!=writable)throw HfsPlus::Error("Unexpected access for registered source");path_="HFVREADONLY:"+std::to_string(next.fetch_add(1));std::lock_guard<std::mutex> lock(mutex);sources.emplace(path_,std::move(source));}
Registration::~Registration(){std::lock_guard<std::mutex> lock(mutex);sources.erase(path_);}
bool IsPath(const char* path){return path&&!strncmp(path,"HFVREADONLY:",12);}
std::shared_ptr<HfsPlus::Source> Find(const char* path){std::lock_guard<std::mutex> lock(mutex);auto i=sources.find(path);return i==sources.end()?nullptr:i->second;}
}
