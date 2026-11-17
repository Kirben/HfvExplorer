// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <functional>
#include <cstring>
#include <algorithm>
namespace CdMedia {
struct MacVolume {WORD signature=0;ULONGLONG offset=0,bytes=0;bool discAddressed=false;};
inline WORD Word(const BYTE* p){return WORD(p[0])*256+p[1];}
inline DWORD Long(const BYTE* p){return (DWORD(p[0])<<24)|(DWORD(p[1])<<16)|(DWORD(p[2])<<8)|p[3];}
// Probe only the selected track. Accept APM offsets either relative to that
// track or absolute to the disc, without rewriting on-disc partition records.
inline bool ProbeMac(ULONGLONG size,ULONGLONG origin,const std::function<bool(ULONGLONG,DWORD,void*)>& read,MacVolume& out){
 auto header=[&](ULONGLONG at,ULONGLONG length,bool absolute){if(at>size||size-at<1536||length<1536)return false;BYTE h[512];if(!read(at+1024,sizeof(h),h))return false;WORD sig=Word(h);if(sig!=0x4244&&sig!=0x482b&&sig!=0x4858)return false;if((sig==0x482b&&Word(h+2)!=4)||(sig==0x4858&&Word(h+2)!=5))return false;
  if(sig==0x4244&&(!Word(h+18)||!Long(h+20)||Long(h+20)%512||!h[36]||h[36]>27))return false;
  out.signature=sig;out.offset=at;out.bytes=(std::min)(length,size-at);out.discAddressed=absolute;return true;};
 if(header(0,size,false))return true;if(size<1024)return false;BYTE first[512];if(!read(0,512,first))return false;DWORD block=Word(first)==0x4552?Word(first+2):512;
 if(block!=512&&block!=2048&&block!=4096)return false;if(block>size-512||!read(block,512,first)||Word(first)!=0x504d)return false;DWORD count=Long(first+4);if(!count||count>4096||ULONGLONG(count+1)*block>size)return false;
 for(DWORD n=1;n<=count;++n){BYTE p[512];if(!read(ULONGLONG(n)*block,512,p)||Word(p)!=0x504d)return false;
  if(std::memcmp(p+48,"Apple_HFS",10)&&std::memcmp(p+48,"Apple_HFSX",11))continue;ULONGLONG at=ULONGLONG(Long(p+8))*block,length=ULONGLONG(Long(p+12))*block;
  if(header(at,length,false))return true;if(origin&&at>=origin&&header(at-origin,length,true))return true;
 }return false;
}
}
