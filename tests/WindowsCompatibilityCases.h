// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "WindowsCompatibility.h"
inline bool VerifyWindowsCompatibilityPolicy(){
 using namespace WindowsCompatibility;
 struct Case {DWORD major,minor;WORD sp;bool dll,expected;};
 const Case cases[]={{5,2,2,true,false},{6,0,2,true,false},{6,1,0,true,false},{6,1,1,false,false},{6,1,1,true,true},{6,1,2,true,true},{6,2,0,false,true},{6,3,0,false,true},{10,0,0,true,true},{11,0,0,true,true}};
 for(auto c:cases){Version v;v.major=c.major;v.minor=c.minor;v.servicePack=c.sp;v.secureDllSearch=c.dll;v.known=true;if(Supported(v)!=c.expected)return false;}
 return !Supported(Version{});
}
