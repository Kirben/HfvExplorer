// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "VolumeCoordinator.h"
#include "MacBrowserModel.h"
#include "ReadOnlyCdBackend.h"
#include <mutex>
#include <algorithm>
#include <atomic>
#include <set>
namespace VolumeCoord {
namespace {
std::string Utf8(const std::wstring& w){int n=WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),nullptr,0,nullptr,nullptr);std::string s(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,w.data(),int(w.size()),&s[0],n,nullptr,nullptr);return s;}
std::wstring Upper(std::wstring w){if(!w.empty())CharUpperBuffW(&w[0],DWORD(w.size()));return w;}
struct Resource {unsigned readers=0;bool writer=false;uint64_t revision=0;};
struct Bus {std::mutex mutex;std::map<std::string,Resource> resources;std::map<std::string,std::string> hosts;std::vector<std::weak_ptr<Subscription::State>> subscribers;};
}
struct Subscription::State {
 std::mutex mutex;HWND window=nullptr;uintptr_t id=0;bool closed=false,posted=false;
 std::set<std::string> watched;std::map<std::string,uint64_t> pending;
};
namespace {Bus& Get(){static Bus bus;return bus;}std::atomic<uintptr_t> nextId{1};}
std::wstring Wide(const std::string& s){int n=MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,0);if(n)MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),&w[0],n);return w;}
std::string DeviceKey(int drive){wchar_t name[]={wchar_t(L'A'+drive),L':',0},target[4096]{};if(drive>=0&&drive<26&&QueryDosDeviceW(name,target,ARRAYSIZE(target)))return "device:"+Utf8(Upper(target));return "device-drive:"+std::to_string(drive);}
std::string HostKey(const std::wstring& path){
 wchar_t root[32768]{},guid[128]{};
 if(GetVolumePathNameW(path.c_str(),root,ARRAYSIZE(root))){if(GetDriveTypeW(root)==DRIVE_CDROM||((root[0]==L'A'||root[0]==L'B')&&GetDriveTypeW(root)==DRIVE_REMOVABLE))return DeviceKey(towupper(root[0])-L'A');if(GetVolumeNameForVolumeMountPointW(root,guid,ARRAYSIZE(guid)))return "host:"+Utf8(Upper(guid));return "host:"+Utf8(Upper(root));}
 // Missing destinations and UNC shares still have stable volume/root identities.
 if(path.size()>2&&path[1]==L':')return "host-drive:"+Utf8(Upper(path.substr(0,2)));
 if(path.size()>2&&path[0]==L'\\'&&path[1]==L'\\'){auto at=path.find(L'\\',2);at=at==path.npos?at:path.find(L'\\',at+1);return "host:"+Utf8(Upper(path.substr(0,at)));}
 throw std::runtime_error("A volume path must be absolute");
}
std::string ImageKey(const std::wstring& path){
 auto ansi=Utf8(path); // Physical pseudo-images have ASCII drive/root markers.
 if(physical_floppy_path(ansi.c_str())>=0)return DeviceKey(physical_floppy_path(ansi.c_str()));
 if(path.size()>4&&!_wcsicmp(path.c_str()+1,L":\\NP~1_2~3.DSK"))return DeviceKey(towupper(path[0])-L'A');
 HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);std::string key;
 if(h!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};if(GetFileInformationByHandle(h,&info))key="image:"+std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);CloseHandle(h);}
 if(key.empty()){wchar_t full[32768]{};DWORD n=GetFullPathNameW(path.c_str(),ARRAYSIZE(full),full,nullptr);if(!n||n>=ARRAYSIZE(full))throw std::runtime_error("Cannot identify image path");key="image-path:"+Utf8(Upper(full));}
 auto host=HostKey(path);auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);bus.hosts[key]=host;return key;
}
Lease::Lease(std::vector<Access> values){
 std::map<std::string,Mode> unique;for(auto& a:values)if(!a.key.empty()){auto found=unique.find(a.key);if(found==unique.end()||a.mode==Mode::Write)unique[a.key]=a.mode;}
 for(auto& a:unique)access_.push_back({a.first,a.second});
 auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);
 for(auto& a:unique){auto& resource=bus.resources[a.first];if(resource.writer||(a.second==Mode::Write&&resource.readers))throw Busy();}
 for(auto& a:unique){auto& resource=bus.resources[a.first];if(a.second==Mode::Write)resource.writer=true;else ++resource.readers;}
}
Lease::~Lease(){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);for(auto& a:access_){auto& resource=bus.resources[a.key];if(a.mode==Mode::Write)resource.writer=false;else --resource.readers;}}
bool Readable(const std::string& key){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);auto found=bus.resources.find(key);return found==bus.resources.end()||!found->second.writer;}
uint64_t Revision(const std::string& key){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);return bus.resources[key].revision;}
void Changed(const std::vector<std::string>& keys){
 auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);std::set<std::string> all(keys.begin(),keys.end());for(auto& key:keys){auto host=bus.hosts.find(key);if(host!=bus.hosts.end())all.insert(host->second);}
 std::vector<Event> events;for(auto& key:all)if(!key.empty())events.push_back({key,++bus.resources[key].revision});
 for(auto it=bus.subscribers.begin();it!=bus.subscribers.end();){auto state=it->lock();if(!state){it=bus.subscribers.erase(it);continue;}++it;
  std::lock_guard<std::mutex> mailbox(state->mutex);if(state->closed)continue;for(auto& event:events)if(state->watched.count(event.key))state->pending[event.key]=event.revision;
  if(!state->pending.empty()&&!state->posted&&state->window){state->posted=PostMessage(state->window,Notification,WPARAM(state->id),0)!=FALSE;}
 }
}
std::shared_ptr<MacBrowser::Volume> SharedImage(const std::string& key,std::function<std::shared_ptr<MacBrowser::Volume>()> open){
 static std::mutex mutex;static std::map<std::string,std::weak_ptr<MacBrowser::Volume>> images;
 std::lock_guard<std::mutex> lock(mutex);auto existing=images[key].lock();if(existing)return existing;auto volume=open();if(volume)images[key]=volume;return volume;
}
Subscription::Subscription():state_(new State){state_->id=nextId++;auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);bus.subscribers.push_back(state_);}
Subscription::~Subscription(){Close();}
void Subscription::Bind(HWND window){std::lock_guard<std::mutex> lock(state_->mutex);state_->window=window;state_->closed=false;state_->posted=false;if(window&&!state_->pending.empty())state_->posted=PostMessage(window,Notification,WPARAM(state_->id),0)!=FALSE;}
void Subscription::Close(){std::lock_guard<std::mutex> lock(state_->mutex);state_->closed=true;state_->window=nullptr;state_->pending.clear();state_->watched.clear();state_->posted=false;}
void Subscription::Watch(const std::string& key){if(key.empty())return;std::lock_guard<std::mutex> lock(state_->mutex);state_->watched.insert(key);}
uintptr_t Subscription::Id()const{return state_->id;}
std::vector<Event> Subscription::Drain(){std::lock_guard<std::mutex> lock(state_->mutex);std::vector<Event> out;for(auto& event:state_->pending)out.push_back({event.first,event.second});state_->pending.clear();state_->posted=false;return out;}
}
