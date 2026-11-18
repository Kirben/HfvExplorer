// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "VolumeCoordinator.h"
#include "WindowsPathIdentity.h"
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
struct Resource {unsigned readers=0,browsers=0;bool writer=false;DWORD writerThread=0;uint64_t revision=0;};
struct Bus {std::mutex mutex;std::map<std::string,Resource> resources;std::map<std::string,std::string> hosts;std::map<std::string,std::set<std::string>> imagePaths;std::map<std::pair<std::string,std::string>,unsigned> dependencies;std::map<std::string,Detail::PathIdentity> paths;std::map<std::string,std::string> pathKeys;uint64_t nextPath=1;std::vector<std::weak_ptr<Subscription::State>> subscribers;};
}
struct Subscription::State {
 std::mutex mutex;HWND window=nullptr;uintptr_t id=0;bool closed=false,posted=false;
 std::set<std::string> watched;std::map<std::string,uint64_t> pending;
};
namespace {Bus& Get(){static Bus bus;return bus;}std::atomic<uintptr_t> nextId{1};}
std::wstring Wide(const std::string& s){int n=MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),nullptr,0);std::wstring w(n,0);if(n)MultiByteToWideChar(CP_ACP,0,s.data(),int(s.size()),&w[0],n);return w;}
std::string DeviceKey(int drive){wchar_t name[]={wchar_t(L'A'+drive),L':',0},target[4096]{};if(drive>=0&&drive<26&&QueryDosDeviceW(name,target,ARRAYSIZE(target)))return "device:"+Utf8(Upper(target));return "device-drive:"+std::to_string(drive);}
std::string HostKey(const std::wstring& input){
 std::wstring path=input;if(path.size()>8&&_wcsnicmp(path.c_str(),L"\\\\?\\UNC\\",8)==0)path=L"\\\\"+path.substr(8);
 wchar_t root[32768]{},guid[128]{};
 if(GetVolumePathNameW(path.c_str(),root,ARRAYSIZE(root))){if(GetDriveTypeW(root)==DRIVE_CDROM||((root[0]==L'A'||root[0]==L'B')&&GetDriveTypeW(root)==DRIVE_REMOVABLE))return DeviceKey(towupper(root[0])-L'A');if(GetVolumeNameForVolumeMountPointW(root,guid,ARRAYSIZE(guid)))return "host:"+Utf8(Upper(guid));return "host:"+Utf8(Upper(root));}
 // Missing destinations and UNC shares still have stable volume/root identities.
 if(path.size()>2&&path[1]==L':')return "host-drive:"+Utf8(Upper(path.substr(0,2)));
 if(path.size()>2&&path[0]==L'\\'&&path[1]==L'\\'){auto at=path.find(L'\\',2);at=at==path.npos?at:path.find(L'\\',at+1);return "host:"+Utf8(Upper(path.substr(0,at)));}
 throw std::runtime_error("A volume path must be absolute");
}
namespace {
std::string RegisterPath(Bus& bus,Detail::PathIdentity identity){if(identity.names.empty())return identity.namespaceName;auto found=bus.pathKeys.find(identity.fingerprint);if(found!=bus.pathKeys.end())return found->second;auto key="windows-path:"+std::to_string(bus.nextPath++);bus.pathKeys[identity.fingerprint]=key;bus.paths.emplace(key,std::move(identity));return key;}
}
#ifdef HFV_TESTING
namespace {std::mutex observerMutex;std::function<void(const std::wstring&)> pathObserver;}
void SetPathLookupObserver(std::function<void(const std::wstring&)> value){std::lock_guard<std::mutex> lock(observerMutex);pathObserver=std::move(value);}
#endif
std::string PathKey(const std::wstring& path){
#ifdef HFV_TESTING
 std::function<void(const std::wstring&)> observe;{std::lock_guard<std::mutex> lock(observerMutex);observe=pathObserver;}if(observe)observe(path);
#endif
 auto identity=Detail::DescribePath(path);auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);return RegisterPath(bus,std::move(identity));
}
std::string ObservedPathKey(const std::string& parent,const std::wstring& leaf,const std::string& identity,bool directory){
 auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);auto found=bus.paths.find(parent);if(found==bus.paths.end())return parent;const auto& root=found->second;Detail::PathIdentity value;value.hosts=root.hosts;value.devices=root.devices;value.ancestors=root.ancestors;if(!root.object.empty())value.ancestors.insert(root.object);value.directory=directory;
 for(auto name:root.names){if(!name.empty()&&name.back()!='\\')name+='\\';value.names.insert(name+Utf8(Upper(leaf)));}
 size_t at=0;for(int n=0;n<3;++n){at=identity.find(':',at);if(at==identity.npos)break;if(n<2)++at;}auto separator=root.object.find('|');if(at!=identity.npos&&separator!=root.object.npos)value.object=root.object.substr(0,separator+1)+identity.substr(0,at);value.fingerprint=Detail::Fingerprint(value);return RegisterPath(bus,std::move(value));
}
std::string LegacyOutputKey(const std::wstring& path){DWORD attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY))return PathKey(path);auto at=path.find_last_of(L"\\/");if(at==path.npos)throw std::runtime_error("Use an absolute Windows destination path");return PathKey(path.substr(0,at==2?3:at));}
namespace {
bool Related(const Bus& bus,const std::string& a,const std::string& b){
 if(a==b)return true;
 auto pa=bus.paths.find(a),pb=bus.paths.find(b);
 if(pa!=bus.paths.end()&&pb!=bus.paths.end())return Detail::PathOverlap(pa->second,pb->second);
 if(pa!=bus.paths.end())return pa->second.hosts.count(b)||pa->second.devices.count(b);
 if(pb!=bus.paths.end())return pb->second.hosts.count(a)||pb->second.devices.count(a);
 return false;
}
bool Contains(const Bus& bus,const std::string& a,const std::string& b){
 if(a==b)return true;auto pa=bus.paths.find(a),pb=bus.paths.find(b);if(pa!=bus.paths.end()&&pb!=bus.paths.end())return Detail::PathContains(pa->second,pb->second);
 auto ia=bus.imagePaths.find(a),ib=bus.imagePaths.find(b);if(ia!=bus.imagePaths.end()){for(const auto& key:ia->second)if(Contains(bus,key,b))return true;return false;}if(ib!=bus.imagePaths.end()){for(const auto& key:ib->second)if(Contains(bus,a,key))return true;return false;}
 return pa==bus.paths.end()&&pb!=bus.paths.end()&&(pb->second.hosts.count(a)||pb->second.devices.count(a));
}
bool Overlap(const Bus& bus,const std::string& a,const std::string& b){if(Related(bus,a,b))return true;auto ia=bus.imagePaths.find(a),ib=bus.imagePaths.find(b);if(ia!=bus.imagePaths.end()){for(const auto& key:ia->second){if(Related(bus,key,b))return true;if(ib!=bus.imagePaths.end())for(const auto& other:ib->second)if(Related(bus,key,other))return true;}}if(ib!=bus.imagePaths.end())for(const auto& key:ib->second)if(Related(bus,a,key))return true;return false;}
}
std::string ImageKey(const std::wstring& path){
 auto ansi=Utf8(path); // Physical pseudo-images have ASCII drive/root markers.
 if(physical_floppy_path(ansi.c_str())>=0)return DeviceKey(physical_floppy_path(ansi.c_str()));
 if(path.size()>4&&!_wcsicmp(path.c_str()+1,L":\\NP~1_2~3.DSK"))return DeviceKey(towupper(path[0])-L'A');
 HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);std::string key;
 if(h!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};if(GetFileInformationByHandle(h,&info))key="image:"+std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);CloseHandle(h);}
 if(key.empty()){wchar_t full[32768]{};DWORD n=GetFullPathNameW(path.c_str(),ARRAYSIZE(full),full,nullptr);if(!n||n>=ARRAYSIZE(full))throw std::runtime_error("Cannot identify image path");key="image-path:"+Utf8(Upper(full));}
 auto host=HostKey(path),file=PathKey(path);auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);bus.hosts[key]=host;bus.imagePaths[key].insert(file);return key;
}
ImageDependency::ImageDependency(const std::string& image,const std::wstring& path):image_(image),path_(PathKey(path)){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);++bus.dependencies[{image_,path_}];bus.imagePaths[image_].insert(path_);}
ImageDependency::~ImageDependency(){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);auto i=bus.dependencies.find({image_,path_});if(i!=bus.dependencies.end()&&!--i->second){bus.dependencies.erase(i);bus.imagePaths[image_].erase(path_);}}
Lease::Lease(std::vector<Access> values):Lease(std::move(values),false){}
Lease::Lease(std::vector<Access> values,bool reuseWriter){
 std::map<std::string,Mode> unique;for(auto& a:values)if(!a.key.empty()){auto found=unique.find(a.key);if(found==unique.end()||a.mode==Mode::Write||(a.mode==Mode::Read&&found->second==Mode::Browse))unique[a.key]=a.mode;}
 auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);auto thread=GetCurrentThreadId();
 for(auto at=unique.begin();at!=unique.end();){auto held=bus.resources.find(at->first);if(reuseWriter&&held!=bus.resources.end()&&held->second.writer&&held->second.writerThread==thread)at=unique.erase(at);else ++at;}
 for(const auto& a:unique)for(const auto& held:bus.resources){auto& r=held.second;if(!r.writer&&!r.readers&&!r.browsers)continue;if(reuseWriter&&r.writer&&r.writerThread==thread)continue;if((r.writer&&(a.second==Mode::Browse?Contains(bus,held.first,a.first):Overlap(bus,a.first,held.first)))||(a.second==Mode::Write&&((r.readers&&Overlap(bus,a.first,held.first))||(r.browsers&&Contains(bus,a.first,held.first)))))throw Busy();}
 for(const auto& a:unique){access_.push_back({a.first,a.second});auto& resource=bus.resources[a.first];if(a.second==Mode::Write){resource.writer=true;resource.writerThread=thread;}else if(a.second==Mode::Browse)++resource.browsers;else ++resource.readers;}
}
Lease::~Lease(){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);for(auto& a:access_){auto& resource=bus.resources[a.key];if(a.mode==Mode::Write)resource.writer=false;else if(a.mode==Mode::Browse)--resource.browsers;else --resource.readers;}}
BackendRead::BackendRead(const std::string& key){lease_.reset(new Lease({{key,Mode::Read}},true));}
bool Overlapping(const std::vector<std::string>& keys,const std::string& key){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);for(auto& held:keys)if(Overlap(bus,held,key))return true;return false;}
bool Available(const std::vector<Access>& access){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);for(const auto& a:access)for(const auto& held:bus.resources){const auto& r=held.second;if((r.writer&&(a.mode==Mode::Browse?Contains(bus,held.first,a.key):Overlap(bus,a.key,held.first)))||(a.mode==Mode::Write&&((r.readers&&Overlap(bus,a.key,held.first))||(r.browsers&&Contains(bus,a.key,held.first)))))return false;}return true;}
bool Readable(const std::string& key){return Available({{key,Mode::Read}});}
OperationLease::OperationLease(std::vector<Access> access){lease_.reset(new Lease(std::move(access),true));}
uint64_t Revision(const std::string& key){auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);return bus.resources[key].revision;}
void Changed(const std::vector<std::string>& keys){
 auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);for(const auto& key:keys)if(!key.empty())bus.resources[key];std::vector<Event> events;
 for(auto& resource:bus.resources)for(const auto& key:keys)if(!key.empty()&&Overlap(bus,resource.first,key)){events.push_back({resource.first,++resource.second.revision});break;}
 for(auto it=bus.subscribers.begin();it!=bus.subscribers.end();){auto state=it->lock();if(!state){it=bus.subscribers.erase(it);continue;}++it;std::lock_guard<std::mutex> mailbox(state->mutex);if(state->closed)continue;for(const auto& event:events)if(state->watched.count(event.key))state->pending[event.key]=event.revision;
  if(!state->pending.empty()&&!state->posted&&state->window)state->posted=PostMessage(state->window,Notification,WPARAM(state->id),0)!=FALSE;
 }
}

