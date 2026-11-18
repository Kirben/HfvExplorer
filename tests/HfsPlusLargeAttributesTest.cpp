// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include "HfsPlusDisk.h"
#include "AttributeReadScope.h"
#include <map>
#include <cstring>
#include <cstdio>
using namespace HfsPlus;
namespace {
void Check(bool ok,const char* message){if(!ok)throw Error(message);}
struct Sparse:Source {
 uint64_t length=8ULL<<30;std::map<uint64_t,std::array<uint8_t,4096>> pages;size_t largest=0;bool fail=false;unsigned writes=0;
 uint64_t Size()const override{return length;}bool Writable()const override{return true;}void Flush()override{}
 void Read(uint64_t at,void* output,size_t n)override{Check(Disk::Range(at,n,length),"Sparse attribute image read bounds");largest=(std::max)(largest,n);auto p=static_cast<uint8_t*>(output);while(n){auto skip=size_t(at%4096),part=(std::min)(n,4096-skip);auto i=pages.find(at/4096);if(i==pages.end())memset(p,0,part);else memcpy(p,i->second.data()+skip,part);p+=part;at+=part;n-=part;}}
 void Write(uint64_t at,const void* input,size_t n)override{Check(Disk::Range(at,n,length),"Sparse attribute image write bounds");largest=(std::max)(largest,n);++writes;if(fail){fail=false;Write(at,input,n/2);throw Error("Injected large attribute partial write");}auto p=static_cast<const uint8_t*>(input);static const uint8_t zero[4096]{};while(n){auto skip=size_t(at%4096),part=(std::min)(n,4096-skip);auto i=pages.find(at/4096);if(i!=pages.end()||memcmp(p,zero,part)){auto& page=pages[at/4096];memcpy(page.data()+skip,p,part);if(!memcmp(page.data(),zero,4096))pages.erase(at/4096);}p+=part;at+=part;n-=part;}}
};
void Pattern(uint64_t at,void* output,size_t n,uint64_t length){memset(output,0,n);auto p=static_cast<uint8_t*>(output);for(auto mark:{uint64_t(7),(1ULL<<31)+31,length-1})if(mark>=at&&mark-at<n)p[size_t(mark-at)]=uint8_t(mark%251+1);}
void Verify(const AttributeData& data,uint64_t length){Check(data.size()==length&&data.FileBacked(),"Large attribute was not file backed");for(auto at:{uint64_t(0),(1ULL<<31)-16,length-65}){if(at>=length)continue;size_t n=size_t((std::min)(uint64_t(64),length-at));uint8_t got[64],expected[64];data.Read(at,got,n);Pattern(at,expected,n,length);Check(!memcmp(got,expected,n),"Large attribute 64-bit contents");}}
}
void RunLargeAttributeTests(){
 const uint64_t length=(1ULL<<31)+12345;size_t maximum=0;
 auto original=AttributeData::Snapshot(length,[&](uint64_t at,void* p,size_t n){maximum=(std::max)(maximum,n);Pattern(at,p,n,length);});Verify(original,length);Check(maximum<=65536,"Snapshot requested an unbounded input buffer");
 auto patch=Bytes(37,0x83);auto changed=original.Patch((1ULL<<31)+17,patch);Verify(original,length);Bytes check(patch.size());changed.Read((1ULL<<31)+17,check.data(),check.size());Check(check==patch,"Page patch beyond 2 GiB");auto copied=changed;changed={};copied.Read((1ULL<<31)+17,check.data(),check.size());Check(check==patch,"Snapshot lifetime lost page edits");
 bool cancelled=false;try{AttributeData::Snapshot(90ULL<<20,[&](uint64_t at,void* p,size_t n){if(at>=65536)throw Error("Cancel input");memset(p,0,n);});}catch(const Error&){cancelled=true;}Check(cancelled,"Snapshot ignored input failure");
 DWORD before=0,after=0;Check(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"Get staging handle count");{unsigned calls=0;AttributeReadScope cancel([&]{if(++calls==3)throw Error("Checkpoint cancelled");});bool stopped=false;try{AttributeData::Snapshot(1ULL<<30,[](uint64_t,void* p,size_t n){memset(p,0,n);});}catch(const Error&){stopped=true;}Check(stopped&&calls==3,"Streaming checkpoint cancellation ignored");}Check(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after,"Cancelled attribute staging leaked a handle");Verify(original,length);
 Attributes retained;Sparse disk;Volume::Format(disk,u"Large attributes",true);uint32_t id;Attributes values{{u"user.Large",original},{u"user.Other",AttributeData::Snapshot(70ULL<<20,[](uint64_t,void* p,size_t n){memset(p,0,n);})}};
 {Volume v(disk,true);id=v.Put(2,u"File",0,0,[](unsigned,uint64_t,void*,size_t){});v.SetAttributes(id,values);auto got=v.GetAttributes(id);retained=got;Verify(got.at(u"user.Large"),length);Check(got.at(u"user.Other").size()==(70ULL<<20),"Aggregate attribute value lost");v.Rename(id,2,u"Renamed");Check(v.GetAttributes(id)==got,"Rename changed large snapshots");}
 Verify(retained.at(u"user.Large"),length);
 {Volume v(disk,true);auto before=v.GetAttributes(id);Verify(before.at(u"user.Large"),length);auto replacement=before;replacement[u"user.Large"]=copied;disk.fail=true;bool failed=false;try{v.SetAttributes(id,replacement);}catch(const Error&){failed=true;}Check(failed,"Large partial attribute write accepted");}
 {Volume v(disk,true);Verify(v.GetAttributes(id).at(u"user.Large"),length);auto target=v.MakeDirectory(2,u"Folder",&values);Verify(v.GetAttributes(target).at(u"user.Large"),length);v.Remove(target);v.Remove(id);Check(v.GetAttributes(2).empty(),"Large deletion leaked root attributes");}
 Check(disk.largest<=1024*1024&&disk.pages.size()<4096,"Large attributes allocated payload-sized buffers or sparse pages");
 puts("PASS: >2 GiB native attribute read/write/reopen, >128 MiB totals, bounded 64 KiB snapshots, sparse staging, high-offset page edits, snapshot lifetime, cancellation, directory metadata, partial-write recovery and reclamation; synthetic image only");
}
