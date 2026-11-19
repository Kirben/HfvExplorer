// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "PhysicalRemovable.h"
#include "DiskPartitions.h"
#include "HfsPlusDisk.h"

namespace {
using namespace HfsPlus;
namespace B=HfsPlus::Disk;
uint32_t Le32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
void CheckRanges(std::vector<std::pair<uint64_t,uint64_t>>& ranges)
{
    std::sort(ranges.begin(),ranges.end());
    for(size_t i=1;i<ranges.size();++i)
        B::Check(ranges[i-1].second<ranges[i].first,"Overlapping disk partitions");
}
}

namespace Removable {
std::vector<Partition> Partitions(Source& source,DWORD sector){
 B::Check(sector>=512&&sector<=4096&&(sector&(sector-1))==0&&source.Size()%sector==0,"Invalid partition-sector geometry");
 auto boot=B::Read(source,0,512);
 std::vector<Partition> result;
 if(DiskPartitions::HasProtectiveMbr(boot)||DiskPartitions::HasGptHeader(source,sector)){
  for(const auto& partition:DiskPartitions::ReadGptMacPartitions(source,sector,DiskPartitions::GptType::AppleFilesystems))
   result.push_back({partition.offset,partition.length,partition.index,partition.readOnly});
  return result;
 }
 bool appleMbr=false,otherMbr=false;
 if(boot[510]==0x55&&boot[511]==0xaa){
  for(unsigned n=0;n<4;++n){
   const auto entry=boot.data()+446+n*16;
   appleMbr|=entry[4]==0xaf;
   const auto start=uint64_t(Le32(entry+8))*sector,length=uint64_t(Le32(entry+12))*sector;
   otherMbr|=(entry[0]==0||entry[0]==0x80)&&entry[4]&&start>=sector&&length&&B::Range(start,length,source.Size());
  }
 }
 if(auto partitions=DiskPartitions::ProbeApm(source,boot)){
  for(const auto& partition:*partitions)
   result.push_back({partition.offset,partition.length,partition.index,partition.readOnly});
  return result;
 }
 if(appleMbr){std::vector<std::pair<uint64_t,uint64_t>> ranges;for(unsigned i=0;i<4;++i){auto p=boot.data()+446+i*16;auto type=p[4];uint64_t start=uint64_t(Le32(p+8))*sector,length=uint64_t(Le32(p+12))*sector;if(!type||!length)continue;B::Check(start>=sector&&B::Range(start,length,source.Size()),"MBR partition lies outside removable disk");ranges.emplace_back(start,start+length-1);if(type==0xaf)result.push_back({start,length,i+1});}CheckRanges(ranges);return result;}
 result.push_back({0,source.Size(),0,otherMbr});return result;
}
}
