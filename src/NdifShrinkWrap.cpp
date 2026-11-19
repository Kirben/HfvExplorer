// SPDX-License-Identifier: LGPL-2.1-or-later
// SIT2 format and code ordering adapted from Aaru.Compression.Native.
// Copyright (c) 2019-2026 Natalia Portillo.
// See licenses/aaru-compression-LGPL-2.1.txt.
#include "NdifShrinkWrap.h"
#include "HfsPlusDisk.h"
#include <optional>

namespace Ndif {
namespace {
using namespace HfsPlus;
using Disk::Check;
class Bits {
    const Bytes& bytes_;
    size_t offset_=16;
    unsigned bit_=0;
public:
    explicit Bits(const Bytes& bytes):bytes_(bytes){}
    unsigned Take(unsigned count){
        unsigned out=0;
        for(unsigned i=0;i<count;++i){
            Check(offset_<bytes_.size(),"Truncated NDIF ShrinkWrap stream");
            out|=((bytes_[offset_]>>bit_)&1u)<<i;
            if(++bit_==8){bit_=0;++offset_;}
        }
        return out;
    }
    void Align(){if(bit_){bit_=0;++offset_;}}
};
struct Code {unsigned bits,symbol;};

// The classic encoder assigns equal-length codes in this partition order.
void SortCodes(std::vector<Code>& codes,size_t first,size_t last)
{
    while(last-first>1){
        size_t lower=first,upper=last;
        do{
            while(++lower<last&&codes[lower].bits<codes[first].bits){}
            while(--upper>first&&codes[upper].bits>codes[first].bits){}
            if(upper>lower)std::swap(codes[lower],codes[upper]);
        }while(upper>lower);
        if(first==upper){++first;continue;}
        std::swap(codes[first],codes[upper]);
        lower=upper+1;
        if(last-lower<=upper-first){
            SortCodes(codes,lower,last);
            last=upper;
        }else{
            SortCodes(codes,first,upper);
            first=lower;
        }
    }
}

class Tree {
    std::vector<std::array<int,2>> nodes_{1};
public:
    explicit Tree(std::vector<Code> codes){
        SortCodes(codes,0,codes.size());
        uint64_t code=0;
        unsigned previous=0;
        for(const auto item:codes){
            if(!item.bits)continue;
            Check(item.bits<=32,"Invalid NDIF ShrinkWrap Huffman length");
            code<<=item.bits-previous;
            Check(code<(uint64_t(1)<<item.bits),"Oversubscribed NDIF ShrinkWrap Huffman tree");
            size_t node=0;
            for(unsigned left=item.bits;left;--left){
                const auto bit=unsigned((code>>(left-1))&1);
                if(left==1){
                    Check(!nodes_[node][bit],"Overlapping NDIF ShrinkWrap Huffman code");
                    nodes_[node][bit]=-int(item.symbol+1);
                }else{
                    Check(nodes_[node][bit]>=0,"Invalid NDIF ShrinkWrap Huffman prefix");
                    if(!nodes_[node][bit]){
                        nodes_[node][bit]=int(nodes_.size());
                        nodes_.push_back({});
                    }
                    node=size_t(nodes_[node][bit]);
                }
            }
            ++code;
            previous=item.bits;
        }
    }
    unsigned Symbol(Bits& bits)const{
        int node=0;
        do{
            node=nodes_[size_t(node)][bits.Take(1)];
            Check(node!=0,"Missing NDIF ShrinkWrap Huffman symbol");
        }while(node>0);
        return unsigned(-node-1);
    }
};

Tree ReadTree(Bits& bits,unsigned size,unsigned depth=0)
{
    Check(depth<32,"NDIF ShrinkWrap Huffman nesting exceeds the supported depth");
    const bool sparse=bits.Take(1)!=0;
    const auto width=bits.Take(2)+2,base=bits.Take(3)+1;
    const unsigned repeat=(1u<<width)-1;
    const bool indirect=(bits.Take(2)&1)!=0;
    std::optional<Tree> codeTree;
    if(indirect)codeTree.emplace(ReadTree(bits,repeat+1,depth+1));
    const auto next=[&](){return codeTree?codeTree->Symbol(bits):bits.Take(width);};
    std::vector<Code> codes;
    codes.reserve(size);
    while(codes.size()<size){
        const auto symbol=next();
        if(symbol==repeat){
            Check(!codes.empty(),"NDIF ShrinkWrap repeats an absent code length");
            const auto copy=(std::min)(size-codes.size(),size_t(next()+3));
            const auto length=codes.back().bits;
            for(size_t i=0;i<copy;++i)codes.push_back({length,unsigned(codes.size())});
        }else{
            const auto length=sparse&&symbol==repeat-1?0:base+symbol;
            codes.push_back({length,unsigned(codes.size())});
        }
    }
    bits.Align();
    return Tree(std::move(codes));
}

uint32_t Little(const uint8_t* p)
{
    return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
}

void DecodeShrinkWrap(const HfsPlus::Bytes& input,HfsPlus::Bytes& output)
{
    Check(input.size()>=16&&Little(input.data())==input.size()&&
          Little(input.data()+4)==output.size()&&Little(input.data()+8)==output.size()&&
          Little(input.data()+12)==UINT32_MAX,"Invalid NDIF ShrinkWrap chunk header");
    Bits bits(input);
    const auto literals=ReadTree(bits,276),distances=ReadTree(bits,16);
    static constexpr auto bases=[]{
        std::array<unsigned,20> values{};
        unsigned base=4;
        for(unsigned i=0;i<values.size();++i){
            values[i]=base;
            base+=1u<<(i<2?0:(i-2)/2);
        }
        return values;
    }();
    for(size_t at=0;at<output.size();){
        const auto symbol=literals.Symbol(bits);
        if(symbol<256){
            output[at++]=uint8_t(symbol);
            continue;
        }
        const auto method=symbol-256;
        Check(method<std::size(bases),"Unsupported NDIF ShrinkWrap copy length");
        const auto length=bases[method]+bits.Take(method<2?0:(method-2)/2);
        const auto slot=distances.Symbol(bits);
        const size_t distance=(size_t(1)<<slot)+bits.Take(slot);
        Check(distance<=at&&length<=output.size()-at,"NDIF ShrinkWrap copy exceeds its history or chunk");
        for(unsigned i=0;i<length;++i,++at)output[at]=output[at-distance];
    }
}
}
