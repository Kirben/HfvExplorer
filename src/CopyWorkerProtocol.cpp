// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CopyWorkerProtocol.h"

namespace CopyWorkerProtocol {
using namespace FileOperations;
using MacBrowser::U16;

void Check(bool ok, const char *message) {
    if (!ok)
        throw HfsPlus::Error(message);
}
DWORD FrameLimit(DWORD message) {
    // A request holds the full selection; other messages remain small.
    return message == RequestPacket ? 64 * maximumFrame : maximumFrame;
}
Message ValidateFrame(const DWORD (&header)[3]) {
    Check(header[0] == protocol && header[1] >= RequestPacket && header[1] <= Finished &&
              header[2] <= FrameLimit(header[1]), "Invalid copy-worker protocol frame");
    return Message(header[1]);
}
void EncodeOutcome(Data &d, const Storage::Result &result) {
    d.put(uint64_t(result.outcome)); d.put(uint64_t(result.rollback));
    d.put(result.completed); d.put(result.skipped); d.put(result.partial);
}
void DecodeOutcome(Data &d, Storage::Result &result) {
    const auto outcome=d.get(), rollback=d.get(), completed=d.get(), skipped=d.get();
    Check(outcome<=uint64_t(Storage::Outcome::Failed) && rollback<=uint64_t(Storage::Rollback::Failed) &&
          completed<=UINT32_MAX && skipped<=UINT32_MAX, "Invalid worker operation outcome");
    result.outcome=Storage::Outcome(outcome); result.rollback=Storage::Rollback(rollback);
    result.completed=unsigned(completed); result.skipped=unsigned(skipped); result.partial=d.flag();
    const auto expected=result.error==ERROR_CANCELLED ? Storage::Outcome::Cancelled :
        result.code ? Storage::Outcome::Failed : result.skipped ? Storage::Outcome::Skipped : Storage::Outcome::Succeeded;
    Check(result.outcome==expected && (!result.partial||expected!=Storage::Outcome::Succeeded),
          "Inconsistent worker operation outcome");
}
void EncodeProgress(Data &d, const Storage::Progress &p) {
    d.put(p.path);
    d.put(p.phase);
    d.put(p.detail);
    d.put(p.bytes);
    d.put(p.processed);
    d.put(p.fileBytes);
    d.put(p.fileTotal);
    d.put(p.totalBytes);
    d.put(p.files);
    d.put(p.skipped);
    d.put(p.totalFiles);
    d.put(p.scanning);
    d.put(p.fileCopy);
    d.put(p.byteTransfer);
    d.put(p.utf8);
}
Storage::Progress DecodeProgress(Data &d) {
    Storage::Progress p;
    p.path = d.text();
    p.phase = d.text();
    p.detail = d.text();
    p.bytes = d.get();
    p.processed = d.get();
    p.fileBytes = d.get();
    p.fileTotal = d.get();
    p.totalBytes = d.get();
    p.files = unsigned(d.get());
    p.skipped = unsigned(d.get());
    p.totalFiles = unsigned(d.get());
    p.scanning = d.get() != 0;
    p.fileCopy = d.get() != 0;
    p.byteTransfer = d.get() != 0;
    p.utf8 = d.get() != 0;
    return p;
}
void EncodeOrigin(Data &d, const WorkerOrigin &o) {
    d.put(o.kind);
    d.wide(o.path);
    d.wide(o.journal);
    d.put(o.root);
    d.put(o.identity);
    d.put(uint64_t(o.drive + 1));
    d.put(o.offset);
    d.put(o.generation);
    d.put(o.generationKnown);
    d.put(o.writable);
}
WorkerOrigin DecodeOrigin(Data &d) {
    WorkerOrigin o;
    auto kind = d.get();
    Check(kind <= WorkerOrigin::Removable, "Invalid worker volume kind");
    o.kind = WorkerOrigin::Kind(kind);
    o.path = d.wide();
    o.journal = d.wide();
    o.root = d.text();
    o.identity = d.text();
    auto drive = d.get();
    Check(drive <= 26, "Invalid worker origin drive");
    o.drive = int(drive) - 1;
    o.offset = d.get();
    auto generation = d.get();
    Check(generation <= UINT32_MAX, "Invalid worker origin generation");
    o.generation = DWORD(generation);
    o.generationKnown = d.flag();
    o.writable = d.flag();
    return o;
}
void EncodeEndpoint(Data &d, const Endpoint &e) {
    const auto &l = e.location;
    d.put(uint64_t(l.kind()));
    d.put(l.cut);
    d.put(l.directory);
    d.put(l.expectedCatalogId);
    d.put(l.expectedParent);
    d.put(l.expectedIdentity);
    d.put(l.expectedResource);
    d.u16(l.expectedName);
    switch (l.kind()) {
    case Location::Kind::None:
        break;
    case Location::Kind::Windows:
        d.wide(l.WindowsPath());
        break;
    case Location::Kind::ClassicImage:
        d.put(l.ClassicImage());
        d.put(l.ClassicPath());
        break;
    case Location::Kind::ClassicMedia:
        d.put(l.Drive());
        d.put(uint64_t(l.Media()));
        d.put(l.ClassicPath());
        d.put(l.MediaGenerationKnown());
        if (l.MediaGenerationKnown())
            d.put(l.MediaGeneration());
        break;
    case Location::Kind::Mounted:
        d.put(l.Id());
        EncodeOrigin(d, e.origin);
        d.put(e.path.size());
        for (auto &p : e.path) {
            d.u16(p.name);
            d.put(p.resolvedId);
            d.put(p.version);
            d.put(p.directory);
            d.put(p.identity);
        }
        break;
    }
}
uint32_t CatalogNumber(Data &d) {
    auto value = d.get();
    Check(value <= UINT32_MAX, "Catalog identifier exceeds its field width");
    return uint32_t(value);
}
WireEndpoint ParseEndpoint(Data &d, bool allowMounted) {
    auto tag = d.get();
    Check(tag <= uint64_t(Location::Kind::ClassicMedia), "Invalid typed worker location");
    Check(allowMounted || tag != uint64_t(Location::Kind::Mounted),
          "Legacy conversion cannot carry a mounted backend");
    WireEndpoint endpoint;
    endpoint.kind = Location::Kind(tag);
    auto& l = endpoint.location;
    l.cut = d.flag();
    l.directory = d.flag();
    l.expectedCatalogId = CatalogNumber(d);
    l.expectedParent = CatalogNumber(d);
    l.expectedIdentity = d.text();
    l.expectedResource = d.text();
    l.expectedName = d.u16();
    switch (Location::Kind(tag)) {
    case Location::Kind::None:
        return endpoint;
    case Location::Kind::Windows:
        l.SetWindows(d.wide());
        return endpoint;
    case Location::Kind::ClassicImage: {
        auto image = d.text(), path = d.text();
        l.SetImage(std::move(image), std::move(path));
        break;
    }
    case Location::Kind::ClassicMedia: {
        auto drive = d.get(), media = d.get();
        Check(drive < 26 && media <= uint64_t(ClassicMedia::Cd), "Invalid physical worker location");
        auto path = d.text();
        auto generation = d.get();
        Check(generation <= 1, "Invalid media-generation presence flag");
        auto guard = l;
        l = Location::Physical(int(drive), ClassicMedia(media), std::move(path));
        l.cut = guard.cut;
        l.directory = guard.directory;
        l.expectedCatalogId = guard.expectedCatalogId;
        l.expectedParent = guard.expectedParent;
        l.expectedIdentity = std::move(guard.expectedIdentity);
        l.expectedResource = std::move(guard.expectedResource);
        l.expectedName = std::move(guard.expectedName);
        if (generation)
            l.BindMediaGeneration(CatalogNumber(d));
        return endpoint;
    }
    case Location::Kind::Mounted: {
        auto selectedId = endpoint.selectedId = CatalogNumber(d);
        Check(selectedId >= 2, "Invalid mounted catalog identifier");
        endpoint.origin = DecodeOrigin(d);
        auto count = d.get();
        Check(count <= 128, "Invalid worker path depth");
        auto& path = endpoint.path;
        for (uint64_t i = 0; i < count; ++i) {
            Entry e;
            e.name = d.u16();
            e.resolvedId = CatalogNumber(d);
            e.version = CatalogNumber(d);
            e.directory = d.flag();
            e.identity = d.text();
            path.push_back(std::move(e));
        }
        break;
    }
    }
    return endpoint;
}
bool SupportsOperation(Kind kind) {
    switch (kind) {
    case Kind::Copy: case Kind::LegacyCopy: case Kind::ExportFork: case Kind::ReplaceResource:
    case Kind::Rename: case Kind::Delete: case Kind::CreateFolder: case Kind::CreateHardLink:
    case Kind::SetProperties: case Kind::SetBasicMetadata: case Kind::SetAttributes:
    case Kind::SetCompression: return true;
    default: return false;
    }
}

void EncodeEntry(Data& d, const Entry& e) {
    d.put(e.id); d.put(e.parent); d.u16(e.name); d.put(e.version);
    d.put(e.resolvedId); d.put(e.linkCount); d.put(e.hardLink); d.put(e.identity); d.put(e.readError);
    d.put(e.created); d.put(e.modified); d.put(e.datesValid);
    d.put(e.directory); d.put(e.protectedFile); d.put(e.compressed);
    d.put(e.forks[0].size); d.put(e.forks[1].size);
    d.put(std::string(reinterpret_cast<const char*>(e.finder.data()), e.finder.size()));
}
Entry DecodeEntry(Data& d) {
    Entry e;
    e.id=CatalogNumber(d); e.parent=CatalogNumber(d); e.name=d.u16(); e.version=CatalogNumber(d);
    e.resolvedId=CatalogNumber(d); e.linkCount=CatalogNumber(d); e.hardLink=d.flag(); e.identity=d.text(); e.readError=d.text();
    e.created=d.get(); e.modified=d.get(); e.datesValid=d.flag();
    e.directory=d.flag(); e.protectedFile=d.flag(); e.compressed=d.flag();
    e.forks[0].size=d.get(); e.forks[1].size=d.get();
    auto finder=d.text();
    Check(finder.size()==e.finder.size(), "Invalid Finder metadata length");
    memcpy(e.finder.data(),finder.data(),finder.size());
    return e;
}
static void EncodeClassic(Data& d, const hfsdirent& e) {
    d.put(std::string(e.name, strnlen_s(e.name,sizeof(e.name))));
    d.put(uint32_t(e.flags)); d.put(uint32_t(e.cnid)); d.put(uint32_t(e.parid));
    d.put(uint64_t(e.crdate)); d.put(uint64_t(e.mddate)); d.put(uint64_t(e.bkdate));
    d.put(uint16_t(e.fdflags)); d.put(uint16_t(e.fdlocation.v)); d.put(uint16_t(e.fdlocation.h));
    if(e.flags&HFS_ISDIR) {
        d.put(e.u.dir.valence); d.put(uint16_t(e.u.dir.rect.top)); d.put(uint16_t(e.u.dir.rect.left));
        d.put(uint16_t(e.u.dir.rect.bottom)); d.put(uint16_t(e.u.dir.rect.right));
    } else {
        d.put(e.u.file.dsize); d.put(e.u.file.rsize);
        d.put(std::string(e.u.file.type,4)); d.put(std::string(e.u.file.creator,4));
    }
}
static uint16_t Word(Data& d) {
    auto value=d.get();
    Check(value<=UINT16_MAX,"Invalid worker metadata field width");
    return uint16_t(value);
}
static hfsdirent DecodeClassic(Data& d) {
    hfsdirent e{};
    auto name=d.text();
    Check(name.size()<sizeof(e.name)&&name.find(char(0))==name.npos,"Invalid classic metadata name");
    memcpy(e.name,name.data(),name.size());
    e.flags=int(CatalogNumber(d)); e.cnid=long(CatalogNumber(d)); e.parid=long(CatalogNumber(d));
    e.crdate=time_t(d.get()); e.mddate=time_t(d.get()); e.bkdate=time_t(d.get());
    e.fdflags=short(Word(d)); e.fdlocation.v=short(Word(d)); e.fdlocation.h=short(Word(d));
    if(e.flags&HFS_ISDIR) {
        e.u.dir.valence=Word(d); e.u.dir.rect.top=short(Word(d)); e.u.dir.rect.left=short(Word(d));
        e.u.dir.rect.bottom=short(Word(d)); e.u.dir.rect.right=short(Word(d));
    } else {
        e.u.file.dsize=CatalogNumber(d); e.u.file.rsize=CatalogNumber(d);
        auto type=d.text(), creator=d.text();
        Check(type.size()==4&&creator.size()==4,"Invalid classic type or creator");
        memcpy(e.u.file.type,type.data(),4); memcpy(e.u.file.creator,creator.data(),4);
    }
    return e;
}
static void EncodeSettings(Data &d, const Request &r, HANDLE parent, unsigned fault) {
    d.put(uint64_t(r.OperationKind()));
    if (auto* output = r.Get<ExportFileFork>()) d.put(uint64_t(output->fork));
    if (const auto* legacy = r.Conversion()) {
        EncodeEndpoint(d, Endpoint{legacy->source, {}, {}});
        EncodeEndpoint(d, Endpoint{legacy->destination, {}, {}});
        d.put(legacy->directory);
        d.put(legacy->move);
        d.put(uint8_t(legacy->options.output));
        d.put(uint8_t(legacy->options.input));
        d.put(legacy->options.appledouble);
        d.put(legacy->options.verifyFloppy);
        d.put(legacy->options.targetGenerationKnown);
        d.put(legacy->options.targetGeneration);
    }
    if(auto* rename=r.Rename()) { d.u16(rename->name); d.put(rename->checkNameLock); }
    if(auto* folder=r.Get<CreateFolder>()) d.u16(folder->name);
    if(auto* link=r.Get<FileOperations::CreateHardLink>()) d.u16(link->name);
    if(auto* metadata=r.Metadata()) {
        if(auto* classic=std::get_if<EditClassicProperties>(metadata)) {
            d.put(classic->change.index());
            if(auto* edit=std::get_if<ClassicPropertyEdit>(&classic->change)) {
                EncodeClassic(d,edit->original); EncodeClassic(d,edit->desired);
            } else if(auto* label=std::get_if<FinderLabel>(&classic->change)) d.put(label->value);
            else {
                const auto& position=std::get<FinderPosition>(classic->change);
                d.put(uint16_t(position.x)); d.put(uint16_t(position.y));
            }
        } else if(auto* edit=std::get_if<EditBasicMetadata>(metadata)) {
            EncodeEntry(d,edit->original); EncodeEntry(d,edit->desired);
        } else if(auto* edit=std::get_if<ChangeCompression>(metadata)) {
            EncodeEntry(d,edit->original); d.put(uint64_t(edit->method));
        }
    }
    d.put(uintptr_t(parent));
    d.put(fault);
}
Data EncodeRequest(const Request &request, HANDLE parent, unsigned testFault,
                   const std::vector<Endpoint> &sources, const Endpoint &target) {
    Data packet;
    EncodeSettings(packet, request, parent, testFault < 8 ? testFault : 0);
#ifdef HFV_TESTING
    if (testFault == 9) packet.bytes[8] = 2; // Invalid fork selector.
#endif
    packet.put(testFault == 8 ? 0 : sources.size() + (testFault == 10 ? 1 : 0));
    if (testFault != 8)
        for (const auto& source : sources) EncodeEndpoint(packet, source);
    if (testFault == 10) EncodeEndpoint(packet, sources.front());
    if (testFault != 11) EncodeEndpoint(packet, target);
#ifdef HFV_TESTING
    if (testFault == 12) packet.bytes.pop_back();
    if (testFault == 13) EncodeEndpoint(packet, target);
#endif
    return packet;
}
WireRequest DecodeRequest(Data &d) {
    WireRequest wire;
    auto kind = d.get();
    Check(kind <= uint64_t(Kind::SetCompression) && SupportsOperation(Kind(kind)),
          "Invalid worker operation");
    wire.kind = Kind(kind);
    if (wire.kind == Kind::LegacyCopy) {
        LegacyConversion legacy;
        legacy.source = ParseEndpoint(d, false).location;
        legacy.destination = ParseEndpoint(d, false).location;
        legacy.source.LegacyVolume();
        legacy.source.LegacyPath();
        legacy.destination.LegacyVolume();
        legacy.destination.LegacyPath();
        legacy.directory = d.flag();
        legacy.move = d.flag();
        auto output = d.get(), input = d.get();
        Check(output <= UINT8_MAX && input <= UINT8_MAX, "Invalid conversion mode");
        legacy.options.output = char(output);
        legacy.options.input = char(input);
        legacy.options.appledouble = d.flag();
        legacy.options.verifyFloppy = d.flag();
        legacy.options.targetGenerationKnown = d.flag();
        legacy.options.targetGeneration = CatalogNumber(d);
        wire.legacy = std::move(legacy);
    } else if (wire.kind == Kind::ExportFork) {
        auto fork = d.get();
        Check(fork <= uint64_t(ForkSelector::Resource), "Invalid fork selector");
        wire.fork = static_cast<ForkSelector>(fork);
    }
    switch(wire.kind) {
    case Kind::Rename:
        wire.name=d.u16(); wire.checkNameLock=d.flag(); break;
    case Kind::CreateFolder: case Kind::CreateHardLink:
        wire.name=d.u16(); break;
    case Kind::SetProperties: {
        auto tag=d.get();
        EditClassicProperties edit{};
        if(tag==0) edit.change=ClassicPropertyEdit{DecodeClassic(d),DecodeClassic(d)};
        else if(tag==1) {
            auto label=d.get(); Check(label<=7,"Invalid Finder label");
            edit.change=FinderLabel{int(label)};
        } else if(tag==2) edit.change=FinderPosition{short(Word(d)),short(Word(d))};
        else throw HfsPlus::Error("Invalid classic Properties change");
        wire.classic=std::move(edit);
        break;
    }
    case Kind::SetBasicMetadata:
        wire.basic=EditBasicMetadata{{},DecodeEntry(d),DecodeEntry(d)}; break;
    case Kind::SetCompression: {
        auto original=DecodeEntry(d); auto method=d.get();
        using Mode=HfsPlus::Volume::CompressionMode;
        Check(method<=uint64_t(Mode::StoredInline),"Invalid compression method");
        wire.compression=ChangeCompression{{},std::move(original),{},Mode(method)};
        break;
    }
    default: break;
    }
    if(wire.kind==Kind::Rename||wire.kind==Kind::CreateFolder||wire.kind==Kind::CreateHardLink)
        Check(!wire.name.empty(),"Missing worker operation name");
    wire.parent = HANDLE(uintptr_t(d.get()));
    Check(wire.parent != nullptr, "Missing copy-worker parent");
    wire.fault = CatalogNumber(d);
    auto count = d.get();
    if (wire.kind == Kind::Copy) Check(count > 0, "Copy requires a source");
    else if (wire.kind == Kind::LegacyCopy) Check(count <= 1, "Invalid conversion source count");
    else if (wire.kind == Kind::Delete) Check(count > 0, "Delete requires a selection");
    else if (wire.kind == Kind::CreateFolder) Check(!count, "Folder creation has no source");
    else Check(count == 1, wire.kind == Kind::ExportFork || wire.kind == Kind::ReplaceResource ?
               "Fork jobs require exactly one source" : "Worker job requires exactly one source");

    // The smallest endpoint has eight 64-bit fields, and a target must follow.
    Check(count < (d.bytes.size() - d.at) / 64, "Truncated copy-worker endpoints");
    for (uint64_t i = 0; i < count; ++i) {
        auto source = ParseEndpoint(d);
        Check(source.kind != Location::Kind::None, "Missing copy-worker source");
        wire.sources.push_back(std::move(source));
    }
    wire.target = ParseEndpoint(d);
    d.end();
    using Address = Location::Kind;
    switch (wire.kind) {
    case Kind::Copy:
        Check(wire.target.kind != Address::None, "Missing copy destination");
        break;
    case Kind::LegacyCopy:
        Check(count == 0 ? wire.target.kind == Address::None :
              wire.target.kind == Address::Windows || wire.target.kind == Address::ClassicImage ||
                  wire.target.kind == Address::ClassicMedia, "Invalid conversion selection");
        break;
    case Kind::ExportFork:
        Check(wire.target.kind == Address::Windows, "Fork export requires a Windows output file");
        break;
    case Kind::ReplaceResource:
        Check(wire.target.kind == Address::Mounted || wire.target.kind == Address::Windows,
              "Resource import requires a mounted or Windows destination file");
        break;
    case Kind::CreateFolder: case Kind::CreateHardLink:
        Check(wire.target.kind!=Address::None,"Missing worker destination"); break;
    case Kind::Rename:
        Check(wire.target.kind==Address::None || wire.target.kind==Address::Mounted,
              "Invalid rename parent"); break;
    case Kind::Delete: case Kind::SetProperties: case Kind::SetBasicMetadata:
    case Kind::SetAttributes: case Kind::SetCompression:
        Check(wire.target.kind==Address::None,"Unexpected worker destination"); break;
    default: throw HfsPlus::Error("Invalid worker operation");
    }
    return wire;
}
} // namespace CopyWorkerProtocol
