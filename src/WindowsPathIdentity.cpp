// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "WindowsPathIdentity.h"
#include "VolumeCoordinator.h"
#include <algorithm>
#include <vector>
namespace VolumeCoord { namespace Detail {
namespace {
std::wstring Upper(std::wstring value){if(!value.empty())CharUpperBuffW(&value[0],DWORD(value.size()));return value;}
std::string Utf8(const std::wstring& value){int n=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);std::string text(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),&text[0],n,nullptr,nullptr);return text;}
std::wstring Root(const std::wstring& path){
 if(path.size()>4&&path.substr(0,4)==L"\\\\?\\"){
  if(path.size()>8&&_wcsnicmp(path.c_str()+4,L"UNC\\",4)==0){auto at=path.find(L'\\',8);at=at==path.npos?at:path.find(L'\\',at+1);return at==path.npos?path+L"\\":path.substr(0,at+1);}
  auto at=path.find(L'\\',4);return at==path.npos?path+L"\\":path.substr(0,at+1);
 }
 if(path.size()>2&&path[1]==L':')return path.substr(0,3);
 if(path.size()>2&&path[0]==L'\\'&&path[1]==L'\\'){auto at=path.find(L'\\',2);at=at==path.npos?at:path.find(L'\\',at+1);return at==path.npos?path+L"\\":path.substr(0,at+1);}return {};
}
std::wstring Parent(const std::wstring& path){auto root=Root(path);if(path.size()<=root.size())return {};auto at=path.find_last_of(L'\\');if(at==path.npos)return {};return path.substr(0,(std::max)(at,root.size()));}
std::wstring Trim(std::wstring path){auto root=Root(path);while(path.size()>root.size()&&path.back()==L'\\')path.pop_back();return path;}
struct Handle{HANDLE h=INVALID_HANDLE_VALUE;explicit Handle(const std::wstring& path):h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)){}~Handle(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
std::wstring Final(HANDLE h){std::vector<wchar_t> text(32768);DWORD n=GetFinalPathNameByHandleW(h,text.data(),DWORD(text.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_GUID);if(!n)n=GetFinalPathNameByHandleW(h,text.data(),DWORD(text.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!n||n>=text.size())return {};return Trim(Upper(std::wstring(text.data(),n)));}
std::string Id(const std::wstring& canonical,const BY_HANDLE_FILE_INFORMATION& info){return Utf8(Upper(Root(canonical)))+"|"+std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow);}
bool Below(const std::string& path,const std::string& parent){return path==parent||(path.size()>parent.size()&&path.compare(0,parent.size(),parent)==0&&(parent.back()=='\\'||path[parent.size()]=='\\'));}
void Ancestors(PathIdentity& out,const std::wstring& canonical){for(auto at=Parent(canonical);!at.empty();at=Parent(at)){Handle handle(at);BY_HANDLE_FILE_INFORMATION info{};if(handle.h!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(handle.h,&info))out.ancestors.insert(Id(at,info));}}
}
PathIdentity DescribePath(const std::wstring& input){
 PathIdentity out;auto broad=HostKey(input);out.hosts.insert(broad);
 // Native sector media keep whole-device coordination, including all paths.
 if(broad.compare(0,7,"device:")==0||broad.compare(0,13,"device-drive:")==0){out.namespaceName=broad;out.fingerprint=broad;out.devices.insert(broad);return out;}
 std::vector<wchar_t> full(32768);DWORD n=GetFullPathNameW(input.c_str(),DWORD(full.size()),full.data(),nullptr);if(!n||n>=full.size())throw std::runtime_error("Cannot identify Windows path");std::wstring path(full.data(),n);std::replace(path.begin(),path.end(),L'/',L'\\');path=Trim(path);
 // An alternate data stream modifies its containing file, not an independent entry.
 auto colon=path.find(L':',path.find_last_of(L'\\')+1);if(colon!=path.npos&&colon!=1)path.resize(colon);
 wchar_t mount[32768]{};if(GetVolumePathNameW(path.c_str(),mount,ARRAYSIZE(mount))){auto type=GetDriveTypeW(mount);if(type==DRIVE_REMOVABLE||type==DRIVE_CDROM){if(mount[1]==L':')out.devices.insert(DeviceKey(towupper(mount[0])-L'A'));}}
 std::wstring probe=path,suffix;DWORD links=1;bool resolved=false;
 for(unsigned depth=0;depth<1024&&!probe.empty();++depth){
  Handle handle(probe);if(handle.h!=INVALID_HANDLE_VALUE){BY_HANDLE_FILE_INFORMATION info{};auto canonical=Final(handle.h);if(!canonical.empty()&&GetFileInformationByHandle(handle.h,&info)){
    auto complete=Trim(Upper(canonical+(suffix.empty()?L"":(canonical.back()==L'\\'?L"":L"\\")+suffix)));out.names.insert(Utf8(complete));out.namespaceName=Utf8(complete);out.directory=suffix.empty()&&(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    auto object=Id(canonical,info);if(suffix.empty()){out.object=object;links=info.nNumberOfLinks;}else out.ancestors.insert(object);
    Ancestors(out,canonical);try{out.hosts.insert(HostKey(canonical));}catch(const std::exception&){}resolved=true;break;
   }}
  DWORD attributes=GetFileAttributesW(probe.c_str()),failure=GetLastError();
  if(attributes!=INVALID_FILE_ATTRIBUTES||(failure!=ERROR_FILE_NOT_FOUND&&failure!=ERROR_PATH_NOT_FOUND)){out.names.clear();out.namespaceName=out.devices.empty()?broad:*out.devices.begin();out.fingerprint=out.namespaceName;return out;}
  auto parent=Parent(probe);if(parent.empty()||parent==probe)break;auto leaf=probe.substr(parent.size());while(!leaf.empty()&&leaf.front()==L'\\')leaf.erase(leaf.begin());
  // Ordinary Win32 names trim trailing dots/spaces before namespace lookup.
  if(input.compare(0,4,L"\\\\?\\")!=0)while(!leaf.empty()&&(leaf.back()==L'.'||leaf.back()==L' '))leaf.pop_back();
  suffix=leaf+(suffix.empty()?L"":L"\\"+suffix);probe=parent;
 }
 if(!resolved){out.namespaceName=out.devices.empty()?broad:*out.devices.begin();out.fingerprint=out.namespaceName;return out;} // Fail closed when metadata cannot be resolved.
 if(links>1&&!out.directory){
  DWORD length=32768;std::vector<wchar_t> name(length);HANDLE find=FindFirstFileNameW(path.c_str(),0,&length,name.data());
  if(find==INVALID_HANDLE_VALUE)out.unknownLinks=true;
  else{auto root=Root(std::wstring(full.data(),n)); // Canonical volume prefix, not a mapped-drive spelling.
   std::wstring canonicalProbe;{Handle h(path);canonicalProbe=Final(h.h);}root=Root(canonicalProbe);
   unsigned count=0;for(;;){std::wstring leaf=name.data();while(!leaf.empty()&&leaf.front()==L'\\')leaf.erase(leaf.begin());auto alias=Trim(Upper(root+leaf));out.names.insert(Utf8(alias));Ancestors(out,alias);if(++count>65536){out.unknownLinks=true;break;}length=DWORD(name.size());if(!FindNextFileNameW(find,&length,name.data())){if(GetLastError()!=ERROR_HANDLE_EOF)out.unknownLinks=true;break;}}FindClose(find);
  }
 }
 out.fingerprint=Fingerprint(out);return out;
}
std::string Fingerprint(const PathIdentity& out){auto value=out.object+"|"+std::to_string(out.directory)+"|"+std::to_string(out.unknownLinks);for(const auto& name:out.names)value+="|N"+name;for(const auto& id:out.ancestors)value+="|A"+id;for(const auto& host:out.hosts)value+="|H"+host;for(const auto& device:out.devices)value+="|D"+device;return value;}

bool PathContains(const PathIdentity& a,const PathIdentity& b){
 if(!a.object.empty()&&(a.object==b.object||b.ancestors.count(a.object)))return true;
 for(const auto& parent:a.names)for(const auto& child:b.names)if(Below(child,parent))return true;
 if(a.unknownLinks&&b.directory)for(const auto& host:a.hosts)if(b.hosts.count(host))return true;return false;
}
bool PathOverlap(const PathIdentity& a,const PathIdentity& b){
 if(!a.object.empty()&&(a.object==b.object||b.ancestors.count(a.object)))return true;
 if(!b.object.empty()&&a.ancestors.count(b.object))return true;
 for(const auto& x:a.names)for(const auto& y:b.names)if(Below(x,y)||Below(y,x))return true;
 if((a.unknownLinks&&b.directory)||(b.unknownLinks&&a.directory))for(const auto& host:a.hosts)if(b.hosts.count(host))return true;
 return false;
}
}}