std::shared_ptr<MacBrowser::Volume> SharedImage(const std::string& key,std::function<std::shared_ptr<MacBrowser::Volume>()> open){
 struct Slot{std::mutex mutex;std::weak_ptr<MacBrowser::Volume> volume;};
 static std::mutex mutex;static std::map<std::string,std::shared_ptr<Slot>> images;std::shared_ptr<Slot> slot;
 {std::lock_guard<std::mutex> lock(mutex);auto& value=images[key];if(!value)value=std::make_shared<Slot>();slot=value;}
 std::lock_guard<std::mutex> lock(slot->mutex);auto existing=slot->volume.lock();if(existing)return existing;auto volume=open();if(volume)slot->volume=volume;return volume;
}
Subscription::Subscription():state_(new State){state_->id=nextId++;auto& bus=Get();std::lock_guard<std::mutex> lock(bus.mutex);bus.subscribers.push_back(state_);}
Subscription::~Subscription(){Close();}
void Subscription::Bind(HWND window){std::lock_guard<std::mutex> lock(state_->mutex);state_->window=window;state_->closed=false;state_->posted=false;if(window&&!state_->pending.empty())state_->posted=PostMessage(window,Notification,WPARAM(state_->id),0)!=FALSE;}
void Subscription::Close(){std::lock_guard<std::mutex> lock(state_->mutex);state_->closed=true;state_->window=nullptr;state_->pending.clear();state_->watched.clear();state_->posted=false;}
void Subscription::Watch(const std::string& key){if(key.empty())return;auto& bus=Get();std::lock_guard<std::mutex> resource(bus.mutex);bus.resources[key];std::lock_guard<std::mutex> lock(state_->mutex);state_->watched.insert(key);}
uintptr_t Subscription::Id()const{return state_->id;}
std::vector<Event> Subscription::Drain(){std::lock_guard<std::mutex> lock(state_->mutex);std::vector<Event> out;for(auto& event:state_->pending)out.push_back({event.first,event.second});state_->pending.clear();state_->posted=false;return out;}
}
