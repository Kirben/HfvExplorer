// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace MainMfsFixture {
static void w16(std::vector<BYTE>& b,size_t p,unsigned v){b.at(p)=BYTE(v>>8);b.at(p+1)=BYTE(v);}
static void w32(std::vector<BYTE>& b,size_t p,uint32_t v){w16(b,p,v>>16);w16(b,p+2,v);}
static BYTE pattern(size_t offset,unsigned seed){return BYTE(offset*71+seed);}
struct Fixture {
    std::vector<BYTE> bytes;unsigned count=391,allocation=1024,directory=4,directoryBlocks=12,first=16;
    std::string longName=std::string(254,'L')+char(-114);
    size_t firstEntry() const {return size_t(directory)*512;}
    // Deliberately pack individual bits rather than using the reader's decoder.
    void link(unsigned block,unsigned next) {
        for(unsigned bit=0;bit<12;++bit) {
            unsigned position=(block-2)*12+bit;BYTE mask=BYTE(1u<<(7-position%8));
            BYTE& byte=bytes.at(1088+position/8);
            byte=BYTE((byte&~mask)|((next&(1u<<(11-bit)))?mask:0));
        }
    }
    void fill(const std::vector<unsigned>& blocks,unsigned length,unsigned seed) {
        for(unsigned i=0;i<blocks.size();++i) {
            link(blocks[i],i+1<blocks.size()?blocks[i+1]:1);
            for(unsigned j=0;j<allocation;++j) {
                size_t logical=size_t(i)*allocation+j;
                bytes.at(size_t(first)*512+size_t(blocks[i]-2)*allocation+j)=logical<length?pattern(logical,seed):0xee;
            }
        }
    }
    unsigned record(unsigned offset,const std::string& name,unsigned id,BYTE version,
                    unsigned dataBlock,unsigned dataLength,unsigned dataPhysical,
                    unsigned resourceBlock,unsigned resourceLength,unsigned resourcePhysical) {
        unsigned p=directory*512+offset;bytes[p]=0x81;bytes[p+1]=version;
        memcpy(bytes.data()+p+2,"TEXTttxt",8);w16(bytes,p+10,0x4001);
        w16(bytes,p+12,33);w16(bytes,p+14,44);w16(bytes,p+16,3);
        w32(bytes,p+18,id);w16(bytes,p+22,dataBlock);w32(bytes,p+24,dataLength);w32(bytes,p+28,dataPhysical);
        w16(bytes,p+32,resourceBlock);w32(bytes,p+34,resourceLength);w32(bytes,p+38,resourcePhysical);
        w32(bytes,p+42,2700000000u);w32(bytes,p+46,2800000000u);bytes[p+50]=BYTE(name.size());
        memcpy(bytes.data()+p+51,name.data(),name.size());return offset+unsigned((51+name.size()+1)&~size_t(1));
    }
    explicit Fixture(bool larger=false) {
        if(larger) {count=790;directory=5;first=17;}
        bytes.resize(larger?800*1024:400*1024);
        w16(bytes,1024,0xd2d7);w32(bytes,1026,2600000000u);w32(bytes,1030,2900000000u);
        w16(bytes,1036,4);w16(bytes,1038,directory);w16(bytes,1040,directoryBlocks);w16(bytes,1042,count);
        w32(bytes,1044,allocation);w32(bytes,1048,allocation);w16(bytes,1052,first);w32(bytes,1054,1000);w16(bytes,1058,count-6);
        bytes[1060]=7;memcpy(bytes.data()+1061,"MFSTest",7);
        unsigned next=record(0,"Read Me",42,0,2,2536,3072,9,1177,2048);
        record(next,"Empty",43,0,0,0,0,0,0,0);
        next=record(512,longName,44,0,12,17,1024,0,0,0);
        record(next,"Read Me",45,1,0,0,0,0,0,0); // Version distinguishes identical names.
        // This zero flag terminates a directory sector despite leftover bytes.
        bytes[directory*512+1024+1]=0xff;
        fill({2,7,4},2536,17);fill({9,3},1177,118);fill({12},17,77);
    }
};

}
