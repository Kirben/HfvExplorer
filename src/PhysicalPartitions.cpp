// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "PhysicalRemovable.h"
#include "HfsPlusDisk.h"
#include <set>
namespace Removable {
namespace {
using HfsPlus::Bytes;namespace B=HfsPlus::Disk;
uint32_t Le32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint64_t Le64(const uint8_t* p){return Le32(p)|uint64_t(Le32(p+4))<<32;}
uint32_t Crc(Bytes b){uint32_t crc=~0u;for(auto byte:b){crc^=byte;for(unsigned i=0;i<8;++i)crc=(crc>>1)^(0xedb88320u&uint32_t(-int(crc&1)));}return ~crc;}
void Ranges(std::vector<std::pair<uint64_t,uint64_t>>& ranges){std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)B::Check(ranges[i-1].second<ranges[i].first,"Overlapping removable-disk partitions");}
bool MacType(const uint8_t* p){static const uint8_t type[]={0,0x53,0x46,0x48,0,0,0xaa,0x11,0xaa,0x11,0,0x30,0x65,0x43,0xec,0xac};return !memcmp(p,type,16);}
}
std::vector<Partition> Partitions(HfsPlus::Source& source,DWORD sector){
 B::Check(sector>=512&&sector<=4096&&(sector&(sector-1))==0&&source.Size()%sector==0,"Invalid partition-sector geometry");auto boot=B::Read(source,0,512);auto probe=B::Read(source,sector,8);std::vector<Partition> result;bool mbr=boot[510]==0x55&&boot[511]==0xaa,protective=false,appleMbr=false;
 if(mbr)for(unsigned n=0;n<4;++n){protective|=boot[446+n*16+4]==0xee;appleMbr|=boot[446+n*16+4]==0xaf;}
 if(protective||!memcmp(probe.data(),"EFI PART",8)){
  // UEFI GPT: validate both headers and both complete tables before mounting.
  auto sectors=source.Size()/sector;B::Check(sectors>=6,"Truncated GPT removable disk");
  auto header=[&](uint64_t lba){auto h=B::Read(source,lba*sector,sector);B::Check(!memcmp(h.data(),"EFI PART",8)&&Le32(h.data()+8)==0x10000&&Le32(h.data()+12)>=92&&Le32(h.data()+12)<=sector&&Le32(h.data()+20)==0&&Le64(h.data()+24)==lba,"Invalid removable-disk GPT header");h.resize(Le32(h.data()+12));auto crc=Le32(h.data()+16);memset(h.data()+16,0,4);B::Check(Crc(h)==crc,"Removable-disk GPT header checksum mismatch");return h;};
  auto primary=header(1),backup=header(sectors-1);B::Check(Le64(primary.data()+32)==sectors-1&&Le64(backup.data()+32)==1&&!memcmp(primary.data()+40,backup.data()+40,32)&&!memcmp(primary.data()+80,backup.data()+80,12),"Removable-disk GPT copies disagree");auto first=Le64(primary.data()+40),last=Le64(primary.data()+48);auto count=Le32(primary.data()+80),size=Le32(primary.data()+84);B::Check(first>1&&last>=first&&last<sectors-1&&count&&count<=4096&&size>=128&&size<=1024&&(size&(size-1))==0,"Invalid removable-disk GPT geometry");
  auto table=[&](const Bytes& h){auto at=Le64(h.data()+72),bytes=uint64_t(count)*size;B::Check(at>1&&at<sectors-1&&B::Range(at*sector,bytes,source.Size())&&(Le64(h.data()+24)==1?(at+(bytes+sector-1)/sector<=first):(at>last&&at+(bytes+sector-1)/sector<=sectors-1)),"GPT table overlaps usable data");auto out=B::Read(source,at*sector,size_t(bytes));B::Check(Crc(out)==Le32(h.data()+88),"Removable-disk GPT table checksum mismatch");return out;};auto entries=table(primary);B::Check(entries==table(backup),"Removable-disk GPT tables disagree");std::vector<std::pair<uint64_t,uint64_t>> ranges;std::set<Bytes> identifiers;
  for(unsigned i=0;i<count;++i){auto p=entries.data()+size_t(i)*size;if(std::all_of(p,p+16,[](uint8_t x){return x==0;}))continue;auto begin=Le64(p+32),end=Le64(p+40);B::Check(begin>=first&&end>=begin&&end<=last,"GPT partition is outside usable data");Bytes id(p+16,p+32);B::Check(std::any_of(id.begin(),id.end(),[](uint8_t x){return x!=0;})&&identifiers.insert(id).second,"Invalid or duplicate GPT partition UUID");ranges.emplace_back(begin,end);if(MacType(p))result.push_back({begin*sector,(end-begin+1)*sector,i+1});}Ranges(ranges);return result;
 }
 if(B::U16(boot.data())==0x4552){
  auto step=B::U16(boot.data()+2);B::Check(step>=512&&step<=4096&&(step&(step-1))==0&&source.Size()>=uint64_t(step)+512,"Invalid Apple partition block size");auto first=B::Read(source,step,512);B::Check(B::U16(first.data())==0x504d,"Missing Apple partition map");auto count=B::U32(first.data()+4);B::Check(count&&count<=4096&&uint64_t(count)*step<=source.Size()-512,"Invalid Apple partition count");std::vector<std::pair<uint64_t,uint64_t>> ranges;
  for(uint32_t i=1;i<=count;++i){auto entry=B::Read(source,uint64_t(i)*step,512);B::Check(B::U16(entry.data())==0x504d&&B::U32(entry.data()+4)==count,"Inconsistent Apple partition map");uint64_t start=uint64_t(B::U32(entry.data()+8))*step,length=uint64_t(B::U32(entry.data()+12))*step;if(!length)continue;B::Check(B::Range(start,length,source.Size()),"Apple partition lies outside removable disk");ranges.emplace_back(start,start+length-1);std::string type(reinterpret_cast<const char*>(entry.data()+48),32);type.resize(type.find('\0')==type.npos?type.size():type.find('\0'));while(!type.empty()&&type.back()==' ')type.pop_back();if(type=="Apple_HFS"||type=="Apple_HFSX"||type=="Apple_MFS")result.push_back({start,length,i});}Ranges(ranges);return result;
 }
 if(appleMbr){std::vector<std::pair<uint64_t,uint64_t>> ranges;for(unsigned i=0;i<4;++i){auto p=boot.data()+446+i*16;auto type=p[4];uint64_t start=uint64_t(Le32(p+8))*sector,length=uint64_t(Le32(p+12))*sector;if(!type||!length)continue;B::Check(start>=sector&&B::Range(start,length,source.Size()),"MBR partition lies outside removable disk");ranges.emplace_back(start,start+length-1);if(type==0xaf)result.push_back({start,length,i+1});}Ranges(ranges);return result;}
 result.push_back({0,source.Size(),0});return result;
}
}
