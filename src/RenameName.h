// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
inline bool HfvValidRenameLeaf(const char* name,bool mac) {
 if(!name||!*name||!strcmp(name,".")||!strcmp(name,".."))return false;
 size_t n=strlen(name);
 if(mac)return n<=31&&!strchr(name,':');
 if(n>=260||name[n-1]=='.'||name[n-1]==' ')return false;
 for(const unsigned char* p=reinterpret_cast<const unsigned char*>(name);*p;++p)
  if(*p<32||strchr("\\/:*?\"<>|",*p))return false;
 return true;
}
