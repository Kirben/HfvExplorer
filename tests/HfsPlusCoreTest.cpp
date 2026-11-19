// SPDX-License-Identifier: GPL-2.0-or-later
#include "StorageOutcome.h"
#include "HfsPlusDisk.h"
#include <cstdio>
using namespace HfsPlus;
void RunIncrementalBTreeTests();
void RunHfsPlusCompressionTests();
int RunHfsPlusOtherFeaturesTests(const wchar_t*);
void CreateHfsPlusFeatureFixture(const wchar_t *);
void CreateHfsPlusCompressionEncodingFixture(const wchar_t*);
void must(bool ok, const char *text) {
    if (!ok)
        throw Error(text);
}
struct Memory : Source {
    Bytes bytes;
    bool writable = true;
    unsigned writes = 0, flushes = 0;
    int failWrite = -1, failFlush = -1;
    explicit Memory(size_t size) : bytes(size, 0) {}
    uint64_t Size() const override { return bytes.size(); }
    bool Writable() const override { return writable; }
    void Read(uint64_t o, void *p, size_t n) override {
        must(Disk::Range(o, n, bytes.size()), "Memory read range");
        memcpy(p, bytes.data() + o, n);
    }
    void Write(uint64_t o, const void *p, size_t n) override {
        if (failWrite >= 0 && writes == unsigned(failWrite))
            throw Error("Injected metadata write error");
        must(writable && Disk::Range(o, n, bytes.size()), "Memory write range");
        memcpy(bytes.data() + o, p, n);
        ++writes;
    }
    void Flush() override {
        if (failFlush >= 0 && flushes == unsigned(failFlush))
            throw Error("Injected metadata flush error");
        ++flushes;
    }
};
template <class F> void fails(F f, const char *message) {
    bool threw = false;
    try {
        f();
    } catch (const Error &) {
        threw = true;
    }
    must(threw, message);
}
auto pattern = [](unsigned f, uint64_t offset, void *p, size_t n) {
    auto b = static_cast<uint8_t *>(p);
    for (size_t i = 0; i < n; ++i)
        b[i] = uint8_t(((offset + i) * 13) ^ ((offset + i) >> 21) ^ ((offset + i) >> 24) ^
                       ((offset + i) >> 32) ^ (f * 71));
};
void verify(Volume &v, uint32_t id, unsigned fork, uint64_t size) {
    Bytes actual(10003), expected(actual.size());
    for (uint64_t o = 0; o < size;) {
        size_t n = size_t((std::min)(uint64_t(actual.size()), size - o));
        must(v.Read(id, fork, o, actual.data(), n) == n, "Read fork length");
        pattern(fork, o, expected.data(), n);
        must(!memcmp(actual.data(), expected.data(), n), "Fork bytes differ");
        o += n;
    }
    must(v.Read(id, fork, size, actual.data(), 1) == 0, "EOF");
}

