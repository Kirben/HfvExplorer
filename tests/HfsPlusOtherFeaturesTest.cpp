// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include "HfsPlusDisk.h"
#include "HfsPlusTime.h"
#include <cstdio>
using namespace HfsPlus;
using namespace HfsPlus::Disk;
namespace {
template<class F>void Reject(F f,const char* why){try{f();}catch(const std::exception&){return;}throw Error(why);}
struct Memory:Source{
 Bytes bytes;bool writable=true;unsigned writes=0,flushes=0;int failWrite=-1,failFlush=-1;
 explicit Memory(size_t n=32*1024*1024):bytes(n,0){}
 uint64_t Size()const override{return bytes.size();}bool Writable()const override{return writable;}
 void Read(uint64_t at,void* p,size_t n)override{Check(Range(at,n,bytes.size()),"Memory read range");memcpy(p,bytes.data()+at,n);}
 void Write(uint64_t at,const void* p,size_t n)override{Check(writable&&Range(at,n,bytes.size()),"Memory write range");if(failWrite>=0&&writes==unsigned(failWrite))throw Error("Injected write error");memcpy(bytes.data()+at,p,n);++writes;}
 void Flush()override{if(failFlush>=0&&flushes==unsigned(failFlush))throw Error("Injected flush error");++flushes;}
};
const std::string uuid="64100C5D-79CA-4C51-9034-0A3265BDA214";
struct Dual:Memory{
 std::shared_ptr<Memory> external=std::make_shared<Memory>(32*1024*1024);bool available=true;
 Dual()=default;Dual(const Dual& d):Memory(d),external(std::make_shared<Memory>(*d.external)),available(d.available){}
 std::shared_ptr<Source> OpenJournal(const Bytes& info)override{Check(available,"External journal is missing");Check(info.size()>=89&&!memcmp(info.data()+52,uuid.c_str(),37),"External journal identity mismatch");return external;}
 void ReadOnly(){writable=false;external->writable=false;}
};
void Pattern(unsigned f,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<uint8_t*>(p)[i]=uint8_t((at+i)*31+f*97);}
void Verify(Volume& v,uint32_t id){for(unsigned f=0;f<2;++f){Bytes expected(f?723:5003),got(expected.size());Pattern(f,0,expected.data(),expected.size());Check(v.Read(id,f,0,got.data(),got.size())==got.size()&&got==expected,"Other features changed file forks");}}
void MutationBoundary(Volume::FormatKind kind){
 Memory baseline;
 Volume::Format(baseline,u"Transaction fixture",true,kind);
 const Attributes original{{u"user.fixture",{1,2,3}}},replacement{{u"user.fixture",{7,8,9}}};
 uint32_t id=0;
 {
  Volume v(baseline,true);
  id=v.Put(2,u"Original",5003,723,Pattern,nullptr,false,nullptr,&original);
  v.MakeDirectory(2,u"Collision");
  const auto before=baseline.writes;
  Reject([&]{v.Rename(id,2,u"Collision");},"Rename accepted a collision");
  Reject([&]{v.SetAttributes(id,{{u"",{4}}});},"Attribute edit accepted an invalid key");
  Check(v.Writable()&&baseline.writes==before&&v.Stat(id).name==u"Original"&&
        v.GetAttributes(id)==original,"Preflight rejection changed or fenced a healthy mount");
 }
 for(bool attributes:{false,true})for(bool afterCommit:{false,true}){
  Memory failed=baseline;
  {
   Volume v(failed,true);
   const auto phase=afterCommit?"journal-committed":"data-flushed";
   bool injected=false;
   v.fault=[&](const char* at){if(!strcmp(at,phase)){injected=true;throw Error("Injected mutation failure");}};
   Reject([&]{if(attributes)v.SetAttributes(id,replacement);else v.Rename(id,2,u"Renamed");},
          "Mutation ignored an injected failure");
   Check(injected&&!v.Writable(),"Failed mutation left the old mount writable");
   Reject([&]{v.Stat(id);},"Failed mutation exposed the staged catalog through Stat");
  }
  const auto writes=failed.writes;
  Volume recovered(failed,false);
  Check(recovered.Stat(id).name==(afterCommit&&!attributes?u"Renamed":u"Original")&&
        recovered.GetAttributes(id)==(afterCommit&&attributes?replacement:original),
        "Read-only replay recovered the wrong side of a mutation");
  Verify(recovered,id);
  Check(failed.writes==writes,"Read-only mutation recovery wrote the image");
 }
}
void Times(Volume::FormatKind kind,bool journaled){
 Memory source;Volume::Format(source,u"Expanded",journaled,kind,true);uint32_t id;
 std::array<uint64_t,2> dates{Time::Epoch,Time::Maximum};
 {Volume v(source,true);Check(v.ExpandedTimes(),"Expanded format flag missing");id=v.Put(2,u"Future",5003,723,Pattern,nullptr,false,nullptr,nullptr,Volume::CompressionMode::Preserve,&dates);Check(v.Stat(id).created==dates[0]&&v.Stat(id).modified==dates[1],"Expanded dates truncated");v.SetCompression(id,true);Check(v.Stat(id).created==dates[0]&&v.Stat(id).modified==dates[1],"Compression lost future dates");v.SetCompression(id,false);Verify(v,id);v.Rename(id,2,u"Renamed");v.SetAttributes(id,{{u"user.keep",{1,2,3}}});}
 {Volume v(source);Check(v.Stat(id).created==Time::Epoch,"Expanded creation epoch changed");Verify(v,id);}
 auto writes=source.writes;{Volume v(source,true);auto bad=dates;bad[0]=Time::Epoch-1;Reject([&]{v.Put(2,u"Bad",0,0,Pattern,nullptr,false,nullptr,nullptr,Volume::CompressionMode::Preserve,&bad);},"Pre-1970 expanded date accepted");bad[0]=Time::Maximum+1;Reject([&]{v.Put(2,u"Bad",0,0,Pattern,nullptr,false,nullptr,nullptr,Volume::CompressionMode::Preserve,&bad);},"Expanded date wrapped after 2106");}Check(source.writes==writes,"Invalid dates wrote image");
 Memory classic;Volume::Format(classic,u"Legacy dates",journaled,kind);{Volume v(classic,true);Reject([&]{v.Put(2,u"Future",0,0,Pattern,nullptr,false,nullptr,nullptr,Volume::CompressionMode::Preserve,&dates);},"Classic date field silently wrapped");}
 // Mixed epochs are selected by each catalog record, independently of the VH.
 uint32_t mixedId=0;Memory mixed;Volume::Format(mixed,u"Mixed",false,kind);{Volume v(mixed,true);mixedId=v.Put(2,u"Legacy record",5003,723,Pattern);}
 for(uint64_t at:{1024ULL,mixed.Size()-1024}){auto h=mixed.bytes.data()+at;P32(h+4,U32(h+4)|0x20000000);for(unsigned p=16;p<=28;p+=4){auto raw=U32(h+p);P32(h+p,raw?Time::Encode(raw,true):0);}}
 {Volume v(mixed,true);auto old=v.Stat(mixedId).created;Check(old>Time::Epoch,"Mixed legacy record decoded with Unix epoch");v.Put(2,u"Legacy record",5003,723,Pattern,nullptr,true,nullptr,nullptr,Volume::CompressionMode::Preserve,&dates);Check(v.Stat(mixedId).created==dates[0]&&v.Stat(mixedId).modified==dates[1],"Mixed record conversion failed");}
 if(journaled){Memory baseline=source;for(const char* phase:{"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}){Memory failed=baseline;std::array<uint64_t,2> later{Time::Maximum-20,Time::Maximum-10};{Volume v(failed,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("Date crash");};Reject([&]{v.Put(2,u"Renamed",5003,723,Pattern,nullptr,true,nullptr,nullptr,Volume::CompressionMode::Preserve,&later);},"Missing date crash phase");}failed.writable=false;Volume v(failed);Verify(v,id);auto e=v.Stat(id);Check((e.created==Time::Epoch)||(e.created==later[0]&&e.modified==later[1]),"Date recovery mixed timestamp epochs");}}
}
void External(Volume::FormatKind kind){
 Dual baseline;Volume::Format(baseline,u"External",true,kind,true);uint32_t id;{Volume v(baseline,true);id=v.Put(2,u"File",5003,723,Pattern);}
 for(const char* phase:{"journal-resize-data-flushed","journal-resize-new-log-committed","journal-resize-jib-written","journal-resize-jib-committed","journal-resize-home-flushed","journal-resize-cleared"}){Dual failed=baseline;{Volume v(failed,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("External switch crash");};Reject([&]{v.UseExternalJournal(uuid);},"Missing external switch fault boundary");}failed.ReadOnly();auto a=failed.writes,b=failed.external->writes;Volume v(failed);Verify(v,id);Check(a==failed.writes&&b==failed.external->writes,"Read-only external switch replay wrote sources");}
 {Volume v(baseline,true);v.UseExternalJournal(uuid);Check(v.ExternalJournal(),"External journal not selected");Verify(v,id);}
 for(const char* phase:{"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}){Dual failed=baseline;{Volume v(failed,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("External transaction crash");};Reject([&]{v.Rename(id,2,u"Renamed");},"Missing external transaction boundary");}failed.ReadOnly();auto a=failed.writes,b=failed.external->writes;Volume v(failed);Verify(v,id);Check(v.ExternalJournal()&&(v.Stat(id).name==u"File"||v.Stat(id).name==u"Renamed"),"External transaction lost namespace");Check(a==failed.writes&&b==failed.external->writes,"Read-only external replay wrote sources");}
 for(bool home:{false,true})for(bool flush:{false,true}){
  Dual success=baseline;auto& target=home?static_cast<Memory&>(success):*success.external;unsigned before=flush?target.flushes:target.writes;{Volume v(success,true);v.Rename(id,2,u"Renamed");}unsigned count=(flush?target.flushes:target.writes)-before;
  for(unsigned step=0;step<count;++step){Dual failed=baseline;auto& out=home?static_cast<Memory&>(failed):*failed.external;if(flush)out.failFlush=int(out.flushes+step);else out.failWrite=int(out.writes+step);{Volume v(failed,true);Reject([&]{v.Rename(id,2,u"Renamed");},"External I/O fault ignored");}failed.failFlush=failed.failWrite=failed.external->failFlush=failed.external->failWrite=-1;failed.ReadOnly();Volume v(failed);Verify(v,id);Check(v.Stat(id).name==u"File"||v.Stat(id).name==u"Renamed","External I/O fault lost entry");}
 }
 {Dual locked=baseline;locked.external->writable=false;auto writes=locked.writes;Reject([&]{Volume v(locked,true);},"Read-only external device accepted for writes");Check(locked.writes==writes,"Read-only external rejection wrote filesystem");}
 {Dual missing=baseline;missing.available=false;Reject([&]{Volume v(missing);},"Missing external journal accepted");}
 {Dual wrong=baseline;auto offset=uint64_t(U32(wrong.bytes.data()+1024+12))*4096;wrong.bytes[size_t(offset)+52]='F';Reject([&]{Volume v(wrong);},"Wrong external identity accepted");}
 // A large metadata transaction must grow into a disjoint external region.
 HfsPlus::Attributes attrs;for(unsigned i=0;i<4500;++i)attrs[u"user."+std::u16string(90,u'x')+std::u16string(1,char16_t(0x400+i))]=Bytes{1};
 if(kind==Volume::FormatKind::HfsPlus)for(const char* phase:{"journal-resize-data-flushed","journal-resize-new-log-committed","journal-resize-jib-written","journal-resize-jib-committed","journal-resize-home-flushed","journal-resize-cleared"}){Dual failed=baseline;{Volume v(failed,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("External enlargement crash");};Reject([&]{v.SetAttributes(id,attrs);},"Missing external enlargement boundary");}failed.ReadOnly();auto a=failed.writes,b=failed.external->writes;Volume v(failed);auto recovered=v.GetAttributes(id);Check(recovered.empty()||recovered==attrs,"External enlargement recovered mixed attributes");Verify(v,id);Check(a==failed.writes&&b==failed.external->writes,"Read-only external enlargement replay wrote sources");}
 {Volume v(baseline,true);v.SetAttributes(id,attrs);Check(v.GetAttributes(id)==attrs,"External enlargement lost attributes");}
 {Volume v(baseline);Check(v.GetAttributes(id)==attrs&&v.ExternalJournal(),"External enlargement failed to reopen");Verify(v,id);}
 auto jib=uint64_t(U32(baseline.bytes.data()+1024+12))*4096;Check(U64(baseline.bytes.data()+jib+36)>0,"External journal did not enlarge to a separate region");
}
}
int RunHfsPlusOtherFeaturesTests(const wchar_t* directory){
 if(!CreateDirectoryW(directory,nullptr))return 2;std::wstring root=directory;FILE* report=_wfopen((root+L"\\other-hfsplus-features-report.txt").c_str(),L"w");if(!report)return 2;
 try{
  Check(Time::Decode(0,false)==0&&Time::Decode(0,true)==Time::Epoch&&Time::Decode(UINT32_MAX,true)==Time::Maximum,"Timestamp boundary decoding");
  for(auto kind:{Volume::FormatKind::HfsPlus,Volume::FormatKind::HfsXCaseSensitive,Volume::FormatKind::HfsXCaseInsensitive}){MutationBoundary(kind);for(bool journaled:{false,true})Times(kind,journaled);External(kind);}
  fprintf(report,"PASS: rename and attribute validation, write fencing, and recovery before and after the journal commit.\n");
  fprintf(report,"PASS: expanded timestamps, 1970/2040/2106 bounds, mixed catalog epochs, no wrapping, metadata/compression preservation and journal crash recovery in HFS+ and both HFSX modes.\n");
  fprintf(report,"PASS: external journal selection, internal-to-external switch/replay, every commit boundary, read-only replay, all home/journal write/flush failure positions, missing/wrong device refusal and disjoint journal enlargement. Injected devices only.\n");
  auto image=root+L"\\external.dsk",journal=root+L"\\journal.img",wrong=root+L"\\wrong.img";auto identity=CreateExternalJournalImage(journal.c_str(),16*1024*1024);CreateExternalJournalImage(wrong.c_str(),16*1024*1024);
  {auto source=CreateImage(image.c_str(),8*1024*1024,journal.c_str());Volume::Format(*source,u"External example",true,Volume::FormatKind::HfsXCaseSensitive,true);Volume v(*source,true);v.UseExternalJournal(identity);std::array<uint64_t,2> sample{Time::Epoch,Time::Maximum};v.Put(2,u"File",5003,723,Pattern,nullptr,false,nullptr,nullptr,Volume::CompressionMode::Preserve,&sample);}
  Reject([&]{auto source=OpenImage(image.c_str(),false);Volume v(*source);},"Missing native journal accepted");Reject([&]{auto source=OpenImage(image.c_str(),true,wrong.c_str());Volume v(*source,true);},"Mismatched GPT journal accepted");
  {auto source=OpenImage(image.c_str(),true,journal.c_str());Volume v(*source,true);Check(v.ExternalJournal()&&v.ExpandedTimes(),"Native GPT journal reopen failed");auto list=v.List();auto f=std::find_if(list.begin(),list.end(),[](const Entry& e){return e.name==u"File";});Check(f!=list.end(),"Native external file absent");Verify(v,f->id);Reject([&]{auto conflict=OpenImage(image.c_str(),true,journal.c_str());},"Conflicting filesystem writer accepted");auto other=root+L"\\conflict.dsk";auto conflict=CreateImage(other.c_str(),8*1024*1024,journal.c_str());Volume::Format(*conflict,u"Conflict",true);Volume c(*conflict,true);Reject([&]{c.UseExternalJournal(identity);},"Conflicting external journal writer accepted");}
  auto corrupt=root+L"\\corrupt-journal.img";Check(CopyFileW(journal.c_str(),corrupt.c_str(),TRUE)!=FALSE,"Create corrupt GPT fixture");
  // A single-bit corrupt GPT header must fail without writing either image.
  {auto raw=OpenImage(corrupt.c_str(),true);uint8_t b;raw->Read(512+16,&b,1);b^=1;raw->Write(512+16,&b,1);raw->Flush();}
  Reject([&]{auto source=OpenImage(image.c_str(),false,corrupt.c_str());Volume v(*source);},"Bad GPT CRC accepted");
  fprintf(report,"PASS: native image/GPT journal creation and binding by partition type/UUID, missing/mismatched/corrupt journal rejection, reopen/write and conflicting-instance exclusion. No physical devices or computer control.\n");
 }catch(const std::exception& e){fprintf(report,"FAIL: %s\n",e.what());fclose(report);return 1;}
 fclose(report);return 0;
}
