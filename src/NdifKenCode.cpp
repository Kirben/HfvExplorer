// SPDX-License-Identifier: BSD-3-Clause
// KenCode coding rules adapted from ndif2raw.
// Copyright (c) 2024-2025 Individual contributors. All rights reserved.
// See licenses/ndif2raw.txt for redistribution terms.
#include "NdifKenCode.h"
#include "HfsPlusDisk.h"

namespace Ndif {
namespace {
using HfsPlus::Disk::Check;
class Bits {
    const HfsPlus::Bytes& input_;
    size_t index_=0;
    unsigned used_=0;
public:
    explicit Bits(const HfsPlus::Bytes& input):input_(input){}
    uint32_t Take(unsigned count){
        uint32_t value=0;
        while(count--){
            Check(index_<input_.size(),"Truncated NDIF KenCode stream");
            value=(value<<1)|((input_[index_]>>(7-used_))&1);
            if(++used_==8){++index_;used_=0;}
        }
        return value;
    }
};

size_t CopyLength(Bits& bits)
{
    unsigned ones=0;
    while(ones<10&&bits.Take(1))++ones;
    if(ones==0)return bits.Take(1);
    if(ones==1)return bits.Take(1)?3+bits.Take(1):2;
    if(ones==2)return bits.Take(1)?7+bits.Take(2):5+bits.Take(1);
    constexpr unsigned base[]={11,19,27,59,123,251,507,1019};
    constexpr unsigned width[]={3,3,5,6,7,8,9,10};
    return base[ones-3]+bits.Take(width[ones-3]);
}
size_t LiteralLength(Bits& bits)
{
    if(!bits.Take(1))return 1;
    const auto code=bits.Take(2);
    if(code<2)return code+2;
    if(code==2)return 4+bits.Take(2);
    const auto extended=bits.Take(4);
    if(extended<8)return 8+extended;
    if(extended<12)return extended*4-16+bits.Take(2);
    return extended*8-64+bits.Take(3);
}
size_t Distance(Bits& bits,size_t produced)
{
    // NDIF uses a KenCode dictionary with 10,240 nodes.
    constexpr size_t thresholds[]={10,20,40,80,160,672,1000,2688,5376,10752};
    const auto width=unsigned(std::lower_bound(std::begin(thresholds),std::end(thresholds),produced)-std::begin(thresholds));
    const size_t window=size_t(1)<<width;
    if(!bits.Take(1))return 1+bits.Take(width);
    if(!bits.Take(1))return window+1+bits.Take(width+2);
    const auto base=5*window+1;
    auto threshold=base;
    for(unsigned n=1;n<=width+4;++n){
        threshold+=size_t(1)<<(n-1);
        const auto boundary=threshold==1664?1644:threshold;
        if(boundary>=produced||n==width+4)return base+bits.Take(n);
    }
    throw HfsPlus::Error("Invalid NDIF KenCode distance");
}
}

void DecodeKenCode(const HfsPlus::Bytes& input,HfsPlus::Bytes& output)
{
    Bits bits(input);
    size_t out=0;
    bool literals=true;
    while(out<output.size()){
        size_t length=CopyLength(bits);
        if(!length&&literals){
            length=LiteralLength(bits);
            Check(length<=output.size()-out,"NDIF KenCode literal exceeds its chunk");
            for(size_t i=0;i<length;++i)output[out++]=uint8_t(bits.Take(8));
            literals=length>62;
        }else{
            length+=literals?2:3;
            Check(length<=output.size()-out,"NDIF KenCode copy exceeds its chunk");
            const auto distance=Distance(bits,out);
            Check(distance&&distance<=out,"NDIF KenCode copy precedes its chunk");
            for(size_t i=0;i<length;++i,++out)output[out]=output[out-distance];
            literals=true;
        }
    }
}
}