uint8_t *catalogValue(Memory &,uint32_t);
Bytes security(unsigned count=2) {
    Bytes b(44+count*24,0);Disk::P32(b.data(),0x012cc16d);Disk::P32(b.data()+36,count);Disk::P32(b.data()+40,0x20000);
    for(unsigned i=0;i<32;++i)b[4+i]=uint8_t(i+1);
    for(unsigned i=0;i<count;++i){for(unsigned j=0;j<16;++j)b[44+i*24+j]=uint8_t(i+j+7);Disk::P32(b.data()+60+i*24,(i%2?2:1)|0x60);Disk::P32(b.data()+64+i*24,0x100083);}
    return b;
}
void attributeTests() {
    for(auto kind:{Volume::FormatKind::HfsPlus,Volume::FormatKind::HfsXCaseSensitive,Volume::FormatKind::HfsXCaseInsensitive})for(bool journal:{false,true}){
        Memory disk(32*1024*1024);Volume::Format(disk,u"Attributes",journal,kind);
        uint32_t file,folder;Attributes original{{u"user.Empty",{}},{u"user.Case",{1,0,2,3}},{u"user.case",Bytes(64003,0x83)},{u"user.\u00e9",{0xff,0,0xfe}},{u"com.apple.system.Security",security()}};
        {Volume v(disk,true);file=v.Put(2,u"File",1003,779,pattern);folder=v.MakeDirectory(2,u"Folder");v.SetAttributes(file,original);v.SetAttributes(folder,original);must(v.GetAttributes(file)==original,"Attribute write roundtrip");must((Disk::U16(catalogValue(disk,file)+2)&12)==12&&(Disk::U16(catalogValue(disk,folder)+2)&12)==12,"Catalog attribute/ACL flags missing");}
        {Volume v(disk,true);auto values=original;values[u"user.SmallDisk"]=AttributeData::Snapshot(17,[](uint64_t,void* p,size_t n){memset(p,0x83,n);},true);v.SetAttributes(file,values);must(v.GetAttributes(file)==values,"Small file-backed inline attribute failed");v.SetAttributes(file,original);}
        {Volume v(disk,true);must(v.GetAttributes(folder)==original,"Directory ACL reopen");v.Rename(file,folder,u"Renamed");v.Put(folder,u"Renamed",53,61,pattern,nullptr,true);must(v.GetAttributes(file)==original,"Rename/overwrite discarded attributes");verify(v,file,0,53);verify(v,file,1,61);auto changed=original;changed.erase(u"user.Case");changed[u"user.case"]={4,5,0,6};v.SetAttributes(file,changed);must(v.GetAttributes(file)==changed&&v.GetAttributes(folder)==original,"Attribute replacement affected another inode");v.SetAttributes(file,{});must(v.GetAttributes(file).empty()&&!(Disk::U16(catalogValue(disk,file)+2)&12),"Remove all attributes or clear catalog flags");v.SetAttributes(file,original);}
        {disk.writable=false;Volume v(disk,false);must(v.GetAttributes(file)==original,"Read-only attributes");fails([&]{v.SetAttributes(file,{});},"Read-only attribute write");disk.writable=true;}
        {Volume v(disk,true);auto before=v.FreeBytes();v.Remove(file);must(v.FreeBytes()>before,"Fork-backed attributes leaked blocks on deletion");v.Remove(folder);}
        {Volume v(disk,true);for(auto& e:v.List())must(e.id!=file&&e.id!=folder,"Attributed directory removal left catalog entries");}
        Attributes invalid{{u"com.apple.system.Security",security()}};invalid.begin()->second.pop_back();fails([&]{ValidateAttributes(invalid);},"Truncated ACL accepted");
        invalid={{u"com.apple.system.Security",security(0)}};Disk::P32(invalid.begin()->second.data()+36,0xffffffff);ValidateAttributes(invalid); // absent ACL differs from an empty ACL
        invalid={{std::u16string(128,u'a'),{1}}};fails([&]{ValidateAttributes(invalid);},"Overlong attribute name accepted");
        invalid={{u"com.apple.decmpfs",{1}}};fails([&]{ValidateAttributes(invalid);},"Compression metadata writable through ordinary attributes");
    }
    Memory base(32*1024*1024);Volume::Format(base,u"Recovery",true);uint32_t id;Attributes old{{u"user.Value",Bytes(6000,0x17)},{u"com.apple.system.Security",security()}};
    {Volume v(base,true);id=v.Put(2,u"File",5003,723,pattern);v.SetAttributes(id,old);}
    Attributes next{{u"user.Value",Bytes(79003,0x82)},{u"user.New",{1,2,0,4}},{u"com.apple.system.Security",security(1)}};
    for(unsigned operation=0;operation<3;++operation)for(const char* phase:{"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}){
        Storage::OutcomeEvidence evidence;
        Memory disk=base;{Volume v(disk,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("Attribute interruption");};fails([&]{if(operation==0)v.SetAttributes(id,next);else if(operation==1)v.Remove(id);else v.Put(2,u"File",81,37,pattern,nullptr,true,nullptr,&next);},"Missing attribute fault boundary");}
        auto unchanged=disk.bytes;Volume replay(disk,false);bool committed=strcmp(phase,"data-flushed")&&strcmp(phase,"journal-data-flushed");
        must(evidence.RollbackState()==(committed?Storage::Rollback::Unverified:Storage::Rollback::Restored),
             "Journal fault did not report its publication boundary");
        if(operation==1&&committed){for(auto& e:replay.List())must(e.id!=id,"Committed attribute deletion not replayed");}
        else{must(replay.GetAttributes(id)==(committed?next:old),"Attribute transaction recovered mixed state");verify(replay,id,0,operation==2&&committed?81:5003);verify(replay,id,1,operation==2&&committed?37:723);}
        must(disk.bytes==unchanged,"Read-only attribute replay wrote image");
    }
    // Every reported payload/metadata write and flush failure must recover an
    // entire old or new attribute set; ordinary forks must remain unchanged.
    {Memory probe=base;unsigned writes=probe.writes,flushes=probe.flushes;{Volume v(probe,true);v.SetAttributes(id,next);}unsigned writeCount=probe.writes-writes,flushCount=probe.flushes-flushes;
        for(bool flush:{false,true})for(unsigned step=0;step<(flush?flushCount:writeCount);++step){Memory disk=base;if(flush)disk.failFlush=int(flushes+step);else disk.failWrite=int(writes+step);
            {Volume v(disk,true);fails([&]{v.SetAttributes(id,next);},"Injected attribute I/O fault was ignored");}disk.failWrite=disk.failFlush=-1;Volume recovered(disk,false);auto result=recovered.GetAttributes(id);must(result==old||result==next,"Attribute I/O failure recovered mixed metadata");verify(recovered,id,0,5003);verify(recovered,id,1,723);}}
    // Incremental splits/merges and catalog flags, with two case-distinct names.
    {Memory disk(64*1024*1024);Volume::Format(disk,u"Many attributes",true);Volume v(disk,true);auto id=v.Put(2,u"Many",1,0,pattern);Attributes a;
        for(unsigned i=0;i<900;++i){auto name=u"user."+std::u16string(1,char16_t(0x100+i))+std::u16string(110,u'z');a[name]=Bytes(63,uint8_t(i));}v.SetAttributes(id,a);must(v.GetAttributes(id)==a,"Attribute B-tree splits lost records");
        for(auto it=a.begin();it!=a.end();)if(uint16_t(it->first[5])%3)it=a.erase(it);else ++it;v.SetAttributes(id,a);Volume reopened(disk,true);must(reopened.GetAttributes(id)==a,"Attribute B-tree merges lost records");}
    // Grow the attributes tree and the journal in one pending user transaction.
    {Memory disk(32*1024*1024);Volume::Format(disk,u"Attribute journal growth",true);auto journalBytes=[&]{return Disk::U64(disk.bytes.data()+uint64_t(Disk::U32(disk.bytes.data()+1036))*4096+44);};auto initialJournal=journalBytes();uint32_t file;Attributes a;
        for(unsigned i=0;i<1300;++i)a[u"user."+std::u16string(1,char16_t(0x100+i))+std::u16string(80,u'x')]=Bytes(1000,uint8_t(i));
        {Volume v(disk,true);file=v.Put(2,u"File",21,13,pattern);v.SetAttributes(file,a);}
        Volume v(disk,false);must(v.GetAttributes(file)==a&&journalBytes()>initialJournal,"Journal growth lost pending attributes or was not exercised");verify(v,file,0,21);verify(v,file,1,13);}
    // Force more than eight payload extents, using first-fit holes in a full image.
    {Memory disk(8*1024*1024);Volume::Format(disk,u"Fragmented attributes",false);uint32_t file;Attributes desired{{u"user.Fragmented",Bytes(70003,0x75)}};
        {Volume v(disk,true);file=v.Put(2,u"File",0,0,pattern);v.SetAttributes(file,{{u"user.Small",{1}}});std::vector<uint32_t> holes;
            for(unsigned i=0;i<40;++i)holes.push_back(v.Put(2,u"Block"+std::u16string(1,char16_t(0xe000+i)),4096,0,pattern));
            v.Put(2,u"Filler",v.FreeBytes()-4*4096,0,pattern);for(unsigned i=0;i<holes.size();i+=2)v.Remove(holes[i]);
            auto before=v.FreeBytes();v.SetAttributes(file,desired);must(v.GetAttributes(file)==desired,"Fragmented attribute write");v.SetAttributes(file,{});must(v.FreeBytes()==before,"Fragmented attribute deletion leaked extents");v.SetAttributes(file,desired);}
        Volume v(disk,false);must(v.GetAttributes(file)==desired,"Fragmented attribute overflow extents failed after reopen");}
    // Failure while writing new fork data must leave the committed old attributes.
    {Memory disk=base;auto saved=disk.bytes;disk.failWrite=int(disk.writes);{Volume v(disk,true);fails([&]{v.SetAttributes(id,next);},"Attribute payload write error ignored");}disk.failWrite=-1;Volume v(disk,false);must(v.GetAttributes(id)==old,"Failed payload write lost old attributes");}
    puts("PASS: HFS+/HFSX inline/fork attributes and Mac ACLs, case-sensitive keys, folders, overwrite preservation, deletion reclamation, split/merge, read-only access and journal crash recovery");
}
void stagedRangeValidation() {
    Memory disk(4096);
    Disk::View view(disk);
    Disk::Sector bytes; bytes.fill(0x71);
    view.Stage(0, bytes.data(), bytes.size());
    view.Stage(3584, bytes.data(), bytes.size());
    view.length = 2048; // A replayed header can reduce the final volume range.
    auto original = disk.bytes;
    auto writes = disk.writes, flushes = disk.flushes;
    fails([&] { view.Apply(); }, "Out-of-range staged write accepted");
    must(disk.bytes == original && disk.writes == writes && disk.flushes == flushes,
         "Invalid staged range caused partial image writes");
    puts("PASS: all staged metadata ranges are validated before any image write");
}
void basic(bool journaled) {
    Memory disk(32 * 1024 * 1024);
    Volume::Format(disk, u"Test \u00e9", journaled);
    uint32_t file = 0, folder = 0;
    {
        Volume v(disk, true);
        must(v.Journaled() == journaled, "Journal state");
        must(v.Name() == u"Test e\u0301", "Decomposed volume name");
        folder = v.MakeDirectory(2, u"Folder");
        std::array<uint8_t, 32> finder{};
        memcpy(finder.data(), "TEXTttxt", 8);
        file = v.Put(folder, u"\u00c9xample", 8197, 9911, pattern, &finder);
        verify(v, file, 0, 8197);
        verify(v, file, 1, 9911);
        must(v.Stat(file).finder == finder, "Finder info");
        fails([&] { v.Put(folder, u"e\u0301XAMPLE", 1, 0, pattern); }, "Duplicate folded name accepted");
        v.Rename(file, 2, u"Renamed");
        must(v.List(folder).empty(), "Moved file remains");
        v.Remove(folder);
    }
    {
        Volume v(disk, true);
        verify(v, file, 1, 9911);
        v.Put(2, u"Renamed", 99, 0, pattern, nullptr, true);
        verify(v, file, 0, 99);
        v.Remove(file);
        must(v.List().size() == (journaled ? 2 : 0), "Unexpected root entries");
    }
    disk.writable = false;
    unsigned writes = disk.writes;
    {
        Volume v(disk);
        fails([&] { v.MakeDirectory(2, u"No"); }, "Read-only mutation accepted");
    }
    must(writes == disk.writes, "Read-only mount wrote image");
}
void crashes(Volume::FormatKind kind = Volume::FormatKind::HfsPlus) {
    Memory clean(16 * 1024 * 1024);
    Volume::Format(clean, u"Crash test", true, kind);
    for (const char *phase :
         {"data-flushed", "journal-data-flushed", "journal-committed", "home-flushed", "journal-cleared"}) {
        Memory disk = clean;
        {
            Volume v(disk, true);
            v.fault = [&](const char *at) {
                if (!strcmp(at, phase))
                    throw Error("Injected power failure");
            };
            fails([&] { v.MakeDirectory(2, u"Committed"); }, "Crash injection did not fire");
        }
        bool committed = strcmp(phase, "data-flushed") && strcmp(phase, "journal-data-flushed");
        disk.writable = false;
        unsigned before = disk.writes;
        {
            Volume v(disk);
            bool found = false;
            for (auto &e : v.List())
                found |= e.name == u"Committed";
            must(found == committed, "Read-only journal replay state");
        }
        must(before == disk.writes, "Read-only recovery wrote image");
        disk.writable = true;
        {
            Volume v(disk, true);
            bool found = false;
            for (auto &e : v.List())
                found |= e.name == u"Committed";
            must(found == committed, "Writable recovery state");
            v.MakeDirectory(2, u"Next");
        }
    }
    // A committed but damaged transaction must not modify any home blocks.
    Memory disk = clean;
    {
        Volume v(disk, true);
        v.fault = [](const char *p) {
            if (!strcmp(p, "journal-committed"))
                throw Error("crash");
        };
        fails([&] { v.MakeDirectory(2, u"Bad"); }, "Inject committed log");
    }
    auto vh = disk.bytes.data() + 1024;
    uint64_t info = uint64_t(Disk::U32(vh + 12)) * Disk::U32(vh + 40),
             offset = Disk::U64(disk.bytes.data() + info + 36);
    disk.bytes[size_t(offset) + 512 + 4096] ^= 1;
    auto original = disk.bytes;
    fails([&] { Volume v(disk, true); }, "Corrupt journal accepted");
    must(disk.bytes == original, "Failed replay changed image");
    // A valid redo log does not authorize writes to an otherwise damaged filesystem.
    Memory damaged = clean;
    {
        Volume v(damaged, true);
        v.fault = [](const char *at) {
            if (!strcmp(at, "journal-committed")) throw Error("Committed redo fixture");
        };
        fails([&] { v.MakeDirectory(2, u"Pending recovery"); }, "Create valid pending redo");
    }
    auto header = damaged.bytes.data() + 1024;
    auto allocation = uint64_t(Disk::U32(header + 128)) * Disk::U32(header + 40);
    damaged.bytes[size_t(allocation)] &= 0x7f; // Reserved boot block falsely marked free.
    auto unchanged = damaged.bytes;
    auto writesBefore = damaged.writes, flushesBefore = damaged.flushes;
    fails([&] { Volume v(damaged); }, "Damaged replayed allocation accepted read-only");
    fails([&] { Volume v(damaged, true); }, "Damaged replayed allocation accepted writable");
    must(damaged.bytes == unchanged && damaged.writes == writesBefore && damaged.flushes == flushesBefore,
         "Rejected writable recovery modified image or cleared its journal");

}
void mutationCrashes(Volume::FormatKind kind) {
    Memory clean(16 * 1024 * 1024);
    Volume::Format(clean, u"Mutation recovery", true, kind);
    uint32_t folder, destination, nested, file, empty, unaffected;
    {
        Volume v(clean, true);
        folder=v.MakeDirectory(2,u"Source");destination=v.MakeDirectory(2,u"Destination");
        nested=v.MakeDirectory(folder,u"Nested");empty=v.MakeDirectory(2,u"Empty");
        file=v.Put(folder,u"Both forks",8197,5003,pattern);
        unaffected=v.Put(2,u"Unaffected",777,913,pattern);
        auto before=clean.bytes;auto writes=clean.writes;
        fails([&]{v.Remove(folder);},"Nonempty folder deletion accepted");
        fails([&]{v.Rename(folder,nested,u"Cycle");},"Folder move into descendant accepted");
        must(clean.bytes==before&&clean.writes==writes,"Rejected folder mutation wrote image");
    }
    for(unsigned operation=0;operation<4;++operation)
        for(const char* phase : {"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}) {
            Memory disk=clean;
            {
                Volume v(disk,true);
                v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("Mutation crash");};
                fails([&]{
                    if(operation==0)v.Rename(file,destination,u"Renamed");
                    if(operation==1)v.Rename(folder,destination,u"Moved folder");
                    if(operation==2)v.Remove(file);
                    if(operation==3)v.Remove(empty);
                },"Mutation crash phase not exercised");
                must(!v.Writable(),"Failed mutation remained writable");
            }
            bool committed=strcmp(phase,"data-flushed")&&strcmp(phase,"journal-data-flushed");
            auto writes=disk.writes;
            auto inspect=[&](Volume& v){
                verify(v,unaffected,0,777);verify(v,unaffected,1,913);
                if(operation==2&&committed)fails([&]{v.Stat(file);},"Deleted file survived committed journal");
                else {
                    auto entry=v.Stat(file);verify(v,file,0,8197);verify(v,file,1,5003);
                    must(entry.parent==(operation==0&&committed?destination:folder),"Recovered file parent mismatch");
                    must(entry.name==(operation==0&&committed?u"Renamed":u"Both forks"),"Recovered file name mismatch");
                }
                must(v.Stat(folder).parent==(operation==1&&committed?destination:2),"Recovered directory move mismatch");
                must(v.Stat(nested).parent==folder,"Directory move detached descendants");
                if(operation==3&&committed)fails([&]{v.Stat(empty);},"Deleted folder survived committed journal");
                else must(v.Stat(empty).parent==2,"Empty folder lost before commit");
            };
            {Volume v(disk);inspect(v);}
            must(disk.writes==writes,"Read-only mutation recovery wrote image");
            {Volume v(disk,true);inspect(v);v.Put(2,u"After recovery",1025,513,pattern);inspect(v);}
        }
}
void journalGeometry(Volume::FormatKind kind = Volume::FormatKind::HfsPlus) {
    for (unsigned sector : {512u, 2048u, 4096u}) {
        Memory disk(16 * 1024 * 1024);
        Volume::Format(disk, u"Wrapped journal", true, kind);
        auto vh = disk.bytes.data() + 1024;
        uint64_t info = uint64_t(Disk::U32(vh + 12)) * Disk::U32(vh + 40),
                 offset = Disk::U64(disk.bytes.data() + info + 36),
                 size = Disk::U64(disk.bytes.data() + info + 44);
        auto j = disk.bytes.data() + offset;
        Disk::P32(j + 40, sector);
        Disk::P64(j + 8, size - sector);
        Disk::P64(j + 16, size - sector);
        Disk::P32(j + 36, 0);
        Disk::P32(j + 36, Disk::Sum(j, 44));
        {
            Volume v(disk, true);
            v.fault = [](const char *at) {
                if (!strcmp(at, "journal-committed"))
                    throw Error("Wrapped commit interruption");
            };
            fails([&] { v.MakeDirectory(2, u"Wrapped"); }, "Wrapped transaction crash");
        }
        {
            Volume v(disk);
            bool found = false;
            for (auto &e : v.List())
                found |= e.name == u"Wrapped";
            must(found, "Wrapped journal read-only replay");
        }
        {
            Volume v(disk, true);
            v.MakeDirectory(2, u"After replay");
        }
    }
}
void fragmentCatalog(Memory &disk) {
    auto vh = disk.bytes.data() + 1024;
    unsigned bs = Disk::U32(vh + 40);
    must(bs == 4096, "Fragmented catalog fixture block size");
    uint32_t map = Disk::U32(vh + 128), old = Disk::U32(vh + 288), tree = Disk::U32(vh + 208);
    auto bitmap = disk.bytes.data() + uint64_t(map) * bs;
    Bytes saved(65536);
    memcpy(saved.data(), disk.bytes.data() + uint64_t(old) * bs, saved.size());
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t block = 1000 + i * 2;
        must(!(bitmap[block / 8] & (0x80 >> (block % 8))), "Fixture relocation block is allocated");
        memcpy(disk.bytes.data() + uint64_t(block) * bs, saved.data() + i * bs, bs);
        bitmap[block / 8] |= uint8_t(0x80 >> (block % 8));
        bitmap[(old + i) / 8] &= uint8_t(~(0x80 >> ((old + i) % 8)));
        if (i < 8) {
            Disk::P32(vh + 288 + i * 8, block);
            Disk::P32(vh + 292 + i * 8, 1);
        }
    }
    auto h = disk.bytes.data() + uint64_t(tree) * bs;
    Disk::P16(h + 14, 1);
    Disk::P32(h + 16, 1);
    Disk::P32(h + 20, 1);
    Disk::P32(h + 24, 1);
    Disk::P32(h + 28, 1);
    Disk::P32(h + 40, 14);
    h[248] = 0xc0;
    auto leaf = h + 4096;
    memset(leaf, 0, 4096);
    leaf[8] = 255;
    leaf[9] = 1;
    Disk::P16(leaf + 10, 1);
    Disk::P16(leaf + 14, 10);
    Disk::P32(leaf + 18, 4);
    Disk::P32(leaf + 22, 8);
    for (unsigned i = 0; i < 8; ++i) {
        Disk::P32(leaf + 26 + i * 8, 1016 + i * 2);
        Disk::P32(leaf + 30 + i * 8, 1);
    }
    Disk::P16(leaf + 4094, 14);
    Disk::P16(leaf + 4092, 90);
    memcpy(disk.bytes.data() + disk.Size() - 1024, vh, 512);
}
void fragmentedCatalog(Volume::FormatKind kind = Volume::FormatKind::HfsPlus) {
    Memory disk(16 * 1024 * 1024);
    Volume::Format(disk, u"Fragmented catalog", true, kind);
    fragmentCatalog(disk);
    {
        Volume v(disk, true);
        must(v.List().size() == 2, "Read fragmented catalog with overflow extents");
        v.MakeDirectory(2, u"New folder");
    }
    {
        Volume v(disk);
        must(v.List().size() == 3, "Reopen modified fragmented catalog");
    }
}
void writeApmEntry(Memory& image, unsigned block, unsigned index, unsigned count,
                   uint32_t firstBlock, uint32_t blocks, const char* type) {
    auto entry = image.bytes.data() + uint64_t(index) * block;
    Disk::P16(entry, 0x504d);
    Disk::P32(entry + 4, count);
    Disk::P32(entry + 8, firstBlock);
    Disk::P32(entry + 12, blocks);
    memcpy(entry + 48, type, strlen(type) + 1);
}

