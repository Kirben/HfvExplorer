// SPDX-License-Identifier: GPL-2.0-or-later
#include "StorageOutcome.h"
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

struct WritableMemory:Memory {
 unsigned writes=0,flushes=0;int failWrite=-1,failFlush=-1;bool enabled=true,shortWrite=false,persistent=false;
 explicit WritableMemory(size_t size):Memory(std::vector<BYTE>(size,0)){}
 bool Writable()const override{return enabled;}
 void DisableWrites()override{enabled=false;}
 bool WriteAt(uint64_t at,const void* p,size_t n)override{
  unsigned call=writes++;if(at>bytes.size()||n>bytes.size()-at)return false;
  if(failWrite>=0&&call>=unsigned(failWrite)&&(persistent||call==unsigned(failWrite))){if(shortWrite&&n)memcpy(bytes.data()+size_t(at),p,n/2);return false;}memcpy(bytes.data()+size_t(at),p,n);return true;
 }
 bool Flush()override{unsigned call=flushes++;return !(failFlush>=0&&call>=unsigned(failFlush)&&(persistent||call==unsigned(failFlush)));}
};
static void writeTests(){
 auto reader=[](unsigned fork,uint64_t at,void* p,size_t n){for(size_t i=0;i<n;++i)static_cast<BYTE*>(p)[i]=BYTE((at+i)*31+fork*89);};
 for(bool container:{false,true})for(size_t size:{size_t(400*1024),size_t(800*1024)}){
  WritableMemory disk(size+(container?84+size/512*12:0));std::string error;check(MfsImage::Format(disk,"Writable",size,container,error),error.c_str());MfsImage image;check(image.Open(disk)&&image.Writable(),"open writable formatted MFS");size_t base=container?84:0;
  check(r16(disk.bytes,base+1024)==0xd2d7&&r16(disk.bytes,base+1038)==4&&r16(disk.bytes,base+1040)>=12,"independent formatter layout fields");
  std::string encoded;check(MfsImage::EncodeName(L"caf\u00e9",encoded,error)&&BYTE(encoded.back())==0x8e,"MacRoman encoding");check(MfsImage::EncodeName(L"cafe\u0301",encoded,error)&&BYTE(encoded.back())==0x8e,"decomposed MacRoman name encoding");
  MfsEntry e;e.name=std::string(254,'L')+char(0x8e);e.version=7;e.flags=0x80;e.created=2700000001u;e.modified=2800000002u;memcpy(e.finderInfo,"TEXTttxt",8);e.finderInfo[10]=17;e.forks[0].length=13003;e.forks[1].length=7001;uint32_t id;
  check(image.Put(e,reader,false,id),image.Error().c_str());check(id!=0&&image.Entries()[0].name==e.name&&image.Entries()[0].version==7,"MFS new ID/long name/version");
  for(unsigned f=0;f<2;++f){std::vector<BYTE> actual(e.forks[f].length),wanted(actual.size());size_t got=0;reader(f,0,wanted.data(),wanted.size());check(image.ReadFork(0,f,0,actual.data(),actual.size(),got)&&got==actual.size()&&actual==wanted,"written MFS fork fidelity");}
  auto second=e;second.version=8;second.forks[0].length=second.forks[1].length=0;uint32_t other;check(image.Put(second,reader,false,other)&&other!=id&&image.Entries().size()==2,"same-name distinct MFS versions");
  check(image.Rename(id,"Renamed"),"rename attributed MFS file");auto saved=disk.bytes;auto current=image.Entries()[0];current.flags|=1;check(image.SetMetadata(id,current),"lock file");check(!image.Remove(id),"locked file deletion allowed");check(image.Open(disk),"reopen after locked rejection");current.flags&=~1;current.version=9;current.finderInfo[12]=0x45;check(image.SetMetadata(id,current),"unlock/edit version and Finder info");
  current.name="Renamed";current.forks[0].length=3;current.forks[1].length=0;uint32_t replaced;check(image.Put(current,reader,true,replaced)&&replaced==id,"overwrite changed MFS file number");check(image.Entries()[0].forks[0].length==3&&image.Entries()[0].forks[1].length==0,"fork shrinking");
  check(image.RenameVolume("Volume renamed")&&image.VolumeName()=="Volume renamed","volume rename");check(image.Remove(id)&&image.Remove(other)&&image.Entries().empty(),"remove both fork/version entries");
  if(container){auto sum=[](const std::vector<BYTE>& b,size_t start,size_t count){uint32_t c=0;for(size_t i=0;i<count;i+=2){c+=unsigned(b[start+i])*256+b[start+i+1];bool low=c&1;c/=2;if(low)c+=0x80000000u;}return c;};check(r32(disk.bytes,72)==sum(disk.bytes,84,size)&&r32(disk.bytes,76)==sum(disk.bytes,84+size+12,size/512*12-12),"DC42 data/tag checksums not updated");}
 }

 // Directory blocks can occupy allocation blocks: preserve their reserved map entries.
 {WritableMemory disk(400*1024);std::string error;check(MfsImage::Format(disk,"Reserved",disk.Length(),false,error),"format reserved fixture");w16(disk.bytes,1052,4);unsigned count=r16(disk.bytes,1042);auto map=[&](unsigned block,unsigned value){for(unsigned bit=0;bit<12;++bit){size_t position=block*12+bit;BYTE mask=BYTE(1u<<(7-position%8));auto& byte=disk.bytes[1088+position/8];byte=BYTE((byte&~mask)|((value&(1u<<(11-bit)))?mask:0));}};for(unsigned i=0;i<6;++i)map(i,0xfff);w16(disk.bytes,1058,count-6);MfsImage image;check(image.Open(disk)&&image.Writable(),"reserved allocation directory not writable");MfsEntry e;e.name="Data";e.forks[0].length=2503;uint32_t id;check(image.Put(e,reader,false,id),"write beside reserved directory");check(image.Entries()[0].forks[0].blocks.front()>=8,"allocator reused reserved directory blocks");check(image.Remove(id),"remove beside reserved directory");for(unsigned i=0;i<9;++i)check(disk.bytes[1088+i]==0xff,"reserved directory bitmap damaged");}
 // The highest 12-bit block ID is valid as a chain target; only directory
 // geometry makes 0xFFF a reserved-map marker rather than a next-block value.
 {Fixture fixture;fixture.count=4094;fixture.allocation=512;fixture.directory=15;fixture.directoryBlocks=12;fixture.first=27;fixture.bytes.assign((27+4094)*512,0);w16(fixture.bytes,1024,0xd2d7);w16(fixture.bytes,1036,1);w16(fixture.bytes,1038,15);w16(fixture.bytes,1040,12);w16(fixture.bytes,1042,4094);w32(fixture.bytes,1044,512);w32(fixture.bytes,1048,512);w16(fixture.bytes,1052,27);w32(fixture.bytes,1054,2);w16(fixture.bytes,1058,4092);fixture.bytes[1060]=3;memcpy(fixture.bytes.data()+1061,"Max",3);fixture.record(0,"Last",1,0,2,1024,1024,0,0,0);fixture.link(2,4095);fixture.link(4095,1);Memory source(fixture.bytes);MfsImage image;check(image.Open(source)&&image.Entries()[0].forks[0].blocks.back()==4095,"highest MFS allocation block rejected");}
 // Inconsistent free counts and corrupt DC42 checksums stay readable but cannot be written.
 {WritableMemory disk(400*1024);std::string error;check(MfsImage::Format(disk,"Audit",disk.Length(),false,error),"format audit fixture");w16(disk.bytes,1058,r16(disk.bytes,1058)-1);MfsImage image;check(image.Open(disk)&&!image.Writable()&&!image.WriteError().empty(),"inconsistent free count allowed writes");}
 {size_t size=400*1024;WritableMemory disk(84+size+size/512*12);std::string error;check(MfsImage::Format(disk,"Checksum",size,true,error),"format checksum fixture");disk.bytes[84+100*512]^=1;MfsImage image;check(image.Open(disk)&&!image.Writable(),"bad DC42 checksum allowed writes");}

 // Write at the upper supported raw-image size, and exercise noncontiguous
 // allocation after a nearly full volume has developed alternating holes.
 {WritableMemory disk(20*1024*1024);std::string error;check(MfsImage::Format(disk,"Twenty MiB",disk.Length(),false,error),"format maximum MFS volume");MfsImage image;check(image.Open(disk)&&image.Writable()&&image.FreeBytes()>19*1024*1024,"maximum MFS layout/free count");MfsEntry e;e.name="Large";e.forks[0].length=17*1024*1024+3;e.forks[1].length=513;uint32_t id;check(image.Put(e,reader,false,id),"write large MFS fork");BYTE bytes[7],expected[7];size_t got=0;reader(0,17*1024*1024-4,expected,7);check(image.ReadFork(0,0,17*1024*1024-4,bytes,7,got)&&got==7&&!memcmp(bytes,expected,7),"large MFS tail bytes");}
 {WritableMemory disk(400*1024);std::string error;check(MfsImage::Format(disk,"Fragmented",disk.Length(),false,error),"format fragmentation fixture");MfsImage image;check(image.Open(disk),"open fragmentation fixture");std::vector<uint32_t> files;for(unsigned i=0;i<20;++i){MfsEntry e;e.name="Block"+std::string(1,char('A'+i));e.forks[0].length=1024;uint32_t id;check(image.Put(e,reader,false,id),"write fragmentation block");files.push_back(id);}MfsEntry filler;filler.name="Filler";filler.forks[0].length=uint32_t(image.FreeBytes()-2048);uint32_t id;check(image.Put(filler,reader,false,id),"fill fragmentation fixture");for(unsigned i=0;i<files.size();i+=2)check(image.Remove(files[i]),"release fragmentation hole");MfsEntry e;e.name="Split forks";e.forks[0].length=6003;e.forks[1].length=5001;check(image.Put(e,reader,false,id),"write fragmented MFS forks");auto& file=image.Entries().back();check(file.forks[0].blocks.size()>1&&file.forks[0].blocks[1]!=file.forks[0].blocks[0]+1,"fragmentation fixture remained contiguous");for(unsigned f=0;f<2;++f){std::vector<BYTE> bytes(e.forks[f].length),expected(bytes.size());size_t got=0;reader(f,0,expected.data(),expected.size());check(image.ReadFork(image.Entries().size()-1,f,0,bytes.data(),bytes.size(),got)&&bytes==expected,"fragmented written fork contents");}}
 WritableMemory base(400*1024);std::string error;check(MfsImage::Format(base,"Failures",base.Length(),false,error),"format failure base");MfsImage original;check(original.Open(base),"open failure base");MfsEntry e;e.name="Old";e.flags=0x80;e.forks[0].length=7301;e.forks[1].length=2903;uint32_t id;check(original.Put(e,reader,false,id),"seed failure base");auto changed=e;changed.forks[0].length=33007;changed.forks[1].length=19001;
 WritableMemory probe=base;MfsImage p;check(p.Open(probe)&&p.Put(changed,reader,true,id),"probe MFS overwrite");unsigned writes=probe.writes-base.writes,flushes=probe.flushes-base.flushes;
 for(bool flush:{false,true})for(bool shortWrite:{false,true})for(unsigned step=0;step<(flush?flushes:writes);++step){auto disk=base;disk.shortWrite=shortWrite;if(flush)disk.failFlush=int(disk.flushes+step);else disk.failWrite=int(disk.writes+step);MfsImage image;check(image.Open(disk),"open injected MFS source");uint32_t target;Storage::OutcomeEvidence evidence;check(!image.Put(changed,reader,true,target)&&evidence.RollbackState()==Storage::Rollback::Restored,"MFS write/flush failure did not report restored rollback");check(disk.bytes==base.bytes,"MFS rollback did not restore exact image bytes");disk.failWrite=disk.failFlush=-1;check(image.Open(disk)&&image.Entries()[0].forks[0].length==7301,"MFS rollback not readable");}
 {auto disk=base;MfsImage image;check(image.Open(disk),"open canceled source");bool failed=false;try{image.Put(changed,[](unsigned,uint64_t,void*,size_t){throw std::runtime_error("Canceled source read");},true,id);}catch(const std::exception&){failed=true;}check(failed&&disk.bytes==base.bytes,"source error changed MFS image");auto huge=changed;huge.forks[0].length=400*1024;check(!image.Put(huge,reader,true,id)&&disk.bytes==base.bytes,"disk-full overwrite changed image");}
 {WritableMemory disk(400*1024);check(MfsImage::Format(disk,"Directory",disk.Length(),false,error),"format directory-full test");MfsImage image;check(image.Open(disk),"open directory-full test");for(unsigned i=0;i<12;++i){MfsEntry item;item.name=std::string(254,'a')+char('A'+i);uint32_t file;check(image.Put(item,reader,false,file),"fill directory sectors");}auto saved=disk.bytes;MfsEntry item;item.name=std::string(255,'z');uint32_t file;check(!image.Put(item,reader,false,file)&&disk.bytes==saved,"directory-full create changed image");}
 {
  auto disk=base;
  disk.failWrite=int(disk.writes);
  disk.persistent=true;
  disk.shortWrite=true;
  MfsImage image;
  check(image.Open(disk),"open failed rollback test");
  Storage::OutcomeEvidence evidence;
  check(!image.Put(changed,reader,true,id)&&evidence.RollbackState()==Storage::Rollback::Failed&&
        image.RecoveryRequired()&&!disk.Writable(),"failed rollback did not disable writes");
  const std::string marker="Original complete image saved at: ";
  const auto& message=image.Error();
  const auto start=message.find(marker);
  check(start!=std::string::npos,"failed rollback did not retain recovery image");
  const auto begin=start+marker.size(),end=message.find(". Verify",begin);
  check(end!=std::string::npos,"missing recovery path terminator");
  const auto path=message.substr(begin,end-begin);
  wchar_t wide[MAX_PATH]{};
  check(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,wide,MAX_PATH)!=0,
        "invalid recovery path encoding");
  const auto backup=CreateFileW(wide,GENERIC_READ|DELETE,0,nullptr,OPEN_EXISTING,
                                FILE_FLAG_DELETE_ON_CLOSE,nullptr);
  check(backup!=INVALID_HANDLE_VALUE,"open saved recovery image");
  std::vector<BYTE> saved(base.bytes.size());
  LARGE_INTEGER length{};
  DWORD read=0;
  const bool complete=GetFileSizeEx(backup,&length)&&length.QuadPart==LONGLONG(saved.size())&&
                      ReadFile(backup,saved.data(),DWORD(saved.size()),&read,nullptr)&&read==saved.size();
  CloseHandle(backup);
  check(complete&&saved==base.bytes,"recovery image did not preserve exact pre-operation bytes");
 }
 puts("PASS: writable MFS raw/DC42 format, both forks, 255-byte MacRoman names, versions, rename, metadata/lock, shrink, delete, checksum updates; exact rollback on every write/short-write/flush failure; disk/directory full and source errors; recovery backup after rollback failure");
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
        writeTests();
        exportTests(directory,fixture);
        printf("PASS: read-only MFS: raw 400/800 KiB and DC42, flat directory, 255-byte MacRoman names, versions, both fragmented forks, seeks/EOF, Finder metadata, AppleSingle/data exports, corrupt chains/layouts, write exclusion and unchanged source\n");
        return 0;
    } catch(const std::exception& error) {fprintf(stderr,"FAIL: MFS: %s\n",error.what());return 1;}
}
