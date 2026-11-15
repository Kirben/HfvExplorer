// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <stdio.h>
#if _MSC_VER != 1944
#error Use the pinned v143 14.44 toolset and matching static MFC/CRT for Windows 7 compatibility.
#endif
static_assert(sizeof(void*)==8,"This compatibility build targets Windows x64.");
namespace WindowsCompatibility {
struct Version {DWORD major=0,minor=0,build=0;WORD servicePack=0;bool secureDllSearch=false,known=false;};
inline bool Supported(const Version& v){
 if(!v.known)return false;
 if(v.major==6&&v.minor==1)return v.servicePack>=1&&v.secureDllSearch;
 return (v.major==6&&(v.minor==2||v.minor==3))||v.major>=10;
}
inline const char* Requirement(){return "HFVExplorer requires Windows 7 SP1 with KB2533623 or a superseding update, Windows 8/8.1, Windows 10 or Windows 11 (x64).";}
inline Version Current(){
 Version v;auto ntdll=GetModuleHandleW(L"ntdll.dll");auto kernel=GetModuleHandleW(L"kernel32.dll");
 typedef LONG (WINAPI* GetVersion)(OSVERSIONINFOW*);
 auto query=reinterpret_cast<GetVersion>(GetProcAddress(ntdll,"RtlGetVersion"));OSVERSIONINFOEXW info={};info.dwOSVersionInfoSize=sizeof(info);
 if(query&&query(reinterpret_cast<OSVERSIONINFOW*>(&info))==0){v.major=info.dwMajorVersion;v.minor=info.dwMinorVersion;v.build=info.dwBuildNumber;v.servicePack=info.wServicePackMajor;v.known=true;}
 v.secureDllSearch=GetProcAddress(kernel,"AddDllDirectory")&&GetProcAddress(kernel,"RemoveDllDirectory")&&GetProcAddress(kernel,"SetDefaultDllDirectories");return v;
}
inline void Report(FILE* file,const Version& v){
 fprintf(file,"Windows version: %lu.%lu.%lu; service pack: %u\nSecure DLL search API set: %s\nCompiler: MSVC %d\nTarget: x64; WINVER=0x%04x; _WIN32_WINNT=0x%04x\nRuntime prerequisite: %s\n%s\n",v.major,v.minor,v.build,v.servicePack,v.secureDllSearch?"present":"absent",_MSC_VER,WINVER,_WIN32_WINNT,Supported(v)?"PASS":"FAIL",Requirement());
}
}