void unsafeApm(const Memory& raw) {
    constexpr uint32_t offset = 4096;
    Memory overlapping(raw.bytes.size() + offset);
    memcpy(overlapping.bytes.data() + offset, raw.bytes.data(), raw.bytes.size());
    // The first entry looks valid on its own. The later non-Mac entry overlaps it.
    writeApmEntry(overlapping, 512, 1, 2, offset / 512, uint32_t(raw.Size() / 512), "Apple_HFS");
    writeApmEntry(overlapping, 512, 2, 2, offset / 512 + 32, 1, "Apple_UNIX_SVR2");
    const auto originalOverlap = overlapping.bytes;
    fails([&] { Volume volume(overlapping, true); }, "Writable open accepted overlapping Apple partitions");
    must(overlapping.writes == 0 && overlapping.bytes == originalOverlap,
         "Rejection of overlapping Apple partitions wrote into the image");

    Memory coveringMap(raw.bytes.size() + offset);
    memcpy(coveringMap.bytes.data() + offset, raw.bytes.data(), raw.bytes.size());
    Disk::P16(coveringMap.bytes.data(), 0x4552);
    Disk::P16(coveringMap.bytes.data() + 2, offset);
    // The map entry is in the partition's unused boot area. There is no
    // Apple_partition_map entry to reveal this overlap as an ordinary range.
    writeApmEntry(coveringMap, offset, 1, 1, 1, uint32_t(raw.Size() / offset), "Apple_HFS");
    const auto originalMap = coveringMap.bytes;
    {
        Volume volume(coveringMap);
        must(!volume.Writable() && volume.List().size() == 2,
             "Map-overlapping volume could not be opened read-only");
        fails([&] { volume.MakeDirectory(2, u"Blocked"); }, "Read-only volume accepted a write");
    }
    fails([&] { Volume volume(coveringMap, true); }, "Writable open accepted Apple partition map sectors");
    must(coveringMap.writes == 0 && coveringMap.bytes == originalMap,
         "Map-overlapping partition modified the disk image");
}

