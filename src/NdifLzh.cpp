// SPDX-License-Identifier: LGPL-2.1-or-later
// Apple LZH adaptive model and window adapted from Aaru.Compression.Native.
// Copyright (c) 2019-2026 Natalia Portillo.
// See licenses/aaru-compression-LGPL-2.1.txt.
#include "NdifLzh.h"
#include "HfsPlusDisk.h"

namespace Ndif {
namespace {
using namespace HfsPlus;
using Disk::Check;
class Bits {
    const Bytes& bytes_;
    size_t at_=0;
    unsigned bit_=0;
public:
    explicit Bits(const Bytes& bytes):bytes_(bytes){}
    unsigned Take(unsigned n){
        unsigned value=0;
        for(unsigned i=0;i<n;++i){
            Check(at_<bytes_.size(),"Truncated NDIF LZH stream");
            value=(value<<1)|((bytes_[at_]>>(7-bit_))&1);
            if(++bit_==8){bit_=0;++at_;}
        }
        return value;
    }
};

class Model {
    static constexpr int Symbols=314,Count=Symbols*2-1;
    struct Node {
        Node* parent=nullptr;
        Node* left=nullptr;
        Node* right=nullptr;
        unsigned weight=0;
        int index=0,symbol=0;
    };
    std::array<Node,Count> storage_{};
    std::array<Node*,Count> order_{};

    static void Attach(Node* branch,Node* left,Node* right){
        branch->left=left;
        branch->right=right;
        left->parent=right->parent=branch;
        branch->weight=left->weight+right->weight;
    }
    void Set(int index,Node* node){
        order_[index]=node;
        node->index=index;
    }
    void Rescale(){
        std::array<Node*,Symbols> leaves{};
        size_t count=0;
        for(auto* node:order_){
            if(node->left)continue;
            node->weight=(node->weight+1)/2;
            leaves[count++]=node;
        }
        int leaf=Symbols-1,branch=Symbols-2,position=Count-1,pair=Count-2;
        while(position>=0){
            while(position>=pair)Set(position--,leaves[leaf--]);
            auto* merged=&storage_[branch--];
            Attach(merged,order_[pair],order_[pair+1]);
            while(leaf>=0&&leaves[leaf]->weight<=merged->weight)
                Set(position--,leaves[leaf--]);
            Set(position--,merged);
            pair-=2;
        }
        order_.front()->parent=nullptr;
    }
    void Update(Node* leaf){
        if(order_.front()->weight==0x8000)Rescale();
        for(auto* node=leaf;node;node=node->parent){
            ++node->weight;
            const auto old=node->index;
            auto position=old;
            while(position>0&&order_[position-1]->weight<node->weight)--position;
            if(position==old)continue;
            auto* peer=order_[position];
            auto parent=node->parent;
            auto other=peer->parent;
            auto& parentSlot=parent->left==node?parent->left:parent->right;
            auto& peerSlot=other->left==peer?other->left:other->right;
            std::swap(parentSlot,peerSlot);
            std::swap(node->parent,peer->parent);
            Set(position,node);
            Set(old,peer);
        }
    }
public:
    Model(){
        for(int i=0;i<Count;++i)Set(i,&storage_[i]);
        for(int i=0;i<Symbols;++i){
            auto& leaf=storage_[Count-i-1];
            leaf.weight=1;
            leaf.symbol=i;
        }
        for(int i=Symbols-2;i>=0;--i)Attach(&storage_[i],order_[2*i+1],order_[2*i+2]);
    }
    unsigned Read(Bits& bits){
        auto* node=order_.front();
        while(node->left)node=bits.Take(1)?node->left:node->right;
        const auto symbol=unsigned(node->symbol);
        Update(node);
        return symbol;
    }
};

size_t Distance(Bits& bits)
{
    constexpr unsigned lengths[]={1,3,8,12,24,16};
    unsigned code=bits.Take(3);
    unsigned first=0,slot=0;
    for(const auto count:lengths){
        if(code>=first&&code-first<count)return (slot+code-first)*64+bits.Take(6)+1;
        first=(first+count)*2;
        slot+=count;
        if(slot<64)code=(code<<1)|bits.Take(1);
    }
    throw Error("Invalid NDIF LZH distance code");
}
}

void DecodeLzh(const HfsPlus::Bytes& input,HfsPlus::Bytes& output)
{
    Bits bits(input);
    Model model;
    std::array<uint8_t,4096> history{};
    for(unsigned i=0;i<256;++i){
        std::fill_n(history.begin()+18+i*13,13,uint8_t(i));
        history[3346+i]=uint8_t(i);
        history[3602+i]=uint8_t(255-i);
    }
    size_t at=0;
    const auto emit=[&](uint8_t byte){
        history[at&(history.size()-1)]=byte;
        output[at++]=byte;
    };
    while(at<output.size()){
        const auto symbol=model.Read(bits);
        if(symbol<256)emit(uint8_t(symbol));
        else{
            const auto length=symbol-253;
            const auto distance=Distance(bits);
            Check(length<=output.size()-at&&distance<=history.size(),"NDIF LZH copy exceeds its chunk");
            for(unsigned i=0;i<length;++i)
                emit(history[(at+history.size()-distance)&(history.size()-1)]);
        }
    }
}
}
