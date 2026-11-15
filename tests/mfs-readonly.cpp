// SPDX-License-Identifier: GPL-2.0-or-later
#include "MfsImage.h"
#include <stdio.h>
#include <stdexcept>
#include <functional>
#include <algorithm>

static void check(bool ok,const char* action) {if(!ok) throw std::runtime_error(action);}
static void w16(std::vector<BYTE>& b,size_t p,unsigned v) {b.at(p)=BYTE(v>>8);b.at(p+1)=BYTE(v);}
static void w32(std::vector<BYTE>& b,size_t p,uint32_t v) {w16(b,p,v>>16);w16(b,p+2,v);}
static unsigned r16(const std::vector<BYTE>& b,size_t p) {return unsigned(b.at(p))*256+b.at(p+1);}
static uint32_t r32(const std::vector<BYTE>& b,size_t p) {return uint32_t(r16(b,p))*65536+r16(b,p+2);}
static BYTE pattern(size_t offset,unsigned seed) {return BYTE(offset*71+seed);}
struct Memory : MfsSource {
    std::vector<BYTE> bytes;size_t reads=0;bool outOfBounds=false;
    explicit Memory(std::vector<BYTE> data):bytes(std::move(data)) {}
    uint64_t Length() const override {return bytes.size();}
    bool ReadAt(uint64_t offset,void* out,size_t length) override {
        ++reads;if(offset>bytes.size() || length>bytes.size()-offset) {outOfBounds=true;return false;}
        memcpy(out,bytes.data()+size_t(offset),length);return true;
    }
};
struct FaultSource : Memory {
    bool failPayload=false;
    explicit FaultSource(const std::vector<BYTE>& data):Memory(data) {}
    bool ReadAt(uint64_t offset,void* out,size_t length) override {
        if(failPayload && offset>=8192) return false;
        return Memory::ReadAt(offset,out,length);
    }
};
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
static std::vector<BYTE> dc42(const std::vector<BYTE>& raw) {
    std::vector<BYTE> result(84+raw.size()+12*raw.size()/512);
    result[0]=7;memcpy(result.data()+1,"MFSTest",7);w32(result,64,uint32_t(raw.size()));w32(result,68,uint32_t(result.size()-84-raw.size()));
    result[80]=0;result[81]=2;w16(result,82,0x0100);std::copy(raw.begin(),raw.end(),result.begin()+84);return result;
}
static void verify(MfsImage& image,const Fixture& f) {
    check(image.VolumeName()=="MFSTest" && image.Entries().size()==4,"volume name and directory enumeration");
    check(image.FreeBytes()==uint64_t(f.count-6)*1024 && image.AllocationSize()==1024,"allocation statistics");
    const MfsEntry& e=image.Entries()[0];
    check(e.name=="Read Me" && e.id==42 && e.flags==0x81 && e.version==0,"file ID/flags/version");
    check(!memcmp(e.finderInfo,"TEXTttxt",8) && e.finderInfo[8]==0x40 && e.finderInfo[9]==1,"Finder information");
    check(e.created==2700000000u && e.modified==2800000000u,"Mac epoch timestamps");
    check(image.Entries()[2].name==f.longName && MfsDisplayName(f.longName).size()==255 && MfsDisplayName(f.longName).back()==0xe9,"255-byte names and MacRoman display");
    check(image.Entries()[3].name==e.name && image.Entries()[3].version==1,"same-name file versions retained");
    const size_t lengths[]={2536,1177};
    for(unsigned fork=0;fork<2;++fork) {
        std::vector<BYTE> bytes(lengths[fork]+100,0xcc);size_t got=0;
        check(image.ReadFork(0,fork,0,bytes.data(),bytes.size(),got) && got==lengths[fork],"fragmented fork length/EOF clipping");
        for(size_t i=0;i<got;++i) check(bytes[i]==pattern(i,17+101*fork),"fragmented fork byte fidelity");
        check(bytes[got]==0xcc,"read does not expose physical padding");
        for(unsigned offset:{1020u,9u,0u,1170u}) {
            size_t expected=offset<lengths[fork]?(std::min)(size_t(47),lengths[fork]-offset):0;
            check(image.ReadFork(0,fork,offset,bytes.data(),47,got) && got==expected,"unaligned random fork reads");
            for(size_t i=0;i<got;++i) check(bytes[i]==pattern(offset+i,17+101*fork),"random read bytes");
        }
        check(image.ReadFork(0,fork,UINT64_MAX,bytes.data(),47,got) && got==0,"large out-of-EOF offset");
    }
    BYTE byte=0xcc;size_t got=99;
    check(image.ReadFork(1,0,0,&byte,1,got) && got==0 && byte==0xcc,"empty data fork");
    check(image.ReadFork(1,1,0,&byte,1,got) && got==0,"empty resource fork");
    std::vector<BYTE> tiny(32);check(image.ReadFork(2,0,0,tiny.data(),32,got) && got==17,"file after sector padding");
    for(size_t i=0;i<got;++i) check(tiny[i]==pattern(i,77),"late directory file bytes");
}
static void save(const std::wstring& path,const std::vector<BYTE>& bytes) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);check(file!=INVALID_HANDLE_VALUE,"exclusive fixture creation");
    DWORD wrote;bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&wrote,NULL) && wrote==bytes.size();CloseHandle(file);check(ok,"write fixture");
}
static std::vector<BYTE> load(const std::wstring& path) {
    MfsImageSource s;std::string error;check(s.Open(path.c_str(),error),"open file read-only");
    std::vector<BYTE> result(size_t(s.Length()));check(s.ReadAt(0,result.data(),result.size()),"read entire test file");return result;
}
static void exportTests(const std::wstring& directory,const Fixture& fixture) {
    std::wstring imagePath=directory+L"\\source.dsk",as=directory+L"\\export.as",raw=directory+L"\\export.data";
    save(imagePath,fixture.bytes);
    {
        MfsImageSource source;std::string error;MfsImage image;check(source.Open(imagePath.c_str(),error)&&image.Open(source),"disk-backed MFS open");verify(image,fixture);
        // The live source denies a competing write handle, rather than merely
        // relying on menu items being disabled.
        HANDLE write=CreateFileW(imagePath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        check(write==INVALID_HANDLE_VALUE,"source handle denies writes");
        check(MfsExportFile(image,0,as.c_str(),true,error),"AppleSingle export");
        check(MfsExportFile(image,0,raw.c_str(),false,error),"data-only export");
        check(!MfsExportFile(image,0,imagePath.c_str(),true,error),"export cannot overwrite its source");
        check(!MfsExportFile(image,0,as.c_str(),true,error),"existing exports are not overwritten");
        check(MfsExportFile(image,2,(directory+L"\\long-name.as").c_str(),true,error),"long-name AppleSingle export");
        check(MfsExportFile(image,1,(directory+L"\\empty.as").c_str(),true,error),"empty-fork AppleSingle export");
    }
    check(load(imagePath)==fixture.bytes,"source unchanged after reads and all exports");
    auto data=load(raw);check(data.size()==2536,"raw export size");for(size_t i=0;i<data.size();++i) check(data[i]==pattern(i,17),"raw export contents");
    auto single=load(as);check(r32(single,0)==0x00051600 && r32(single,4)==0x00020000 && r16(single,24)==5,"AppleSingle v2 header");
    unsigned expectedIDs[]={1,2,3,9,8},expectedLengths[]={2536,1177,7,32,16};unsigned previous=86;
    for(unsigned i=0;i<5;++i) {
        unsigned id=r32(single,26+i*12),offset=r32(single,30+i*12),length=r32(single,34+i*12);
        check(id==expectedIDs[i] && length==expectedLengths[i] && offset==previous && offset+length<=single.size(),"AppleSingle descriptor bounds");previous=offset+length;
        if(i<2) for(unsigned j=0;j<length;++j) check(single[offset+j]==pattern(j,17+i*101),"AppleSingle fork payloads");
        if(i==2) check(!memcmp(single.data()+offset,"Read Me",7),"AppleSingle original name");
        if(i==3) check(!memcmp(single.data()+offset,fixture.bytes.data()+fixture.firstEntry()+2,16) &&
                         std::all_of(single.begin()+offset+16,single.begin()+offset+32,[](BYTE b){return b==0;}),"AppleSingle Finder metadata");
        if(i==4) check(r32(single,offset)==uint32_t(int64_t(2700000000u)-3029529600LL) &&
                         r32(single,offset+4)==uint32_t(int64_t(2800000000u)-3029529600LL) && r32(single,offset+8)==0x80000000u,"AppleSingle date epoch");
    }
    check(previous==single.size(),"AppleSingle total length");
    auto longName=load(directory+L"\\long-name.as");unsigned pos=r32(longName,30+2*12);
    check(r32(longName,34+2*12)==255 && !memcmp(longName.data()+pos,fixture.longName.data(),255),"export preserves full original long name");
    FaultSource faulty(fixture.bytes);MfsImage broken;check(broken.Open(faulty),"open fault-injection source");faulty.failPayload=true;
    std::string error;std::wstring incomplete=directory+L"\\incomplete.as";
    check(!MfsExportFile(broken,0,incomplete.c_str(),true,error) && GetFileAttributesW(incomplete.c_str())==INVALID_FILE_ATTRIBUTES,"failed read removes incomplete export");
    check(faulty.bytes==fixture.bytes,"failed export leaves source unchanged");
}
int wmain(int argc,wchar_t** argv) {
    try {
        check(argc==2,"Usage: mfs-readonly NEW_TEST_DIRECTORY");std::wstring directory=argv[1];
        check(CreateDirectoryW(directory.c_str(),NULL)!=FALSE,"exclusive test directory");
        Fixture fixture;
        for(bool larger:{false,true}) {
            Fixture f(larger);Memory memory(f.bytes);MfsImage image;check(MfsImage::Recognizes(memory)&&image.Open(memory),"raw MFS recognition/open");verify(image,f);
            check(memory.bytes==f.bytes && !memory.outOfBounds,"bounded read-only memory access");
            Memory container(dc42(f.bytes));check(MfsImage::Recognizes(container)&&image.Open(container),"Disk Copy 4.2 MFS open");verify(image,f);
        }
        // Directory-within-allocation-area variant allowed by Apple's format.
        { Fixture f;w16(f.bytes,1052,4);f.first=4;
          for(unsigned b=2;b<8;++b) f.link(b,0xfff);
          // Empty files leave no file chains overlapping the reserved directory.
          w16(f.bytes,1036,0);std::fill(f.bytes.begin()+2048,f.bytes.begin()+8192,BYTE(0));
          Memory source(f.bytes);MfsImage image;check(image.Open(source)&&image.Entries().empty(),"reserved directory blocks inside allocation area"); }
        std::vector<std::function<void(Fixture&)>> corrupt={
            [](Fixture& f){w16(f.bytes,1042,4094);},
            [](Fixture& f){w32(f.bytes,1044,0);},
            [](Fixture& f){w32(f.bytes,1044,513);},
            [](Fixture& f){w16(f.bytes,1038,2);},
            [](Fixture& f){w16(f.bytes,1040,65535);},
            [](Fixture& f){w16(f.bytes,1052,65535);},
            [](Fixture& f){f.bytes[1060]=28;},
            [](Fixture& f){f.bytes.resize(3000);},
            [](Fixture& f){f.link(2,2);},
            [](Fixture& f){f.link(2,0);},
            [](Fixture& f){f.link(2,1);},
            [](Fixture& f){f.link(2,4000);},
            [](Fixture& f){f.link(2,0xfff);},
            [](Fixture& f){f.link(4,8);f.link(8,1);},
            [](Fixture& f){w16(f.bytes,f.firstEntry()+32,2);},
            [](Fixture& f){w32(f.bytes,f.firstEntry()+24,4096);},
            [](Fixture& f){w32(f.bytes,f.firstEntry()+28,3071);},
            [](Fixture& f){f.bytes[f.firstEntry()+50]=0;},
            [](Fixture& f){f.bytes[f.firstEntry()+51]=':';},
            [](Fixture& f){w32(f.bytes,f.firstEntry()+512+18,42);},
            [](Fixture& f){w16(f.bytes,1036,9);},
            [](Fixture& f){w16(f.bytes,f.firstEntry()+58+22,2);},
            [](Fixture& f){f.bytes[f.firstEntry()+512+306+50]=255;},
            [](Fixture& f){f.bytes[f.firstEntry()]=0x82;}
        };
        for(size_t i=0;i<corrupt.size();++i) {
            Fixture f;corrupt[i](f);Memory source(f.bytes);MfsImage image;
            if(image.Open(source)) {fprintf(stderr,"Accepted corruption %zu\n",i);check(false,"reject corrupt MFS");}
            check(source.reads<100 && !image.Error().empty(),"corrupt input bounded failure");
        }
        { auto bytes=dc42(fixture.bytes);w32(bytes,64,0xffffffff);Memory source(bytes);MfsImage image;check(!image.Open(source),"reject bad DC42 lengths"); }
        { Memory source(std::vector<BYTE>(409600));MfsImage image;check(!MfsImage::Recognizes(source)&&!image.Open(source),"non-MFS routing"); }
        exportTests(directory,fixture);
        printf("PASS: read-only MFS: raw 400/800 KiB and DC42, flat directory, 255-byte MacRoman names, versions, both fragmented forks, seeks/EOF, Finder metadata, AppleSingle/data exports, corrupt chains/layouts, write exclusion and unchanged source\n");
        return 0;
    } catch(const std::exception& error) {fprintf(stderr,"FAIL: MFS: %s\n",error.what());return 1;}
}