void containers() {
    Memory raw(16 * 1024 * 1024);
    Volume::Format(raw, u"Containers", true);
    unsafeApm(raw);
    Memory apm(raw.bytes.size() + 4096);
    memcpy(apm.bytes.data() + 4096, raw.bytes.data(), raw.bytes.size());
    auto p = apm.bytes.data();
    Disk::P16(p, 0x4552);
    Disk::P16(p + 2, 512);
    Disk::P16(p + 512, 0x504d);
    Disk::P32(p + 516, 1);
    Disk::P32(p + 520, 8);
    Disk::P32(p + 524, uint32_t(raw.Size() / 512));
    memcpy(p + 560, "Apple_HFS", 9);
    Bytes outer(apm.bytes.begin(), apm.bytes.begin() + 4096);
    {
        Volume v(apm, true);
        v.MakeDirectory(2, u"Inside APM");
    }
    must(!memcmp(outer.data(), apm.bytes.data(), 4096), "APM was modified");
    {
        Volume v(apm);
        must(v.List().size() == 3, "Reopen APM-contained volume");
    }
    Memory wrapper(raw.bytes.size() + 4096);
    memcpy(wrapper.bytes.data() + 4096, raw.bytes.data(), raw.bytes.size());
    p = wrapper.bytes.data() + 1024;
    Disk::P16(p, 0x4244);
    Disk::P32(p + 20, 4096);
    Disk::P16(p + 28, 8);
    Disk::P16(p + 124, 0x482b);
    Disk::P16(p + 126, 0);
    Disk::P16(p + 128, uint16_t(raw.Size() / 4096));
    {
        Volume v(wrapper, true);
        v.MakeDirectory(2, u"Inside wrapper");
    }
    {
        Volume v(wrapper);
        must(v.List().size() == 3, "Embedded HFS+ wrapper");
    }
    auto h = raw.bytes.data() + 1024;
    Disk::P16(h, 0x4858);
    Disk::P16(h + 2, 6);
    uint32_t catalog = Disk::U32(h + 288);
    raw.bytes[uint64_t(catalog) * 4096 + 51] = 0xbc;
    unsigned writes = raw.writes;
    fails([&] { Volume v(raw, true); }, "Unknown HFSX version accepted for writing");
    fails([&] { Volume v(raw); }, "Unknown HFSX version accepted for reading");
    must(raw.writes == writes, "Unknown HFSX version modified image");
    must(CompareNames(u"Readme", u"README", true) != 0, "HFSX case-sensitive comparison");
    Memory hx(raw.bytes.size());
    Volume::Format(hx, u"HFSX APM", true, Volume::FormatKind::HfsXCaseSensitive);
    memcpy(apm.bytes.data() + 4096, hx.bytes.data(), hx.bytes.size());
    memcpy(apm.bytes.data() + 560, "Apple_HFSX", 11);
    {
        Volume v(apm, true);
        must(v.IsHfsX(), "Apple_HFSX partition not recognized");
        v.MakeDirectory(2, u"Written through APM");
    }
    memcpy(wrapper.bytes.data() + 4096, hx.bytes.data(), hx.bytes.size());
    writes = wrapper.writes;
    fails([&] { Volume v(wrapper, true); }, "HFSX in a legacy HFS wrapper accepted");
    must(wrapper.writes == writes, "Invalid wrapped HFSX modified source");
}
uint8_t *catalogValue(Memory &disk, uint32_t id) {
    auto header = disk.bytes.data() + 1024;
    uint32_t blockSize = Disk::U32(header + 40);
    auto address = [&](uint64_t offset, size_t length) {
        for (unsigned i = 0; i < 8; ++i) {
            uint64_t bytes = uint64_t(Disk::U32(header + 292 + i * 8)) * blockSize;
            if (offset >= bytes) {
                offset -= bytes;
                continue;
            }
            must(length <= bytes - offset, "Fixture node crosses extents");
            return disk.bytes.data() + uint64_t(Disk::U32(header + 288 + i * 8)) * blockSize + offset;
        }
        throw Error("Fixture needs too many inline catalog extents");
    };
    auto tree = address(0, 120);
    unsigned nodeSize = Disk::U16(tree + 32);
    uint32_t leaf = Disk::U32(tree + 24);
    while (leaf) {
        auto node = address(uint64_t(leaf) * nodeSize, nodeSize);
        for (unsigned i = 0; i < Disk::U16(node + 10); ++i) {
            unsigned offset = Disk::U16(node + nodeSize - 2 * (i + 1));
            auto value = node + offset + ((Disk::U16(node + offset) + 3) & ~1u);
            if (Disk::U16(value) <= 2 && Disk::U32(value + 8) == id)
                return value;
        }
        leaf = Disk::U32(node);
    }
    throw Error("Test catalog entry missing");
}
std::u16string longName(unsigned n) {
    std::u16string name = u"Entry ";
    name += char16_t('A' + n / 26);
    name += char16_t('A' + n % 26);
    name += std::u16string(235, u'x');
    return name;
}
void smallJournal(Memory &disk) {
    auto h = disk.bytes.data() + 1024;
    unsigned bs = Disk::U32(h + 40);
    uint64_t jib = uint64_t(Disk::U32(h + 12)) * bs;
    uint64_t offset = Disk::U64(disk.bytes.data() + jib + 36);
    uint32_t journalId = 0;
    {
        Volume v(disk);
        for (auto e : v.List())
            if (!e.directory && !e.forks[0].extents.empty() &&
                uint64_t(e.forks[0].extents[0].start) * bs == offset)
                journalId = e.id;
    }
    must(journalId != 0, "Fixture journal file");
    auto r = catalogValue(disk, journalId);
    uint32_t old = Disk::U32(r + 100), keep = 12288 / bs, start = Disk::U32(r + 104);
    must(keep && 12288 % bs == 0 && Disk::U32(r + 108) == old, "Fixture journal allocation");
    auto bitmap = disk.bytes.data() + uint64_t(Disk::U32(h + 128)) * bs;
    for (uint32_t i = keep; i < old; ++i)
        bitmap[(start + i) / 8] &= uint8_t(~(0x80 >> ((start + i) % 8)));
    Disk::P64(r + 88, 12288);
    Disk::P32(r + 100, keep);
    Disk::P32(r + 108, keep);
    Disk::P32(h + 48, Disk::U32(h + 48) + old - keep);
    memcpy(disk.bytes.data() + disk.Size() - 1024, h, 512);
    Disk::P64(disk.bytes.data() + jib + 44, 12288);
    Disk::View view(disk);
    Disk::Journal journal(view);
    auto j = disk.bytes.data() + offset;
    journal.little = Disk::U32(j + 4) == 0x78563412;
    unsigned headerSize = journal.R32(j + 40);
    journal.W64(j + 8, headerSize);
    journal.W64(j + 16, headerSize);
    journal.W64(j + 24, 12288);
    journal.W32(j + 36, 0);
    journal.W32(j + 36, Disk::Sum(j, 44));
}
Memory resizeFixture(Volume::FormatKind kind) {
    Memory disk(32 * 1024 * 1024);
    Volume::Format(disk, u"Journal enlargement", true, kind);
    {
        Volume v(disk, true);
        for (unsigned i = 0; i < 80; ++i)
            v.MakeDirectory(2, longName(i));
    }
    smallJournal(disk);
    return disk;
}
uint64_t journalBytes(Memory &disk) {
    auto h = disk.bytes.data() + 1024;
    return Disk::U64(disk.bytes.data() + uint64_t(Disk::U32(h + 12)) * Disk::U32(h + 40) + 44);
}
void exerciseResize(Memory &disk, const char *phase = nullptr, bool recover = true) {
    std::vector<uint32_t> files;
    bool fired = false;
    std::u16string attempted;
    bool grew = false;
    {
        Volume v(disk, true);
        if (phase)
            v.fault = [&](const char *at) {
                if (!strcmp(at, phase)) {
                    fired = true;
                    throw Error("Injected journal enlargement interruption");
                }
            };
        auto folders = v.List();
        uint32_t parent = 0;
        for (auto e : folders)
            if (e.directory)
                parent = e.id;
        must(parent != 0, "Resize fixture folder");
        for (unsigned i = 0; i < 80; ++i) {
            attempted = longName(i + 100);
            try {
                files.push_back(v.Put(parent, attempted, 65539, 5007, pattern));
            } catch (const Error &) {
                if (!fired)
                    throw;
                break;
            }
            if (journalBytes(disk) > 12288) {
                grew = true;
                break;
            }
        }
    }
    must(phase ? fired : grew, "Operation never exercised journal enlargement");
    unsigned writes = disk.writes;
    {
        Volume v(disk);
        for (auto id : files) {
            verify(v, id, 0, 65539);
            verify(v, id, 1, 5007);
        }
        if (fired) {
            for (auto e : v.List()) {
                if (!e.directory)
                    continue;
                for (auto f : v.List(e.id))
                    must(f.name != attempted, "Uncommitted user file appeared during journal relocation");
            }
        }
    }
    must(disk.writes == writes, "Read-only resize recovery wrote source");
    if (recover) {
        Volume v(disk, true);
        for (auto id : files)
            verify(v, id, 0, 65539);
        v.MakeDirectory(2, u"Recovered");
    }
}
void journalResizeTests() {
    for (auto kind : {Volume::FormatKind::HfsPlus, Volume::FormatKind::HfsXCaseSensitive,
                      Volume::FormatKind::HfsXCaseInsensitive}) {
        auto disk = resizeFixture(kind);
        exerciseResize(disk);
    }
    auto original = resizeFixture(Volume::FormatKind::HfsXCaseSensitive);
    for (auto phase :
         {"journal-resize-data-flushed", "journal-resize-new-log-committed", "journal-resize-jib-written",
          "journal-resize-jib-committed", "journal-resize-home-flushed", "journal-resize-cleared"}) {
        Memory disk = original;
        exerciseResize(disk, phase);
    }
    puts("PASS: automatic journal enlargement, preserved pending file data, both journal-location recovery "
         "paths, read-only recovery and every relocation crash boundary");
}
void resizeIoErrors() {
    auto original = resizeFixture(Volume::FormatKind::HfsXCaseSensitive);
    unsigned failures = 0;
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned index = 0; index < (mode ? 16u : 80u); ++index) {
            Memory disk = original;
            disk.writes = disk.flushes = 0;
            if (mode)
                disk.failFlush = int(index);
            else
                disk.failWrite = int(index);
            try {
                exerciseResize(disk);
            } catch (const Error &) {
                ++failures;
            }
            disk.failWrite = disk.failFlush = -1;
            {
                Volume v(disk);
                for (auto folder : v.List()) {
                    if (!folder.directory)
                        continue;
                    for (auto file : v.List(folder.id))
                        if (!file.directory) {
                            verify(v, file.id, 0, 65539);
                            verify(v, file.id, 1, 5007);
                        }
                }
            }
            {
                Volume v(disk, true);
                v.MakeDirectory(2, u"IO error recovery");
            }
        }
    must(failures > 50, "I/O fault injection did not cover relocation");
    puts("PASS: write/flush failures throughout journal growth leave a recoverable filesystem and complete "
         "file forks");
}
Memory largeCatalog() {
    Memory disk(256 * 1024 * 1024);
    Volume::Format(disk, u"Beyond header map");
    auto h = disk.bytes.data() + 1024;
    unsigned bs = Disk::U32(h + 40), old = Disk::U32(h + 288), oldCount = Disk::U32(h + 292);
    Bytes saved(65536);
    memcpy(saved.data(), disk.bytes.data() + uint64_t(old) * bs, saved.size());
    const uint32_t first = 3000, nodes = 40000, map = 30720;
    auto bitmap = disk.bytes.data() + uint64_t(Disk::U32(h + 128)) * bs;
    for (uint32_t i = 0; i < oldCount; ++i)
        bitmap[(old + i) / 8] &= uint8_t(~(0x80 >> ((old + i) % 8)));
    for (uint32_t i = 0; i < nodes; ++i) {
        must(!(bitmap[(first + i) / 8] & (0x80 >> ((first + i) % 8))),
             "Large catalog fixture overlaps allocation");
        bitmap[(first + i) / 8] |= uint8_t(0x80 >> ((first + i) % 8));
    }
    memcpy(disk.bytes.data() + uint64_t(first) * bs, saved.data(), saved.size());
    auto tree = disk.bytes.data() + uint64_t(first) * bs;
    Disk::P32(tree, map);
    Disk::P32(tree + 36, nodes);
    Disk::P32(tree + 40, nodes - 3);
    auto extra = tree + uint64_t(map) * 4096;
    extra[8] = 2;
    Disk::P16(extra + 10, 1);
    Disk::P16(extra + 4094, 14);
    Disk::P16(extra + 4092, 4090);
    extra[14] = 0x80;
    Disk::P64(h + 272, uint64_t(nodes) * bs);
    Disk::P32(h + 284, nodes);
    Disk::P32(h + 288, first);
    Disk::P32(h + 292, nodes);
    Disk::P32(h + 48, Disk::U32(h + 48) - (nodes - oldCount));
    memcpy(disk.bytes.data() + disk.Size() - 1024, h, 512);
    {
        Volume v(disk, true);
        auto before = disk.writes;
        v.MakeDirectory(2, u"Incremental edit");
        must(disk.writes - before < 128, "Small change rewrote large catalog");
    }
    must(Disk::U64(h + 272) == uint64_t(nodes) * bs && Disk::U32(tree + 36) == nodes,
         "Existing catalog was replaced or shrunk");
    {
        Volume v(disk);
        must(v.List().size() == 3, "Large map-chain catalog reopened");
    }
    return disk;
}
void hfsx(bool journaled, bool sensitive) {
    Memory disk(32 * 1024 * 1024);
    auto kind = sensitive ? Volume::FormatKind::HfsXCaseSensitive : Volume::FormatKind::HfsXCaseInsensitive;
    Volume::Format(disk, u"HFSX", journaled, kind);
    must(Disk::U16(disk.bytes.data() + 1024) == 0x4858 && Disk::U16(disk.bytes.data() + 1026) == 5,
         "HFSX v5 headers");
    uint32_t a, b = 0, folder;
    {
        Volume v(disk, true);
        must(v.IsHfsX() && v.CaseSensitive() == sensitive && v.Journaled() == journaled, "HFSX mode");
        a = v.Put(2, u"Readme", 123, 97, pattern);
        if (sensitive) {
            b = v.Put(2, u"README", 43, 71, pattern);
            v.Put(2, u"Readme\u200c", 7, 0, pattern);
            v.Put(2, u"\u200c", 5, 0, pattern);
        } else {
            fails([&] { v.Put(2, u"README", 1, 0, pattern); }, "Case-insensitive HFSX duplicate accepted");
            fails([&] { v.Put(2, u"Readme\u200c", 1, 0, pattern); },
                  "Case-insensitive ignorable duplicate accepted");
        }
        v.Put(2, u"\u00e9", 1, 0, pattern);
        fails([&] { v.Put(2, u"e\u0301", 1, 0, pattern); }, "Canonical equivalent HFSX duplicate accepted");
        v.Rename(a, 2, u"readme");
        {
            Volume reopened(disk);
            must(reopened.Stat(a).name == u"readme", "Case-only rename was not written to disk");
        }
        if (sensitive)
            fails([&] { v.Rename(a, 2, u"README"); }, "HFSX rename collision accepted");
        v.Put(2, u"readme", 91, 17, pattern, nullptr, true);
        verify(v, a, 0, 91);
        if (sensitive)
            verify(v, b, 0, 43);
        folder = v.MakeDirectory(2, u"Folder");
        auto sub = v.MakeDirectory(folder, u"Sub");
        must(Disk::U32(catalogValue(disk, 2) + 84) == 1 && Disk::U32(catalogValue(disk, folder) + 84) == 1,
             "HFSX folder counters after creation");
        v.Rename(sub, 2, u"Sub");
        must(Disk::U32(catalogValue(disk, 2) + 84) == 2 && Disk::U32(catalogValue(disk, folder) + 84) == 0,
             "HFSX folder counters after move");
        v.Remove(sub);
        // Force catalog index/leaf rebuilding while preserving its comparison mode.
        for (unsigned i = 0; i < 80; ++i) {
            std::u16string name = u"Entry ";
            name += char16_t('A' + i / 26);
            name += char16_t('A' + i % 26);
            name += std::u16string(70, u'x');
            v.Put(folder, name, 13, 7, pattern);
        }
    }
    {
        Volume v(disk, true);
        must(v.CaseSensitive() == sensitive, "HFSX mode survived catalog rebuild");
        verify(v, a, 1, 17);
        if (sensitive)
            verify(v, b, 1, 71);
        must(v.List(folder).size() == 80, "HFSX indexed catalog reopened");
        v.Remove(a);
    }
    must(Disk::U16(disk.bytes.data() + 1024) == 0x4858 && Disk::U16(disk.bytes.data() + 1026) == 5,
         "HFSX changed into HFS+");
    {
        Volume v(disk);
        must(v.IsHfsX() && !v.Writable(), "HFSX read-only open");
    }
    auto flags = Disk::U16(catalogValue(disk, folder) + 2);
    Disk::P16(catalogValue(disk, folder) + 2, flags | 0x40);
    unsigned writes = disk.writes;
    {
        Volume v(disk, true);
        v.Rename(folder, 2, u"Linked subtree");must(v.Stat(folder).name==u"Linked subtree","Child-link hint prevented ordinary folder rename");
    }
    must(disk.writes > writes, "Ordinary folder rename did not commit");
}
void permissions() {
    Memory disk(16 * 1024 * 1024);
    Volume::Format(disk, u"Flags", true);
    uint32_t id;
    {
        Volume v(disk, true);
        id = v.Put(2, u"File", 100, 17, pattern);
    }
    // Group IDs are not BSD owner flags. GID 2 must not lock an ordinary file.
    Disk::P32(catalogValue(disk, id) + 36, 2);
    {
        Volume v(disk, true);
        v.Put(2, u"File", 50, 0, pattern, nullptr, true);
    }
    catalogValue(disk, id)[41] = 0x20;
    {
        Volume v(disk, true);
        uint8_t b;
        fails([&] { v.Read(id, 0, 0, &b, 1); }, "Compressed file silently exported");
        fails([&] { v.Put(2, u"File", 50, 0, pattern, nullptr, true); }, "Compressed file overwritten");
    }
    catalogValue(disk, id)[41] = 2;
    {
        Volume v(disk, true);
        fails([&] { v.Remove(id); }, "Immutable file removed");
    }
    catalogValue(disk, id)[41] = 0;
    catalogValue(disk, 2)[40] = 2;
    {
        Volume v(disk, true);
        fails([&] { v.Remove(id); }, "File removed from immutable parent");
    }
}
void trees() {
    Memory disk(64 * 1024 * 1024);
    Volume::Format(disk, u"Catalog", true);
    {
        Volume v(disk, true);
        for (unsigned i = 0; i < 160; ++i) {
            std::u16string n = u"Long filename ";
            n += char16_t('A' + i / 26);
            n += char16_t('A' + i % 26);
            n += std::u16string(80, u'x');
            v.Put(2, n, 29, 17, pattern);
        }
        must(v.List().size() == 162, "Multi-leaf catalog count");
    }
    {
        Volume v(disk);
        for (auto &e : v.List())
            if (!e.protectedFile)
                verify(v, e.id, 1, 17);
    }
}
void fragmented(Volume::FormatKind kind = Volume::FormatKind::HfsPlus, const wchar_t *output = nullptr) {
    Memory disk(16 * 1024 * 1024);
    Volume::Format(disk, u"Fragmented", true, kind);
    uint32_t result;
    {
        Volume v(disk, true);
        std::vector<uint32_t> ids;
        for (unsigned i = 0; i < 80; ++i) {
            std::u16string name = u"Block ";
            name += char16_t('A' + i / 26);
            name += char16_t('A' + i % 26);
            ids.push_back(v.Put(2, name, 4096, 0, pattern));
        }
        for (size_t i = 0; i < ids.size(); i += 2)
            v.Remove(ids[i]);
        result = v.Put(2, u"Overflow forks", 31 * 4096 + 7, 19 * 4096 + 9, pattern);
        must(v.Stat(result).forks[0].extents.size() > 8, "Fragmentation did not create overflow extents");
        must(v.Stat(result).forks[1].extents.size() > 8, "Resource fork did not use overflow extents");
    }
    {
        Volume v(disk, true);
        verify(v, result, 0, 31 * 4096 + 7);
        verify(v, result, 1, 19 * 4096 + 9);
        if (output) {
            auto image = CreateImage(output, disk.Size());
            image->Write(0, disk.bytes.data(), disk.bytes.size());
            image->Flush();
        }
        v.Remove(result);
    }
}
void failures() {
    Memory disk(16 * 1024 * 1024);
    Volume::Format(disk, u"Failures", true);
    uint32_t existing;
    {
        Volume v(disk, true);
        existing = v.Put(2, u"Retained", 8000, 32, pattern);
        fails(
            [&] {
                v.Put(
                    2, u"Retained", 10000, 1000,
                    [](unsigned, uint64_t, void *, size_t) { throw Error("Injected source read error"); },
                    nullptr, true);
            },
            "Source read error ignored");
    }
    {
        Volume v(disk);
        verify(v, existing, 0, 8000);
        verify(v, existing, 1, 32);
    }
    // Allocation failures cannot replace the existing catalog entry.
    {
        Volume v(disk, true);
        fails([&] { v.Put(2, u"Retained", 32 * 1024 * 1024, 0, pattern, nullptr, true); },
              "Out-of-space write accepted");
    }
    {
        Volume v(disk);
        verify(v, existing, 0, 8000);
    }
    auto original = disk.bytes;
    Disk::P32(disk.bytes.data() + 1024 + 40, 0xffff);
    fails([&] { Volume v(disk, true); }, "Invalid block geometry accepted");
    disk.bytes = original;
    // Fork/bitmap mismatch is rejected before any write.
    uint32_t map = Disk::U32(disk.bytes.data() + 1024 + 128);
    disk.bytes[size_t(map) * 4096] = 0;
    unsigned writes = disk.writes;
    fails([&] { Volume v(disk, true); }, "Free metadata block accepted");
    must(writes == disk.writes, "Corrupt image mount wrote source");
}
void large(const wchar_t *path, Volume::FormatKind kind = Volume::FormatKind::HfsPlus) {
    uint8_t low[32], high[32];
    pattern(0, 0, low, sizeof(low));
    pattern(0, 0x80000000ULL, high, sizeof(high));
    must(memcmp(low, high, sizeof(low)) != 0, "Large-fork pattern must detect a 2 GiB offset alias");
    auto disk = CreateImage(path, 6ULL * 1024 * 1024 * 1024);
    Volume::Format(*disk, u"Large HFS forks", true, kind);
    uint32_t id;
    const uint64_t length = 0x80010003ULL;
    {
        Volume v(*disk, true);
        id = v.Put(2, u"Large both forks", length, length + 19,
                   [&](unsigned f, uint64_t off, void *p, size_t n) {
                       pattern(f, off, p, n);
                       if ((off % (512ULL * 1024 * 1024)) == 0) {
                           printf("Writing fork %u: %llu MiB\n", f, off / (1024 * 1024));
                           fflush(stdout);
                       }
                   });
    }
    {
        Volume v(*disk);
        Bytes actual(65539), expected(actual.size());
        for (unsigned f = 0; f < 2; ++f) {
            must(v.Stat(id).forks[f].size == length + f * 19, "Persisted 64-bit fork size");
            for (uint64_t off : {0ULL, 0x7ffff000ULL, 0x80000000ULL, length - 10}) {
                size_t n = size_t((std::min)(uint64_t(actual.size()), length + f * 19 - off));
                must(v.Read(id, f, off, actual.data(), n) == n, "Large-fork read count");
                pattern(f, off, expected.data(), n);
                must(!memcmp(actual.data(), expected.data(), n), "Large fork boundary content");
            }
        }
    }
    puts(kind == Volume::FormatKind::HfsPlus
             ? "PASS: native image writes and persisted reads beyond 2 GiB in both HFS+ forks"
             : "PASS: native image writes and persisted reads beyond 2 GiB in both HFSX forks");
}

uint32_t named(Volume& v,uint32_t parent,const std::u16string& name){for(auto& e:v.List(parent))if(e.name==name)return e.id;throw Error("Missing named hard-link fixture");}
std::string linkState(Volume& v){
 std::string out;std::set<uint32_t> active;std::function<void(uint32_t)> walk=[&](uint32_t parent){for(auto& e:v.List(parent)){if(e.protectedFile&&!e.hardLink)continue;must(!e.hardLink||!e.protectedFile,"Recovered hard-link chain is not writable");must(e.readError.empty(),"Recovered link is unreadable");out+="[";for(auto c:e.name)out+=std::to_string(unsigned(c))+",";out+=":"+std::to_string(e.id)+":"+std::to_string(e.resolvedId)+":"+std::to_string(e.linkCount);
  for(auto& a:v.GetAttributes(e.id)){for(auto c:a.first)out+=std::to_string(unsigned(c))+",";for(auto c:a.second)out+=std::to_string(c)+",";}
  if(e.directory){must(active.insert(e.resolvedId).second,"Recovered directory cycle");walk(e.id);active.erase(e.resolvedId);}else for(unsigned f=0;f<2;++f){Bytes bytes(size_t(e.forks[f].size));must(v.Read(e.id,f,0,bytes.data(),bytes.size())==bytes.size(),"Recovered fork read");uint64_t hash=14695981039346656037ULL;for(auto b:bytes){hash^=b;hash*=1099511628211ULL;}out+=":"+std::to_string(bytes.size())+":"+std::to_string(hash);}out+="]";}};walk(2);return out;
}
struct TornLinkMemory:Memory{int torn=-1;explicit TornLinkMemory(const Memory& value):Memory(value){}void Write(uint64_t at,const void* p,size_t n)override{if(torn>=0&&writes==unsigned(torn)){Memory::Write(at,p,n/2);throw Error("Torn hard-link metadata write");}Memory::Write(at,p,n);}};
void hardLinkFailures(){
 for(unsigned action=0;action<10;++action){Memory baseline(16*1024*1024);Volume::Format(baseline,u"Link failures",true);bool directory=action==1||action==4||action==5||action==9;
  {Volume v(baseline,true);auto a=directory?v.MakeDirectory(2,u"A"):v.Put(2,u"A",5003,723,pattern);if(directory&&action!=5)v.Put(a,u"Child",101,17,pattern);v.SetAttributes(a,{{u"user.keep",Bytes(5001,7)}});if(action!=0&&action!=1)v.MakeHardLink(a,2,u"B");if(action==3||action==5)v.Remove(named(v,2,u"A"));}
  auto operation=[&](Volume& v){if(action==0||action==1)v.MakeHardLink(named(v,2,u"A"),2,u"B");else if(action>=2&&action<=5)v.Remove(named(v,2,(action==3||action==5)?u"B":u"A"));else if(action==6)v.Put(2,u"B",71,91,pattern,nullptr,true);else if(action==7)v.Rename(named(v,2,u"B"),2,u"Renamed");else if(action==8)v.SetAttributes(named(v,2,u"B"),{{u"user.new",Bytes(7003,9)}});else v.MakeDirectory(named(v,2,u"B"),u"Added");};
  std::string oldState,newState;{Volume v(baseline);oldState=linkState(v);}Memory success=baseline;auto startWrites=success.writes,startFlushes=success.flushes;{Volume v(success,true);operation(v);newState=linkState(v);}auto writes=success.writes-startWrites,flushes=success.flushes-startFlushes;must(oldState!=newState&&writes&&flushes,"Hard-link failure test did not exercise a change");
  auto check=[&](Memory& failed){failed.failWrite=failed.failFlush=-1;auto before=failed.writes;Volume v(failed,false);auto state=linkState(v);must(state==oldState||state==newState,"Hard-link failure recovered a mixed topology/content state");must(before==failed.writes,"Read-only link recovery wrote image");};
  for(const char* phase:{"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}){Memory failed=baseline;{Volume v(failed,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("Link phase error");};fails([&]{operation(v);},"Hard-link phase fault missed");}check(failed);}
  for(unsigned mode=0;mode<3;++mode){auto count=mode==1?flushes:writes;for(auto step:std::set<unsigned>{0,1,count/2,count-1}){if(mode==2){TornLinkMemory failed(baseline);failed.torn=int(failed.writes+step);{Volume v(failed,true);fails([&]{operation(v);},"Torn hard-link write fault missed");}failed.torn=-1;check(failed);}else{Memory failed=baseline;if(mode==1)failed.failFlush=int(failed.flushes+step);else failed.failWrite=int(failed.writes+step);{Volume v(failed,true);fails([&]{operation(v);},"Hard-link I/O fault missed");}check(failed);}}}
 }
 Memory valid(16*1024*1024);Volume::Format(valid,u"Corrupt links",true);uint32_t inode,a,b;{Volume v(valid,true);auto file=v.Put(2,u"A",101,17,pattern);b=v.MakeHardLink(file,2,u"B");a=named(v,2,u"A");inode=v.Stat(a).resolvedId;}
 for(unsigned corruption=0;corruption<3;++corruption){Memory bad=valid;auto raw=catalogValue(bad,corruption==0?inode:a);if(corruption==0)Disk::P32(raw+44,3);else if(corruption==1)Disk::P32(raw+36,a);else Disk::P32(raw+32,b);auto before=bad.writes;{Volume v(bad,true);must(v.Stat(a).protectedFile,"Broken chain exposed as writable");fails([&]{v.Remove(a);},"Broken chain was unlinked");fails([&]{v.MakeHardLink(a,2,u"C");},"Broken chain was extended");verify(v,b,0,101);}must(bad.writes==before,"Rejected corrupt link operation wrote image");}
 {Memory bad(16*1024*1024);Volume::Format(bad,u"Malformed directory ancestor",true);uint32_t sub,child,inode,nestedLink,outside;
  {Volume v(bad,true);auto folder=v.MakeDirectory(2,u"Folder");sub=v.MakeDirectory(folder,u"Sub");child=v.Put(sub,u"Child",101,17,pattern);auto file=v.Put(2,u"Outside",101,17,pattern);nestedLink=v.MakeHardLink(file,sub,u"Nested link");outside=named(v,2,u"Outside");auto alias=v.MakeHardLink(folder,2,u"Alias");inode=v.Stat(alias).resolvedId;}
  Disk::P32(catalogValue(bad,inode)+44,3);auto before=bad.writes;{Volume v(bad,true);must(v.Stat(child).protectedFile,"Child of a corrupt directory link is writable");fails([&]{v.MakeDirectory(sub,u"Unsafe");},"Corrupt linked ancestor accepted child create");fails([&]{v.Put(sub,u"Child",71,91,pattern,nullptr,true);},"Corrupt linked ancestor accepted child overwrite");fails([&]{v.SetAttributes(child,{});},"Corrupt linked ancestor accepted attribute edit");fails([&]{v.Remove(child);},"Corrupt linked ancestor accepted child delete");fails([&]{v.SetAttributes(nestedLink,{});},"Corrupt visible linked ancestor accepted shared attribute edit");must(!v.Stat(outside).protectedFile,"Corrupt alias ancestor protected an outside link");verify(v,child,0,101);}must(bad.writes==before,"Rejected corrupt-ancestor edits wrote image");}
 puts("PASS: link conversion, shared overwrite/attributes, rename, file/directory unlink and child creation recover a complete old/new state after every journal boundary, write/flush faults and torn writes; malformed counts/chains rejected before writes");
}
void hardLinks(){
 for(auto kind:{Volume::FormatKind::HfsPlus,Volume::FormatKind::HfsXCaseSensitive,Volume::FormatKind::HfsXCaseInsensitive})for(bool journal:{false,true}){
  Memory disk(24*1024*1024);Volume::Format(disk,u"Links",journal,kind);uint32_t a,b,c,folder,alias,child,inode;
  {Volume v(disk,true);auto file=v.Put(2,u"One",5003,723,pattern);Attributes attrs{{u"user.shared",Bytes(6001,9)}};v.SetAttributes(file,attrs);auto free=v.FreeBytes();b=v.MakeHardLink(file,2,u"Two");a=named(v,2,u"One");inode=v.Stat(a).resolvedId;
   must(Disk::U32(catalogValue(disk,inode)+4)==a&&Disk::U32(catalogValue(disk,a)+36)==b&&Disk::U32(catalogValue(disk,b)+32)==a&&Disk::U32(catalogValue(disk,inode)+44)==2,"Independent raw link-chain fields");
   must(a!=inode&&v.Stat(a).hardLink&&v.Stat(a).linkCount==2&&v.Stat(b).resolvedId==inode,"Created shared file links");must(v.GetAttributes(b)==attrs,"Linked attributes");verify(v,b,0,5003);verify(v,a,1,723);
   c=v.MakeHardLink(a,2,u"Three");must(v.Stat(c).linkCount==3,"Third hard link");v.Rename(a,2,u"Renamed");must(v.Stat(a).name==u"Renamed"&&v.Stat(a).resolvedId==inode,"Rename changed shared inode");
   v.Put(2,u"Two",71,91,pattern,nullptr,true);verify(v,a,0,71);verify(v,c,1,91);must(v.GetAttributes(a)==attrs,"Shared overwrite dropped attributes");v.SetAttributes(c,{{u"user.other",{1,2,3}}});must(v.GetAttributes(a)==v.GetAttributes(c),"Linked attributes not shared");
   v.SetCompression(b,true);must(v.Stat(a).compressed,"Shared compression creation");v.SetCompression(a,false);verify(v,c,0,71);
   v.Remove(b);must(v.Stat(a).linkCount==2,"Unlink count");verify(v,c,1,91);v.Remove(a);must(v.Stat(c).linkCount==1,"Last visible link count");v.Remove(c);must(v.FreeBytes()>=free,"Last unlink did not reclaim file/attribute blocks");fails([&]{v.Stat(inode);},"Last unlink retained backing inode");
   folder=v.MakeDirectory(2,u"Folder");child=v.Put(folder,u"Child",101,17,pattern);alias=v.MakeHardLink(folder,2,u"Alias");folder=named(v,2,u"Folder");must(v.Stat(alias).directory&&v.Stat(alias).linkCount==2,"Directory link presentation");
   {auto raw=catalogValue(disk,alias);auto fork=disk.bytes.data()+uint64_t(Disk::U32(raw+184))*Disk::U32(disk.bytes.data()+1064);auto map=Disk::U32(fork+4);auto data=Disk::U32(fork);must(Disk::U64(raw+168)==464&&data==256&&Disk::U32(fork+12)==50&&!memcmp(fork+map+30,"alis",4),"Directory alias Resource Manager layout");must(Disk::U16(fork+data+4+6)==2&&Disk::U32(fork+data+4+114)==v.Stat(alias).resolvedId,"Directory alias target CNID/version");}
must(v.List(alias).size()==1&&v.List(alias)[0].id==child,"Directory link contents");auto added=v.MakeDirectory(alias,u"New folder");must(v.List(folder).size()==2,"Directory link child creation");
   fails([&]{v.MakeHardLink(folder,alias,u"Cycle");},"Self directory hard link accepted");fails([&]{v.Rename(folder,added,u"Cycle");},"Directory alias move cycle accepted");
   auto extra=v.MakeHardLink(alias,2,u"Third folder");v.Remove(folder);must(v.Stat(alias).linkCount==2&&v.List(extra).size()==2,"Non-last directory unlink deleted children");v.Rename(extra,2,u"Moved alias");v.Remove(extra);fails([&]{v.Remove(alias);},"Last nonempty directory link removed");v.Remove(child);v.Remove(added);v.Remove(alias);
  }
  {Volume v(disk);for(auto& e:v.List())must(!e.hardLink,"Removed links survived reopen");}
 }
 // Crash every durable boundary of conversion, insertion, shared overwrite and
 // unlink. Reopen from disk, independently check pointer fields and content.
 for(unsigned action=0;action<4;++action){Memory baseline(24*1024*1024);Volume::Format(baseline,u"Link crash",true);uint32_t original;
  {Volume v(baseline,true);original=v.Put(2,u"A",5003,723,pattern);if(action)v.MakeHardLink(original,2,u"B");}
  for(const char* phase:{"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}){Memory failed=baseline;{Volume v(failed,true);v.fault=[&](const char* p){if(!strcmp(p,phase))throw Error("Link crash");};fails([&]{if(action==0)v.MakeHardLink(original,2,u"B");else if(action==1)v.MakeHardLink(named(v,2,u"A"),2,u"C");else if(action==2)v.Put(2,u"B",101,17,pattern,nullptr,true);else v.Remove(named(v,2,u"A"));},"Missing hard-link crash phase");}
   Volume recovered(failed,false);auto listing=recovered.List();listing.erase(std::remove_if(listing.begin(),listing.end(),[](const Entry& e){return e.protectedFile&&!e.hardLink;}),listing.end());must(!listing.empty(),"Crash lost all links");uint32_t inode=0;unsigned links=0;for(auto& e:listing){must(e.readError.empty(),"Crash left broken link");if(e.hardLink){if(!inode)inode=e.resolvedId;must(e.resolvedId==inode,"Crash split shared inode");++links;}must(e.forks[0].size==5003||(action==2&&e.forks[0].size==101),"Crash fork size");verify(recovered,e.id,0,e.forks[0].size);verify(recovered,e.id,1,e.forks[1].size);}
   for(auto& e:listing)if(e.hardLink)must(e.linkCount==links,"Crash link count");
  }
 }
 puts("PASS: HFS+/both HFSX file and directory link creation, chained inode accounting, rename, shared forks/attributes/compression, child writes, cycle refusal, non-last unlink, final reclamation and journal crash recovery; journaled and non-journaled images");
}
void directoryMetadata(){
 for(auto kind:{Volume::FormatKind::HfsPlus,Volume::FormatKind::HfsXCaseSensitive,Volume::FormatKind::HfsXCaseInsensitive}){
  Memory baseline(16*1024*1024);Volume::Format(baseline,u"Folder metadata",true,kind,true);uint32_t folder,file;
  std::array<uint8_t,32> oldFinder{},newFinder{};for(unsigned i=0;i<32;++i){oldFinder[i]=uint8_t(i+1);newFinder[i]=uint8_t(100+i);}oldFinder[8]=newFinder[8]=0;oldFinder[9]=newFinder[9]=0;
  std::array<uint64_t,2> oldDates{3000000000,3000000123},newDates{2082844800ULL,6377812095ULL};
  Attributes oldAttrs{{u"user.keep",Bytes(8003,0x31)}},newAttrs{{u"user.keep",Bytes(9007,0x72)},{u"user.empty",{}}};
  {Volume v(baseline,true);folder=v.MakeDirectory(2,u"Folder");file=v.Put(folder,u"Child",5003,723,pattern);v.SetDirectoryMetadata(folder,&oldFinder,&oldDates,&oldAttrs);}
  auto check=[&](Memory& disk){auto writes=disk.writes;Volume v(disk,false);auto e=v.Stat(folder);auto a=v.GetAttributes(folder);bool old=e.finder==oldFinder&&e.created==oldDates[0]&&e.modified==oldDates[1]&&a==oldAttrs;bool fresh=e.finder==newFinder&&e.created==newDates[0]&&e.modified==newDates[1]&&a==newAttrs;must(old||fresh,"Folder metadata recovered a mixed transaction");verify(v,file,0,5003);verify(v,file,1,723);must(disk.writes==writes,"Read-only folder metadata replay wrote source");};
  for(const char* phase:{"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}){Memory failed=baseline;{Volume v(failed,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("Folder metadata crash");};fails([&]{v.SetDirectoryMetadata(folder,&newFinder,&newDates,&newAttrs);},"Missing folder metadata crash phase");}check(failed);}
  for(bool flush:{false,true}){Memory success=baseline;auto start=flush?success.flushes:success.writes;{Volume v(success,true);v.SetDirectoryMetadata(folder,&newFinder,&newDates,&newAttrs);}unsigned count=(flush?success.flushes:success.writes)-start;must(count>1,"Folder metadata fault coverage");std::set<unsigned> steps{0,1,count/2,count-1};for(auto step:steps){Memory failed=baseline;if(flush)failed.failFlush=int(failed.flushes+step);else failed.failWrite=int(failed.writes+step);{Volume v(failed,true);fails([&]{v.SetDirectoryMetadata(folder,&newFinder,&newDates,&newAttrs);},"Folder metadata I/O fault ignored");}failed.failFlush=failed.failWrite=-1;check(failed);}}
  {Volume v(baseline,true);v.SetDirectoryMetadata(folder,nullptr,&newDates,nullptr);auto e=v.Stat(folder);must(e.finder==oldFinder&&v.GetAttributes(folder)==oldAttrs&&e.modified==newDates[1],"Date-only merge finalization replaced Finder info or attributes");fails([&]{v.SetDirectoryMetadata(file,&newFinder,&oldDates,nullptr);},"File accepted as folder");fails([&]{v.SetDirectoryMetadata(2,&newFinder,&oldDates,nullptr);},"Volume root metadata accepted");}
  baseline.writable=false;{Volume v(baseline);fails([&]{v.SetDirectoryMetadata(folder,&newFinder,&newDates,&newAttrs);},"Read-only folder metadata edit accepted");}
 }
 puts("PASS: folder Finder/date/attribute transactions, 64-bit dates, date-only merge finalization, child fork preservation, read-only refusal and journal/write/flush recovery in HFS+ and both HFSX modes");
}
void RunLargeAttributeTests();
int wmain(int argc, wchar_t **argv) {
    try {
        if(argc==2&&!wcscmp(argv[1],L"--large-attributes")){RunLargeAttributeTests();return 0;}
        if(argc==2&&!wcscmp(argv[1],L"--hard-links")){hardLinks();hardLinkFailures();return 0;}
          if(argc==3&&!wcscmp(argv[1],L"--other-features"))return RunHfsPlusOtherFeaturesTests(argv[2]);
        auto saveMemory = [](Memory &memory, const wchar_t *path) {
            auto out = CreateImage(path, memory.Size());
            out->Write(0, memory.bytes.data(), memory.bytes.size());
            out->Flush();
        };
        if(argc==3&&!wcscmp(argv[1],L"--compression-encoding-fixture")){CreateHfsPlusCompressionEncodingFixture(argv[2]);return 0;}
        if (argc == 3 && !wcscmp(argv[1], L"--feature-fixture")) {
            CreateHfsPlusFeatureFixture(argv[2]);
            return 0;
        }
        if (argc == 5 && !wcscmp(argv[1], L"--export-hfsplus")) {
            auto source = OpenImage(argv[2], false);
            Volume volume(*source);
            std::wstring path = argv[3];
            uint32_t id = 2;
            size_t start = 0;
            while (start < path.size()) {
                size_t end = path.find(L'/', start);
                if (end == std::wstring::npos) end = path.size();
                std::u16string name(path.begin() + start, path.begin() + end);
                uint32_t found = 0;
                for (const auto &entry : volume.List(id))
                    if (entry.name == name) found = entry.id;
                must(found != 0, "Export fixture path not found");
                id = found; start = end + 1;
            }
            auto entry = volume.Stat(id);
            volume.Read(id, 0, 0, nullptr, 0);
            FILE *file = nullptr;
            must(_wfopen_s(&file, argv[4], L"wb") == 0 && file, "Cannot create fixture export");
            try {
                Bytes buffer(65536);
                for (uint64_t offset = 0; offset < entry.forks[0].size;) {
                    size_t n = size_t((std::min)(uint64_t(buffer.size()), entry.forks[0].size - offset));
                    must(volume.Read(id, 0, offset, buffer.data(), n) == n, "Short fixture export read");
                    must(fwrite(buffer.data(), 1, n, file) == n, "Fixture export write failed");
                    offset += n;
                }
                fclose(file);
            } catch (...) { fclose(file); throw; }
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--incremental-large-catalog")) {
            auto memory = largeCatalog();
            saveMemory(memory, argv[2]);
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--incremental-resize")) {
            auto memory = resizeFixture(Volume::FormatKind::HfsXCaseSensitive);
            exerciseResize(memory);
            saveMemory(memory, argv[2]);
            return 0;
        }
        if (argc == 4 && !wcscmp(argv[1], L"--incremental-resize-crash")) {
            std::string phase;
            for (auto c : std::wstring(argv[2]))
                phase += char(c);
            auto memory = resizeFixture(Volume::FormatKind::HfsXCaseSensitive);
            exerciseResize(memory, phase.c_str(), false);
            saveMemory(memory, argv[3]);
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--incremental-reference")) {
            auto source = OpenImage(argv[2], true);
            Memory memory(size_t(source->Size()));
            source->Read(0, memory.bytes.data(), memory.bytes.size());
            {
                Volume v(memory, true);
                for (unsigned i = 0; i < 80; ++i)
                    v.MakeDirectory(2, longName(i));
            }
            smallJournal(memory);
            exerciseResize(memory);
            source->Write(0, memory.bytes.data(), memory.bytes.size());
            source->Flush();
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--fragmented-forks")) {
            fragmented(Volume::FormatKind::HfsXCaseSensitive, argv[2]);
            return 0;
        }
        if (argc == 2 && !wcscmp(argv[1], L"--self-test"))
            argc = 1;
        if (argc == 3 && !wcscmp(argv[1], L"--large")) {
            large(argv[2]);
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--large-hfsx")) {
            large(argv[2], Volume::FormatKind::HfsXCaseSensitive);
            return 0;
        }
        if (argc == 3 && (!wcscmp(argv[1], L"--hfsx") || !wcscmp(argv[1], L"--hfsx-ci"))) {
            auto disk = CreateImage(argv[2], 128 * 1024 * 1024);
            bool sensitive = !wcscmp(argv[1], L"--hfsx");
            Volume::Format(*disk, u"HFVExplorer HFSX", true,
                           sensitive ? Volume::FormatKind::HfsXCaseSensitive
                                     : Volume::FormatKind::HfsXCaseInsensitive);
            Volume v(*disk, true);
            auto folder = v.MakeDirectory(2, u"Folder");
            v.Put(folder, u"Readme", 10000, 5000, pattern);
            if (sensitive)
                v.Put(folder, u"README", 99, 17, pattern);
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--check-recovered")) {
            auto disk = OpenImage(argv[2], false);
            Volume v(*disk);
            bool found = false;
            for (auto &e : v.List())
                found |= e.name == u"Recovered directory";
            must(found, "Independent checker did not replay our directory transaction");
            puts("PASS: independent checker recovered the committed catalog entry");
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--crash-existing")) {
            auto disk = OpenImage(argv[2], true);
            Volume v(*disk, true);
            v.fault = [](const char *at) {
                if (!strcmp(at, "journal-committed"))
                    throw Error("Simulated interruption");
            };
            fails([&] { v.MakeDirectory(2, u"Recovered directory"); }, "Crash did not fire");
            puts("PASS: committed transaction in reference volume retained for independent replay");
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--fragmented")) {
            Memory memory(16 * 1024 * 1024);
            Volume::Format(memory, u"Fragmented catalog", true);
            fragmentCatalog(memory);
            {
                Volume v(memory, true);
                auto id = v.MakeDirectory(2, u"Modified fragmented catalog");
                v.Put(id, u"Both forks", 10000, 5000, pattern);
            }
            auto disk = CreateImage(argv[2], memory.Size());
            disk->Write(0, memory.bytes.data(), memory.bytes.size());
            disk->Flush();
            puts("PASS: retained fragmented catalog interoperability fixture");
            return 0;
        }
        if (argc == 3 && !wcscmp(argv[1], L"--modify")) {
            auto disk = OpenImage(argv[2], true);
            Volume v(*disk, true);
            auto folder = v.MakeDirectory(2, u"From HFVExplorer \u00e9");
            v.Put(folder, u"Both forks", 100001, 5555, pattern);
            if (v.IsHfsX() && v.CaseSensitive()) {
                auto a = v.Put(folder, u"Readme", 777, 23, pattern);
                v.Put(folder, u"README", 99, 31, pattern);
                v.Rename(a, folder, u"readme");
                auto nested = v.MakeDirectory(folder, u"Nested");
                v.Rename(nested, 2, u"Moved folder");
            }
            puts("PASS: imported into independently formatted image");
            return 0;
        }
        if (argc == 3 && (!wcscmp(argv[1], L"--crash") || !wcscmp(argv[1], L"--crash-hfsx") ||
                          !wcscmp(argv[1], L"--crash-hfsx-ci"))) {
            auto disk = CreateImage(argv[2], 128 * 1024 * 1024);
            Volume::Format(*disk, u"Journal recovery", true,
                           !wcscmp(argv[1], L"--crash")        ? Volume::FormatKind::HfsPlus
                           : !wcscmp(argv[1], L"--crash-hfsx") ? Volume::FormatKind::HfsXCaseSensitive
                                                               : Volume::FormatKind::HfsXCaseInsensitive);
            Volume v(*disk, true);
            v.fault = [](const char *at) {
                if (!strcmp(at, "journal-committed"))
                    throw Error("Simulated interruption");
            };
            fails([&] { v.MakeDirectory(2, u"Recovered directory"); }, "Crash did not fire");
            puts("PASS: retained committed journal without applying home blocks");
            return 0;
        }
        attributeTests();
        stagedRangeValidation();
        hardLinks();hardLinkFailures();directoryMetadata();
        RunHfsPlusCompressionTests();
        RunIncrementalBTreeTests();
        journalResizeTests();
        resizeIoErrors();
        {
            auto fixture = largeCatalog();
        }
        basic(false);
        basic(true);
        crashes();
        for(auto kind : {Volume::FormatKind::HfsPlus,Volume::FormatKind::HfsXCaseSensitive,Volume::FormatKind::HfsXCaseInsensitive})
            mutationCrashes(kind);
        puts("PASS: HFS+/HFSX file/folder rename/delete recovery at every journal boundary; both forks and unrelated entries preserved");
        journalGeometry();
        trees();
        fragmented();
        fragmentedCatalog();
        containers();
        for (bool journaled : {false, true})
            for (bool sensitive : {false, true})
                hfsx(journaled, sensitive);
        crashes(Volume::FormatKind::HfsXCaseSensitive);
        crashes(Volume::FormatKind::HfsXCaseInsensitive);
        for (auto kind : {Volume::FormatKind::HfsXCaseSensitive, Volume::FormatKind::HfsXCaseInsensitive}) {
            journalGeometry(kind);
            fragmented(kind);
            fragmentedCatalog(kind);
        }
        must(CompareNames(std::u16string(1, 0) + u"a", u"a", true) < 0, "HFSX binary comparison skipped NUL");
        must(CompareNames(std::u16string(1, 0) + u"a", std::u16string(1, 0) + u"b", true) < 0,
             "HFSX binary comparison stopped at NUL");
        permissions();
        failures();
        must(Normalize(u"\uac01") == u"\u1100\u1161\u11a8", "Hangul decomposition");
        must(Normalize(u"\u212b") == u"\u212b", "HFS decomposition exclusion");
        fails([] { Normalize(std::u16string(256, u'a')); }, "Overlong name");
        must(Normalize(u"\U0001d15e") == u"\U0001d157\U0001d165", "Supplementary canonical decomposition");
        must(Normalize(u"\U0002f800") == u"\U0002f800", "Supplementary HFS exclusion");
        must(CompareNames(u"\u00c0", u"\u00e0") != 0, "HFS fold must preserve decomposable Latin codepoints");
        must(CompareNames(u"a\u200c", u"A") == 0, "HFS ignorable mapping");
        if (argc >= 2) {
            auto disk = CreateImage(argv[1], 128 * 1024 * 1024);
            Volume::Format(*disk, u"HFVExplorer HFS+", true);
            Volume v(*disk, true);
            auto folder = v.MakeDirectory(2, u"Unicode \u00e9");
            v.Put(folder, u"Both forks", 65539, 777, pattern);
        }
        puts("PASS: own HFS+ / HFSX formatters, both HFSX comparison modes, directory counts, read/write "
             "both forks, Unicode, multi-level catalog, journal "
             "commit/replay, crash phases, corrupt journal and read-only recovery");
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
