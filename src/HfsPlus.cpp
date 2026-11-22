// SPDX-License-Identifier: GPL-2.0-or-later
// On-disk structures are decoded explicitly, following Apple TN1150. No native
// structs, compiler packing assumptions, or third-party filesystem engine.
#include "HfsPlusDisk.h"
#include "DiskPartitions.h"
#include "HfsPlusBTree.h"
#include "HfsPlusCompression.h"
#include "HfsPlusTime.h"
#include <ctime>
#include <type_traits>
namespace HfsPlus {
namespace {
using namespace Disk;
uint64_t Now() { return uint64_t(time(nullptr)) + Time::Epoch; }
using Record = TreeRecord;
std::u16string String(const uint8_t *p, size_t available) {
    Check(available >= 2, "Truncated Unicode name");
    unsigned n = U16(p);
    Check(n <= 255 && 2 + n * 2 <= available, "Invalid Unicode name length");
    std::u16string s;
    for (unsigned i = 0; i < n; ++i)
        s += char16_t(U16(p + 2 + i * 2));
    return s;
}
Bytes Key(uint32_t parent, const std::u16string &name) {
    Bytes b(8 + name.size() * 2);
    P16(b.data(), uint16_t(b.size() - 2));
    P32(b.data() + 2, parent);
    P16(b.data() + 6, uint16_t(name.size()));
    for (size_t i = 0; i < name.size(); ++i)
        P16(b.data() + 8 + i * 2, uint16_t(name[i]));
    return b;
}
Fork DecodeFork(const uint8_t *p) {
    Fork f;
    f.size = U64(p);
    f.blocks = U32(p + 12);
    Check(f.size <= INT64_MAX, "HFS+ fork exceeds Apple limit");
    for (unsigned i = 0; i < 8; ++i) {
        Extent e{U32(p + 16 + i * 8), U32(p + 20 + i * 8)};
        if (e.count)
            f.extents.push_back(e);
    }
    return f;
}
void EncodeFork(uint8_t *p, const Fork &f, uint32_t clump) {
    memset(p, 0, 80);
    P64(p, f.size);
    P32(p + 8, clump);
    P32(p + 12, f.blocks);
    for (size_t i = 0; i < f.extents.size() && i < 8; ++i) {
        P32(p + 16 + i * 8, f.extents[i].start);
        P32(p + 20 + i * 8, f.extents[i].count);
    }
}
void ForkRead(View &v, const Fork &f, uint32_t bs, uint64_t offset, void *buffer, size_t count) {
    Check(Range(offset, count, f.size), "Read past HFS+ fork EOF");
    if (!count)
        return;
    auto p = static_cast<uint8_t *>(buffer);
    for (auto &e : f.extents) {
        uint64_t n = uint64_t(e.count) * bs;
        if (offset >= n) {
            offset -= n;
            continue;
        }
        size_t part = size_t((std::min)(uint64_t(count), n - offset));
        v.Read(uint64_t(e.start) * bs + offset, p, part);
        p += part;
        count -= part;
        offset = 0;
        if (!count)
            return;
    }
    Check(!count, "Incomplete HFS+ fork extents");
}
void ForkStage(View &v, const Fork &f, uint32_t bs, const Bytes &data) {
    size_t pos = 0;
    for (auto &e : f.extents) {
        size_t n = size_t((std::min)(uint64_t(data.size() - pos), uint64_t(e.count) * bs));
        if (n)
            v.Stage(uint64_t(e.start) * bs, data.data() + pos, n);
        pos += n;
        if (pos == data.size())
            return;
    }
    Check(pos == data.size(), "Metadata fork is too short");
}
// Walk the leaf chain with structural and bounded-range checks. The rebuilt
// trees below include indexes for interoperability with native Mac readers.
std::vector<Record> ReadTree(View &view, const Fork &fork, uint32_t bs, unsigned keyLimit,
                             uint8_t *comparison = nullptr) {
    Check(fork.size >= 512, "Missing HFS+ B-tree");
    Bytes initial(120);
    ForkRead(view, fork, bs, 0, initial.data(), initial.size());
    Check(initial[8] == 1 && U16(initial.data() + 10) == 3, "Invalid HFS+ B-tree header node");
    uint16_t ns = U16(initial.data() + 32);
    uint32_t nodes = U32(initial.data() + 36), leaf = U32(initial.data() + 24),
             last = U32(initial.data() + 28), expected = U32(initial.data() + 20);
    Check(ns >= 512 && ns <= 32768 && (ns & (ns - 1)) == 0 && nodes && uint64_t(nodes) * ns <= fork.size,
          "Invalid HFS+ B-tree geometry");
    if (comparison)
        *comparison = initial[51];
    std::vector<Record> result;
    std::set<uint32_t> visited;
    uint32_t previous = 0;
    while (leaf) {
        Check(leaf < nodes && visited.insert(leaf).second, "Invalid or cyclic HFS+ leaf chain");
        Bytes node(ns);
        ForkRead(view, fork, bs, uint64_t(leaf) * ns, node.data(), ns);
        Check(node[8] == 255 && node[9] == 1 && U32(node.data() + 4) == previous,
              "Invalid HFS+ leaf descriptor");
        unsigned records = U16(node.data() + 10);
        Check(records && records < (ns - 14) / 4, "Invalid HFS+ leaf record count");
        for (unsigned i = 0; i < records; ++i) {
            unsigned a = U16(node.data() + ns - 2 * (i + 1)), b = U16(node.data() + ns - 2 * (i + 2));
            Check(a >= 14 && a + 2 <= b && b <= ns - 2 * (records + 1), "Invalid HFS+ record offsets");
            unsigned kl = U16(node.data() + a) + 2;
            Check(kl >= 4 && kl <= keyLimit + 2 && kl <= b - a, "Invalid HFS+ B-tree key");
            unsigned padded = (kl + 1) & ~1u;
            Check(padded <= b - a, "Truncated HFS+ record");
            result.push_back({Bytes(node.begin() + a, node.begin() + a + kl),
                              Bytes(node.begin() + a + padded, node.begin() + b)});
            Check(result.size() <= expected, "HFS+ leaf count exceeds its header");
        }
        previous = leaf;
        leaf = U32(node.data());
    }
    Check(result.size() == expected && previous == last, "HFS+ leaf count mismatch");
    return result;
}
// Bootstrap only: formatting starts with a few root/journal records. Existing
// trees are edited incrementally by Disk::BTree below, never rebuilt.
Bytes BuildInitialTree(const std::vector<Record> &records, bool catalog, uint32_t clump = 65536,
                       uint32_t allocationBlock = 4096, bool caseSensitive = false) {
    const unsigned ns = 4096;
    std::vector<Bytes> nodes(1, Bytes(ns, 0));
    struct Index {
        Bytes key;
        uint32_t node;
    };
    std::vector<Index> level;
    auto nodeFrom = [&](const std::vector<Bytes> &recs, int kind, unsigned height) -> uint32_t {
        Bytes node(ns, 0);
        node[8] = uint8_t(kind);
        node[9] = uint8_t(height);
        P16(node.data() + 10, uint16_t(recs.size()));
        size_t pos = 14;
        for (size_t i = 0; i < recs.size(); ++i) {
            Check(pos + recs[i].size() + 2 * (recs.size() + 1) <= ns, "HFS+ record exceeds node size");
            P16(node.data() + ns - 2 * (i + 1), uint16_t(pos));
            memcpy(node.data() + pos, recs[i].data(), recs[i].size());
            pos += recs[i].size();
        }
        P16(node.data() + ns - 2 * (recs.size() + 1), uint16_t(pos));
        nodes.push_back(std::move(node));
        return uint32_t(nodes.size() - 1);
    };
    uint32_t first = 0, last = 0;
    std::vector<Bytes> group;
    size_t used = 14;
    Bytes firstKey;
    auto flushLeaf = [&] {
        if (group.empty())
            return;
        uint32_t num = nodeFrom(group, -1, 1);
        if (last) {
            P32(nodes[last].data(), num);
            P32(nodes[num].data() + 4, last);
        } else
            first = num;
        last = num;
        level.push_back({firstKey, num});
        group.clear();
        used = 14;
    };
    for (auto &r : records) {
        Bytes record = r.key;
        if (record.size() & 1)
            record.push_back(0);
        record.insert(record.end(), r.value.begin(), r.value.end());
        if (used + record.size() + 2 * (group.size() + 2) > ns)
            flushLeaf();
        if (group.empty())
            firstKey = r.key;
        used += record.size();
        group.push_back(std::move(record));
    }
    flushLeaf();
    unsigned depth = level.empty() ? 0 : 1;
    while (level.size() > 1) {
        std::vector<Index> next;
        group.clear();
        used = 14;
        auto flushIndex = [&] {
            if (group.empty())
                return;
            uint32_t num = nodeFrom(group, 0, depth + 1);
            next.push_back({firstKey, num});
            group.clear();
            used = 14;
        };
        for (auto &e : level) {
            Bytes r = e.key;
            if (r.size() & 1)
                r.push_back(0);
            size_t pos = r.size();
            r.resize(pos + 4);
            P32(r.data() + pos, e.node);
            if (used + r.size() + 2 * (group.size() + 2) > ns)
                flushIndex();
            if (group.empty())
                firstKey = e.key;
            used += r.size();
            group.push_back(std::move(r));
        }
        flushIndex();
        level = std::move(next);
        ++depth;
    }
    size_t total = (std::max)(size_t(16), nodes.size());
    if (allocationBlock > ns)
        total = (total + allocationBlock / ns - 1) / (allocationBlock / ns) * (allocationBlock / ns);
    Check(total <= (ns - 256) * 8, "HFS+ B-tree exceeds supported map size");
    size_t inUse = nodes.size();
    nodes.resize(total, Bytes(ns, 0));
    auto &header = nodes[0];
    header[8] = 1;
    P16(header.data() + 10, 3);
    auto p = header.data() + 14;
    P16(p, uint16_t(depth));
    P32(p + 2, level.empty() ? 0 : level[0].node);
    P32(p + 6, uint32_t(records.size()));
    P32(p + 10, first);
    P32(p + 14, last);
    P16(p + 18, ns);
    P16(p + 20, catalog ? 516 : 10);
    P32(p + 22, uint32_t(total));
    P32(p + 26, uint32_t(total - inUse));
    P32(p + 32, clump);
    p[37] = catalog ? (caseSensitive ? 0xbc : 0xcf) : 0;
    P32(p + 38, catalog ? 6 : 2);
    P16(header.data() + ns - 2, 14);
    P16(header.data() + ns - 4, 120);
    P16(header.data() + ns - 6, 248);
    P16(header.data() + ns - 8, ns - 8);
    for (size_t i = 0; i < inUse; ++i)
        header[248 + i / 8] |= uint8_t(0x80 >> (i % 8));
    Bytes out;
    out.reserve(total * ns);
    for (auto &n : nodes)
        out.insert(out.end(), n.begin(), n.end());
    return out;
}
} // namespace
struct Volume::Impl {
    Source &source;
    View view;
    Journal journal;
    Bytes header, bitmap;
    uint32_t bs = 0, total = 0, freeBlocks = 0;
    bool write = false, usable = true, caseSensitive = false;
    Fork allocation, extents, catalog, attributes;
    std::vector<Record> catalogRecords, overflow;
    std::vector<Record> committedCatalog, committedOverflow, committedAttributes;
    Bytes committedBitmap;
    Fork committedCatalogFork, committedExtentsFork, committedAttributesFork;
    const Bytes *allocationExclusions = nullptr;
    std::map<uint32_t, Entry> entries;
    std::vector<Record> attributeRecords;
    std::map<std::pair<uint32_t,std::u16string>,AttributeData> attributeSnapshots;
    bool attributesLoaded = false, attributesDirty = false, attributeIndexValid = false;
    std::map<std::pair<uint32_t,std::u16string>,std::map<uint32_t,size_t>> attributeIndex;
    std::map<uint32_t, std::unique_ptr<Compression>> compressedReaders;
    uint32_t fileMetadata = 0, directoryMetadata = 0;
    explicit Impl(Source &s, bool w) : source(s), view(s), journal(view), write(w) {
        Check(!w || s.Writable(), "Cannot open a read-only image for writing");
        const auto selected=DiskPartitions::FindHfsPlus(s);
        Check(selected.has_value(), "No HFS+ volume found");
        const auto& partition=selected->partition;
        Check(!w||!partition.readOnly,"HFS+ partition is read-only");
        view.base=partition.offset;
        view.length=partition.length;
        LoadHeader();
        // A relocated journal can be discovered through either committed redo
        // copy. Follow JIB after each replay; read-only mounts use the overlay.
        std::set<uint64_t> journalLocations;
        bool replayPending = false;
        for (;;) {
            Journal current(view);
            // Recovery is an overlay until all filesystem validation succeeds.
            current.Open(header, false);
            replayPending |= current.enabled && current.start != current.end;
            journal.Adopt(current);
            if (!journal.enabled)
                break;
            auto info = view.Read(journal.infoOffset, 180);
            uint64_t next = U64(info.data() + 36);
            if (next == journal.offset && U64(info.data() + 44) == journal.size)
                break;
            Check(journalLocations.insert(journal.offset).second, "Cyclic relocated journal chain");
        }
        if(w&&journal.External())Check(journal.device->Writable(),"The external journal must be writable for writable recovery");
        LoadHeader();
        // Read extents first; the extents file itself must bootstrap from its inline extents.
        allocation = DecodeFork(header.data() + 112);
        extents = DecodeFork(header.data() + 192);
        catalog = DecodeFork(header.data() + 272);
        Validate(extents);
        overflow = ReadTree(view, extents, bs, 10);
        for (const auto &r : overflow)
            Check(r.key.size() == 12 && r.value.size() == 64 && (r.key[2] == 0 || r.key[2] == 0xff),
                  "Invalid HFS+ extents-overflow record");
        Complete(allocation, 6, 0);
        Complete(catalog, 4, 0);
        Validate(allocation);
        Validate(catalog);
        Check(allocation.size >= (uint64_t(total) + 7) / 8 && allocation.size <= 536870912,
              "Invalid HFS+ allocation bitmap length");
        bitmap.resize(size_t(allocation.size));
        ForkRead(view, allocation, bs, 0, bitmap.data(), bitmap.size());
        if (write)
            freeBlocks = CountFreeBlocks();
        uint8_t compare = 0;
        catalogRecords = ReadTree(view, catalog, bs, 516, &compare);
        caseSensitive = compare == 0xbc;
        Check(compare == 0xcf || compare == 0xbc, "Unknown HFS+ filename comparison");
        Check(U16(header.data()) == 0x4858 || !caseSensitive,
              "Binary filename comparison requires an HFSX volume");
        LoadEntries();
        LoadAttributes();
        AuditAllocation();
        if (w && replayPending) {
            view.Apply();
            // Only the final referenced journal may be cleared: an older journal
            // region can already have been freed/reused by the recovered state.
            journal.start = journal.end;
            journal.StoreHeader();
        }
        Snapshot();
    }
    void Snapshot() {
        committedCatalog = catalogRecords;
        committedOverflow = overflow;
        committedBitmap = bitmap;
        committedCatalogFork = catalog;
        committedExtentsFork = extents;
        committedAttributes = attributeRecords;
        attributesDirty=false;
        committedAttributesFork = attributes;
    }
    void LoadHeader() {
        header = view.Read(1024, 512);
        uint16_t sig = U16(header.data());
        Check((sig == 0x482b && U16(header.data() + 2) == 4) ||
                  (sig == 0x4858 && U16(header.data() + 2) == 5),
              "Invalid HFS+ volume signature/version");
        bs = U32(header.data() + 40);
        total = U32(header.data() + 44);
        Check(bs >= 512 && bs <= 1024 * 1024 && (bs & (bs - 1)) == 0 && total >= 16 &&
                  uint64_t(total) * bs <= view.length,
              "Invalid HFS+ volume geometry");
        view.length = uint64_t(total) * bs;
        Check(U32(header.data() + 48) <= total, "Invalid HFS+ free block count");
        if (write) {
            uint32_t attributes = U32(header.data() + 4);
            Check(!(attributes & 0xc080), "HFS+ volume is locked or inconsistent");
            Check((attributes & 0x2000) || (attributes & 0x100),
                  "Non-journaled HFS+ volume was not cleanly unmounted; repair it before writing");
            Check(!(attributes & 0x40000000), "Content-protected HFS+ volumes are unsupported");
        }
    }
    void Validate(const Fork &f) const {
        uint64_t blocks = 0;
        std::vector<Extent> sorted = f.extents;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Extent &a, const Extent &b) { return a.start < b.start; });
        uint64_t end = 0;
        for (auto &e : sorted) {
            Check(e.count && e.start >= end && Range(e.start, e.count, total),
                  "Invalid or overlapping HFS+ extents");
            end = uint64_t(e.start) + e.count;
            blocks += e.count;
        }
        Check(blocks == f.blocks && f.size <= blocks * bs, "HFS+ fork allocation mismatch");
    }
    void Complete(Fork &f, uint32_t id, unsigned which) {
        uint64_t have = 0;
        for (auto e : f.extents)
            have += e.count;
        while (have < f.blocks) {
            const Record *found = nullptr;
            for (auto &r : overflow) {
                Check(r.key.size() == 12 && r.value.size() == 64, "Invalid overflow extent record");
                if (r.key[2] == (which ? 0xff : 0) && U32(r.key.data() + 4) == id &&
                    U32(r.key.data() + 8) == have) {
                    Check(!found, "Duplicate overflow extent record");
                    found = &r;
                }
            }
            Check(found, "Missing HFS+ overflow extents");
            uint64_t before = have;
            for (unsigned i = 0; i < 8; ++i) {
                Extent e{U32(found->value.data() + i * 8), U32(found->value.data() + i * 8 + 4)};
                if (e.count) {
                    f.extents.push_back(e);
                    have += e.count;
                }
            }
            Check(have > before && have <= f.blocks, "Invalid HFS+ overflow extent count");
        }
        Validate(f);
    }
    int CatalogOrder(const Bytes& a, const Bytes& b) const {
        if (a == b)
            return 0;
        const auto parentA = U32(a.data() + 2), parentB = U32(b.data() + 2);
        if (parentA != parentB)
            return parentA < parentB ? -1 : 1;
        return CompareNames(String(a.data() + 6, a.size() - 6),
                            String(b.data() + 6, b.size() - 6), caseSensitive);
    }
    int Order(const Record& a, const Record& b) const {
        return CatalogOrder(a.key, b.key);
    }
    size_t CatalogPosition(const Bytes& key) const {
        const auto at = std::lower_bound(catalogRecords.begin(), catalogRecords.end(), key,
            [&](const Record& record, const Bytes& value) { return CatalogOrder(record.key, value) < 0; });
        return size_t(at - catalogRecords.begin());
    }
    Record* InsertCatalogRecord(Record record) {
        const auto position = CatalogPosition(record.key);
        Check(position == catalogRecords.size() || CatalogOrder(catalogRecords[position].key, record.key) != 0,
              "Duplicate HFS+ catalog key");
        return &*catalogRecords.insert(catalogRecords.begin() + position, std::move(record));
    }
    Record* ChangeCatalogKey(Record* record, Bytes key) {
        if (record->key == key)
            return record;
        if (CatalogOrder(record->key, key) == 0) {
            record->key = std::move(key);
            return record;
        }
        Record updated = std::move(*record);
        catalogRecords.erase(catalogRecords.begin() + (record - catalogRecords.data()));
        updated.key = std::move(key);
        return InsertCatalogRecord(std::move(updated));
    }
    void LoadEntries() {
        compressedReaders.clear();
        attributeSnapshots.clear();attributeRecords.clear();
        attributeIndex.clear();attributeIndexValid=false;
        attributesLoaded = false;
        fileMetadata = directoryMetadata = 0;
        entries.clear();
        const Record *previous = nullptr;
        for (auto &r : catalogRecords) {
            Check(r.key.size() >= 8 && r.value.size() >= 2, "Truncated HFS+ catalog record");
            if (previous)
                Check(Order(*previous, r) < 0, "HFS+ catalog keys are not ordered");
            previous = &r;
            unsigned type = U16(r.value.data());
            Check(type >= 1 && type <= 4, "Unknown HFS+ catalog record type");
            if (type != 1 && type != 2)
                continue;
            Check(r.value.size() == (type == 1 ? 88 : 248), "Invalid HFS+ catalog entry length");
            Entry e;
            e.parent = U32(r.key.data() + 2);
            e.name = String(r.key.data() + 6, r.key.size() - 6);
            e.directory = type == 1;
            const auto mode=U16(r.value.data()+42);
            e.symbolicLink = !e.directory && (mode & 0170000) == 0120000;
            const auto special=e.directory?SpecialFileType::None:SpecialType(mode>>12);
            if(e.symbolicLink||special!=SpecialFileType::None||(mode&0170000)==(e.directory?0040000:0100000))
                e.posix=PosixMetadata{U32(r.value.data()+32),U32(r.value.data()+36),
                                      uint16_t(mode&PosixMetadata::ModeMask),
                                      uint8_t(((r.value[41]&2)?PosixMetadata::UserImmutable:0)|
                                              ((r.value[40]&2)?PosixMetadata::SystemImmutable:0)|
                                              ((r.value[41]&4)?PosixMetadata::UserAppendOnly:0)|
                                              ((r.value[40]&4)?PosixMetadata::SystemAppendOnly:0)),
                                      uint8_t(((r.value[41]&1)?PosixMetadata::NoDump:0)|
                                              ((r.value[41]&8)?PosixMetadata::Opaque:0)|
                                              ((r.value[40]&1)?PosixMetadata::Archived:0)),special,
                                      IsDevice(special)?U32(r.value.data()+44):0};
            e.compressed = !e.directory && (r.value[41] & 0x20);
            e.id = U32(r.value.data() + 8);
            e.created = Time::Decode(U32(r.value.data() + 12),(U16(r.value.data()+2)&0x1000)!=0);
            e.modified = Time::Decode(U32(r.value.data() + 16),(U16(r.value.data()+2)&0x1000)!=0);
            memcpy(e.finder.data(), r.value.data() + 48, 32);
            e.protectedFile =
                e.id < 16 ||
                (journal.enabled && (e.name == u".journal" || e.name == u".journal_info_block")) ||
                e.name == u".HFS+ Private Directory Data\r" ||
                e.name == std::u16string(4, 0) + u"HFS+ Private Data";
            if (!e.directory) {
                e.forks[0] = DecodeFork(r.value.data() + 88);
                e.forks[1] = DecodeFork(r.value.data() + 168);
                Complete(e.forks[0], e.id, 0);
                Complete(e.forks[1], e.id, 1);
                if (journal.enabled)
                    for (auto &f : e.forks)
                        for (auto x : f.extents) {
                            uint64_t o = uint64_t(x.start) * bs, n = uint64_t(x.count) * bs;
                            if (journal.Overlaps(o, n) ||
                                (o <= journal.infoOffset && journal.infoOffset < o + n))
                                e.protectedFile = true;
                        }
            }
            Check(entries.emplace(e.id, std::move(e)).second, "Duplicate HFS+ catalog ID");
        }
        Check(entries.count(2) && entries.at(2).directory, "Missing HFS+ root directory");
        for (auto &pair : entries) {
            auto &e = pair.second;
            if (e.parent != 2 || !e.directory)
                continue;
            if (e.name == std::u16string(4, 0) + u"HFS+ Private Data" ||
                e.name == std::u16string(4, 0x2400) + u"HFS+ Private Data") {
                Check(!fileMetadata, "Ambiguous HFS+ hard-link metadata directory");
                fileMetadata = e.id;
            }
            if (e.name == u".HFS+ Private Directory Data\r")
                directoryMetadata = e.id;
        }
        // Never expose or mutate hidden backing inodes through the ordinary UI.
        for (auto &pair : entries) {
            auto &e = pair.second;
            uint32_t parent = e.id;
            std::set<uint32_t> seen;
            while (parent != 2 && seen.insert(parent).second) {
                if (e.id == fileMetadata || e.id == directoryMetadata || e.parent == fileMetadata || e.parent == directoryMetadata) {
                    e.protectedFile = true;
                    break;
                }
                auto i = entries.find(parent);
                if (i == entries.end())
                    break; // AuditAllocation reports malformed parent chains.
                parent = i->second.parent;
            }
        }
    }
    void Guard() {
        Check(usable, "Reopen the HFS+ image after a failed operation");
        Check(write, "HFS+ image is read-only");
    }
    Entry &Get(uint32_t id) {
        auto p = entries.find(id);
        Check(p != entries.end(), "HFS+ entry not found");
        return p->second;
    }
    void LoadAttributes() {
        if (attributesLoaded)
            return;
        attributes = DecodeFork(header.data() + 352);
        Fork &f = attributes;
        if (f.size) {
            Complete(f, 8, 0);
            Validate(f);
            auto records = ReadTree(view, f, bs, 522);
            for (const auto &r : records)
                Check(r.key.size() >= 14 && 14 + uint64_t(U16(r.key.data() + 12)) * 2 == r.key.size() &&
                          U16(r.key.data() + 12) <= 255 && r.value.size() >= 4,
                      "Invalid HFS+ attribute record");
            const Record *previous = nullptr;
            for (const auto &r : records) {
                Check(!previous || AttributeOrder(*previous, r) < 0, "Unordered HFS+ attribute keys");
                previous = &r;
            }
            attributeRecords = std::move(records);
        }
        attributesLoaded = true;
        IndexAttributes();
        for(const auto& group:attributeIndex){auto id=group.first.first;Check(entries.count(id)||id==1,"Orphan HFS+ attribute owner");
            Check(!group.first.second.empty(),"Empty HFS+ attribute name");auto storage=AttributeStorage(id,group.first.second);(void)storage;}

    }
    static std::u16string AttributeName(const Record &r) { return String(r.key.data()+12, r.key.size()-12); }
    static int AttributeOrder(const Record &a, const Record &b) {
        auto x=U32(a.key.data()+4),y=U32(b.key.data()+4);
        if(x!=y)return x<y?-1:1;
        auto an=AttributeName(a),bn=AttributeName(b);
        if(an!=bn)return an<bn?-1:1;
        x=U32(a.key.data()+8);y=U32(b.key.data()+8);return x==y?0:x<y?-1:1;
    }
    static Bytes AttributeKey(uint32_t id,const std::u16string& name,uint32_t block=0) {
        Bytes key(14+name.size()*2,0);P16(key.data(),uint16_t(key.size()-2));P32(key.data()+4,id);
        P32(key.data()+8,block);P16(key.data()+12,uint16_t(name.size()));
        for(size_t i=0;i<name.size();++i)P16(key.data()+14+2*i,name[i]);return key;
    }
    void IndexAttributes() {
        if(attributeIndexValid)return;attributeIndex.clear();
        for(size_t i=0;i<attributeRecords.size();++i){auto& r=attributeRecords[i];Check(attributeIndex[{U32(r.key.data()+4),AttributeName(r)}].emplace(U32(r.key.data()+8),i).second,"Duplicate HFS+ attribute extent");}
        attributeIndexValid=true;
    }
    struct AttributeValue {const Bytes *inlineValue=nullptr;Fork fork;};
    AttributeValue AttributeStorage(uint32_t id,const std::u16string& name) {
        LoadAttributes();IndexAttributes();auto found=attributeIndex.find({id,name});Check(found!=attributeIndex.end(),"Missing HFS+ attribute");auto& parts=found->second;
        Check(parts.count(0),"Missing HFS+ attribute base record");const auto& v=attributeRecords[parts.at(0)].value;AttributeValue result;
        if(U32(v.data())==0x10){
            Check(parts.size()==1&&v.size()>=16&&U32(v.data()+12)<=v.size()-16&&v.size()-16-U32(v.data()+12)<=1,"Invalid inline HFS+ attribute size");
            result.inlineValue=&v;return result;
        }
        Check(U32(v.data())==0x20&&v.size()==88,"Unknown HFS+ attribute storage type");auto& f=result.fork;f=DecodeFork(v.data()+8);
        uint64_t have=0;for(auto e:f.extents)have+=e.count;size_t used=1;
        while(have<f.blocks){auto i=parts.find(uint32_t(have));Check(i!=parts.end()&&attributeRecords[i->second].value.size()==72&&U32(attributeRecords[i->second].value.data())==0x30,"Missing HFS+ attribute overflow extents");
            auto before=have;for(unsigned n=0;n<8;++n){auto p=attributeRecords[i->second].value.data()+8+n*8;Extent e{U32(p),U32(p+4)};if(e.count){f.extents.push_back(e);have+=e.count;}}
            Check(have>before&&have<=f.blocks,"Invalid HFS+ attribute overflow extents");++used;
        }
        Check(parts.size()==used,"Unexpected HFS+ attribute extents");Validate(f);return result;
    }
    AttributeData Attribute(uint32_t id,const std::u16string& name) {
        auto key=std::make_pair(id,name);auto cached=attributeSnapshots.find(key);if(cached!=attributeSnapshots.end())return cached->second;
        auto value=AttributeStorage(id,name);AttributeData data;
        if(value.inlineValue){auto& v=*value.inlineValue;data=Bytes(v.begin()+16,v.begin()+16+U32(v.data()+12));}
        else{auto& f=value.fork;
            if(name==u"com.apple.decmpfs"){Check(f.size<=131088,"Oversized com.apple.decmpfs attribute");Bytes bytes(size_t(f.size));ForkRead(view,f,bs,0,bytes.data(),bytes.size());data=std::move(bytes);}
            else data=AttributeData::Snapshot(f.size,[&](uint64_t at,void* p,size_t n){ForkRead(view,f,bs,at,p,n);});
        }
        attributeSnapshots.emplace(std::move(key),data);return data;
    }
    Attributes GetAttributes(uint32_t id) {
        LoadAttributes();IndexAttributes();Attributes out;
        for(auto i=attributeIndex.lower_bound({id,{}});i!=attributeIndex.end()&&i->first.first==id;++i){auto name=i->first.second;out.emplace(name,Attribute(id,name));}
        return out;
    }
    void AttributeFlags(uint32_t id) {
        bool any=false,security=false;for(const auto& r:attributeRecords)if(U32(r.key.data()+4)==id){any=true;security|=AttributeName(r)==u"com.apple.system.Security";}
        auto record=CatalogRecord(id);auto flags=U16(record->value.data()+2);P16(record->value.data()+2,uint16_t((flags&~12u)|(any?4:0)|(security?8:0)));
    }
    void ReplaceAttributes(uint32_t id,const Attributes& desired,bool compressionWriter=false,bool linkWriter=false) {
        auto ordinary=desired;if(linkWriter)ordinary.erase(u"com.apple.system.hfs.firstlink");if(compressionWriter){auto i=ordinary.find(u"com.apple.decmpfs");if(i!=ordinary.end()){Check(i->second.size()<=131088,"Oversized decmpfs attribute");ordinary.erase(i);}}ValidateAttributes(ordinary);LoadAttributes();auto old=GetAttributes(id);if(old==desired)return;attributesDirty=true;
        std::vector<Fork> freed;
        for(const auto& value:old){auto now=desired.find(value.first);if(now!=desired.end()&&now->second==value.second)continue;
            auto storage=AttributeStorage(id,value.first);if(!storage.inlineValue)freed.push_back(storage.fork);}
        // Allocate payloads before freeing old blocks. Existing on-disk values must
        // survive an interrupted transaction and never become scratch space.
        std::vector<Record> added;
        for(const auto& value:desired){auto was=old.find(value.first);if(was!=old.end()&&was->second==value.second)continue;
            Record record;record.key=AttributeKey(id,value.first);
            if(value.second.size()<=1024){record.value.assign(16+value.second.size()+(value.second.size()&1),0);P32(record.value.data(),0x10);P32(record.value.data()+12,uint32_t(value.second.size()));value.second.Read(0,record.value.data()+16,size_t(value.second.size()));added.push_back(std::move(record));}
            else{auto f=Allocate(value.second.size(),false,true);uint64_t logical=0;Bytes buffer(65536,0);
                for(auto e:f.extents){uint64_t at=uint64_t(e.start)*bs,remaining=uint64_t(e.count)*bs;while(remaining){size_t n=size_t((std::min)(remaining,uint64_t(buffer.size()))),actual=size_t((std::min)(uint64_t(n),f.size-logical));std::fill(buffer.begin(),buffer.begin()+n,0);value.second.Read(logical,buffer.data(),actual);view.Write(at,buffer.data(),n);logical+=actual;at+=n;remaining-=n;}}
                record.value.assign(88,0);P32(record.value.data(),0x20);EncodeFork(record.value.data()+8,f,bs);added.push_back(std::move(record));
                uint32_t block=0;for(size_t i=0;i<f.extents.size();){if(i<8){block+=f.extents[i++].count;continue;}Record continuation;continuation.key=AttributeKey(id,value.first,block);continuation.value.assign(72,0);P32(continuation.value.data(),0x30);for(unsigned n=0;n<8&&i<f.extents.size();++n,++i){P32(continuation.value.data()+8+n*8,f.extents[i].start);P32(continuation.value.data()+12+n*8,f.extents[i].count);block+=f.extents[i].count;}added.push_back(std::move(continuation));}
            }
        }
        attributeRecords.erase(std::remove_if(attributeRecords.begin(),attributeRecords.end(),[&](const Record& r){if(U32(r.key.data()+4)!=id)return false;auto name=AttributeName(r);auto was=old.find(name);auto now=desired.find(name);return now==desired.end()||was==old.end()||was->second!=now->second;}),attributeRecords.end());
        attributeRecords.insert(attributeRecords.end(),added.begin(),added.end());attributeIndexValid=false;for(auto& f:freed)Free(f);AttributeFlags(id);for(auto i=attributeSnapshots.lower_bound({id,{}});i!=attributeSnapshots.end()&&i->first.first==id;)i=attributeSnapshots.erase(i);
    }
    bool IsCompressed(uint32_t id) { return Get(id).compressed; }
    Compression &Compressed(uint32_t id) {
        auto i = compressedReaders.find(id);
        if (i == compressedReaders.end()) {
            auto attribute = Attribute(id, u"com.apple.decmpfs");
            Fork resource = Get(id).forks[1];
            auto reader = std::unique_ptr<Compression>(new Compression(
                Bytes(attribute), resource.size, [this, resource](uint64_t offset, void *p, size_t n) {
                    ForkRead(view, resource, bs, offset, p, n);
                }));
            // Keep one active file's decoded unit, not 64 KiB for every file
            // ever exported from a large catalog.
            compressedReaders.clear();
            i = compressedReaders.emplace(id, std::move(reader)).first;
        }
        return *i->second;
    }
    int LinkKind(uint32_t id) {
        auto &e = Get(id);
        if (e.directory || e.symbolicLink)
            return 0;
        bool file = !memcmp(e.finder.data(), "hlnkhfs+", 8);
        bool folder =
            !memcmp(e.finder.data(), "fdrpMACS", 8) && (U16(CatalogRecord(id)->value.data() + 2) & 0x20);
        if (!file && !folder)
            return 0;
        // Matching Finder signatures alone can also occur on ordinary files.
        // Apple additionally requires the root/private-directory creation date.
        uint32_t metadata=folder?directoryMetadata:fileMetadata;
        bool date = e.created == Get(2).created || (metadata && e.created == Get(metadata).created);
        return date ? (folder ? 2 : 1) : 0;
    }
    uint32_t Resolve(uint32_t id) {
        std::set<uint32_t> seen;
        for (;;) {
            Check(seen.insert(id).second, "Cyclic HFS+ hard-link reference");
            int kind = LinkKind(id);
            if (!kind)
                return id;
            uint32_t reference = U32(CatalogRecord(id)->value.data() + 44);
            uint32_t directory = kind == 2 ? directoryMetadata : fileMetadata;
            Check(reference && directory, "Broken HFS+ hard link: missing reference or metadata directory");
            auto number = std::to_string(reference);
            std::u16string name = kind == 2 ? u"dir_" : u"iNode";
            name.append(number.begin(), number.end());
            uint32_t target = Existing(directory, name);
            Check(target, "Broken HFS+ hard link: backing inode not found");
            auto &e = Get(target);
            // A real backing inode is never another link. Explicitly diagnose
            // cycles before rejecting other malformed link-to-link chains.
            if (LinkKind(target)) {
                Check(!seen.count(target), "Cyclic HFS+ hard-link reference");
                throw Error("Invalid HFS+ hard-link backing inode");
            }
            Check(e.directory == (kind == 2), "HFS+ hard-link target has the wrong type");
            return target;
        }
    }
    // Independent on-disk implementation using TN1150 and the published
    // HFS catalog field definitions. No Apple implementation code is included.
    void ValidateLinkAncestors(uint32_t id){std::set<uint32_t> seen;for(uint32_t parent=Get(id).parent;parent!=1;parent=Get(parent).parent){Check(seen.insert(parent).second,"Cyclic hard-link inode ancestry");if(directoryMetadata&&Get(parent).parent==directoryMetadata)ValidateLinks(parent);}}
    std::vector<uint32_t> Links(uint32_t inode) {
        auto& e=Get(inode);uint32_t metadata=e.directory?directoryMetadata:fileMetadata;
        Check(metadata&&e.parent==metadata,"Invalid hard-link inode location");
        std::u16string prefix=e.directory?u"dir_":u"iNode";Check(e.name.size()>prefix.size()&&e.name.compare(0,prefix.size(),prefix)==0,"Invalid hard-link inode name");
        uint64_t reference=0;for(size_t i=prefix.size();i<e.name.size();++i){Check(e.name[i]>=u'0'&&e.name[i]<=u'9',"Invalid hard-link reference");reference=reference*10+e.name[i]-u'0';Check(reference<=UINT32_MAX,"Hard-link reference overflow");}
        Check(reference&&e.name[prefix.size()]!=u'0',"Invalid hard-link reference");
        std::vector<uint32_t> links;
        for(auto& item:entries)if(LinkKind(item.first)==(e.directory?2:1)&&U32(CatalogRecord(item.first)->value.data()+44)==reference){Check(Resolve(item.first)==inode,"Ambiguous hard-link inode");links.push_back(item.first);}
        return links;
    }
    std::vector<uint32_t> ValidateLinks(uint32_t inode) {
        auto links=Links(inode);Check(!links.empty(),"Orphan hard-link inode");auto& e=Get(inode);auto raw=CatalogRecord(inode);
        bool chained=(U16(raw->value.data()+2)&0x20)!=0;
        Check(!e.directory||chained,"Directory hard-link inode has no link chain");
        if(chained){
            Check(U32(raw->value.data()+44)==links.size(),"Hard-link count does not match catalog links");uint32_t head=0;
            if(e.directory){auto a=GetAttributes(inode);auto i=a.find(u"com.apple.system.hfs.firstlink");Check(i!=a.end()&&!i->second.empty()&&i->second.size()<=11&&i->second.back()==0,"Missing directory first-link attribute");uint64_t n=0;for(size_t j=0;j+1<i->second.size();++j){auto c=i->second[j];Check(c>='0'&&c<='9',"Invalid directory first-link attribute");n=n*10+c-'0';Check(n<=UINT32_MAX,"Directory first-link ID overflow");}head=uint32_t(n);}
            else head=U32(raw->value.data()+4);
            std::set<uint32_t> expected(links.begin(),links.end()),seen;uint32_t previous=0;
            for(uint32_t id=head;id;){Check(expected.count(id)&&seen.insert(id).second,"Broken/cyclic hard-link chain");auto r=CatalogRecord(id);Check((U16(r->value.data()+2)&0x20)&&U32(r->value.data()+32)==previous,"Broken hard-link back pointer");previous=id;id=U32(r->value.data()+36);}
            Check(seen==expected,"Incomplete hard-link chain");
        }else for(auto id:links)Check(!(U16(CatalogRecord(id)->value.data()+2)&0x20),"Mixed legacy/chained hard links");
        for(auto id:links){auto& link=Get(id);Check(!link.forks[0].size&&(!link.forks[1].size||e.directory),"Unexpected hard-link stub contents");Check(GetAttributes(id).empty(),"Unexpected hard-link stub attributes");}
        return links;
    }
    bool DirectoryReaches(uint32_t start,uint32_t destination) {
        std::set<uint32_t> active,done;std::function<bool(uint32_t)> walk=[&](uint32_t id){id=Resolve(id);if(id==destination)return true;if(done.count(id))return false;Check(active.insert(id).second,"Cyclic directory hard-link graph");
            for(auto& child:entries)if(child.second.parent==id&&(child.second.directory||LinkKind(child.first)==2))if(walk(child.first))return true;
            active.erase(id);done.insert(id);return false;};return walk(start);
    }
    uint32_t Content(uint32_t id,bool copiedImmutable=false) {
        ValidateLinkAncestors(id);auto target=Resolve(id);if(target!=id){ValidateLinks(target);Mutable(Get(target),true,false,copiedImmutable);}else Mutable(Get(id),false,false,copiedImmutable);return target;
    }
    uint32_t WriteParent(uint32_t id,bool internal=false) {
        ValidateLinkAncestors(id);auto target=Resolve(id);auto& e=Get(target);Check(e.directory,"Destination is not a folder");
        if(target!=id||(internal&&target!=2&&e.parent==directoryMetadata)){ValidateLinks(target);Mutable(e,true);}
        else Parent(target);return target;
    }
    void SyncLinks(uint32_t inode,const std::vector<uint32_t>& links,bool chained,bool copied=false) {
        auto raw=CatalogRecord(inode);
        if(!copied)P32(raw->value.data()+20,Time::Encode(Now(),(U16(raw->value.data()+2)&0x1000)!=0));
        P32(raw->value.data()+44,uint32_t(links.size()));
        if(!chained)return;
        P16(raw->value.data()+2,U16(raw->value.data()+2)|0x20);
        if(Get(inode).directory){auto desired=GetAttributes(inode);auto number=std::to_string(links.empty()?0:links.front());desired[u"com.apple.system.hfs.firstlink"]=Bytes(number.begin(),number.end());desired[u"com.apple.system.hfs.firstlink"].push_back(0);ReplaceAttributes(inode,desired,true,true);}
        else P32(raw->value.data()+4,links.empty()?0:links.front());
        for(size_t i=0;i<links.size();++i){auto r=CatalogRecord(links[i]);P16(r->value.data()+2,U16(r->value.data()+2)|0x20);P32(r->value.data()+32,i?links[i-1]:0);P32(r->value.data()+36,i+1<links.size()?links[i+1]:0);}
    }
    void Erase(uint32_t id) {
        auto e=Get(id);if(!e.directory)for(unsigned f=0;f<2;++f){Free(e.forks[f]);Overflow(id,f,Fork{});}ReplaceAttributes(id,{});entries.erase(id);
        catalogRecords.erase(std::remove_if(catalogRecords.begin(),catalogRecords.end(),[&](const Record& r){return (U16(r.value.data())<=2&&U32(r.value.data()+8)==id)||(U16(r.value.data())>=3&&U32(r.key.data()+2)==id);}),catalogRecords.end());
    }
    void Unlink(uint32_t id){auto inode=Resolve(id);if(inode==id){Erase(id);return;}auto links=ValidateLinks(inode);bool chained=(U16(CatalogRecord(inode)->value.data()+2)&0x20)!=0;Erase(id);links.erase(std::remove(links.begin(),links.end(),id),links.end());if(links.empty())Erase(inode);else SyncLinks(inode,links,chained);}
    uint32_t Private(bool directory) {
        auto& id=directory?directoryMetadata:fileMetadata;if(id)return id;
        Entry e;e.directory=true;e.parent=2;e.name=directory?u".HFS+ Private Directory Data\r":std::u16string(4,0x2400)+u"HFS+ Private Data";
        Check(!Existing(2,e.name),"An existing private-directory name requires reopening the image");e.finder[8]=0x50;New(e);id=e.id;entries[id].protectedFile=true;return id;
    }
    Fork Alias(uint32_t inode) {
        // Resource Manager map with one standard version-2 directory alias.
        // Target CNID/parent and volume date identify the hidden backing folder.
        Bytes alias(154,0);P16(alias.data()+4,uint16_t(alias.size()));P16(alias.data()+6,2);P16(alias.data()+8,1);
        auto volume=Get(2).name;alias[10]=uint8_t((std::min)(size_t(27),volume.size()));for(unsigned i=0;i<alias[10];++i)alias[11+i]=volume[i]<128?uint8_t(volume[i]):'?';
        P32(alias.data()+38,U32(header.data()+16));P16(alias.data()+42,0x482b);P16(alias.data()+44,5);P32(alias.data()+46,directoryMetadata);
        auto name=Get(inode).name;alias[50]=uint8_t(name.size());for(unsigned i=0;i<alias[50];++i)alias[51+i]=uint8_t(name[i]);P32(alias.data()+114,inode);P32(alias.data()+118,U32(CatalogRecord(inode)->value.data()+12));P16(alias.data()+130,0xffff);P16(alias.data()+132,0xffff);P16(alias.data()+150,0xffff);
        uint32_t map=256+4+uint32_t(alias.size());Bytes resource(map+50,0);P32(resource.data(),256);P32(resource.data()+4,map);P32(resource.data()+8,4+uint32_t(alias.size()));P32(resource.data()+12,50);P32(resource.data()+256,uint32_t(alias.size()));memcpy(resource.data()+260,alias.data(),alias.size());
        auto m=resource.data()+map;memcpy(m,resource.data(),16);P16(m+24,28);P16(m+26,50);P16(m+28,0);memcpy(m+30,"alis",4);P16(m+34,0);P16(m+36,10);P16(m+38,0);P16(m+40,0xffff);
        Fork f=Allocate(resource.size());size_t at=0;for(auto x:f.extents){Bytes block(size_t(x.count)*bs,0);auto n=(std::min)(block.size(),resource.size()-at);memcpy(block.data(),resource.data()+at,n);view.Write(uint64_t(x.start)*bs,block.data(),block.size());at+=n;}return f;
    }
    uint32_t Stub(uint32_t inode,uint32_t parent,const std::u16string& name,uint32_t reference,bool chained) {
        bool directory=Get(inode).directory;Entry e;e.parent=parent;e.name=name;memcpy(e.finder.data(),directory?"fdrpMACS":"hlnkhfs+",8);P16(e.finder.data()+8,directory?0x8000:0x0100);if(directory)e.forks[1]=Alias(inode);New(e);
        auto r=CatalogRecord(e.id);P16(r->value.data()+2,U16(r->value.data()+2)|((chained||directory)?0x20:0));r->value[41]|=2;P16(r->value.data()+42,0100444);P32(r->value.data()+44,reference);
        entries[e.id].posix=PosixMetadata{0,0,0444};
        auto metadata=directory?directoryMetadata:fileMetadata;entries[e.id].created=Get(metadata).created;P32(r->value.data()+12,Time::Encode(entries[e.id].created,(U16(r->value.data()+2)&0x1000)!=0));return e.id;
    }
    Entry Presented(uint32_t id) {
        auto visible = Get(id);
        uint32_t target = Resolve(id);
        Entry result = Get(target);
        result.id = visible.id;
        result.parent = visible.parent;
        result.name = visible.name;
        result.resolvedId = target;
        result.hardLink = target != id;
        const auto* dates=CatalogRecord(target)->value.data();
        const bool expanded=(U16(dates+2)&0x1000)!=0;
        result.additionalDates={Time::Decode(U32(dates+24),expanded),Time::Decode(U32(dates+20),expanded)};
        const auto type=U16(dates+42)&0170000;
        if(!result.directory&&type==0160000)
            result.readError="HFS+/HFSX special files cannot be copied";
        const bool catalogLocked=!result.directory&&(U16(dates+2)&1);
        if(catalogLocked&&result.posix)result.posix->protection|=PosixMetadata::UserImmutable;
        result.protectedFile = catalogLocked||(!result.hardLink&&visible.protectedFile);try{ValidateLinkAncestors(id);}catch(const Error&){result.protectedFile=true;}
        if(result.hardLink){result.linkCount=uint32_t(Links(target).size());try{ValidateLinks(target);}catch(const Error&){result.protectedFile=true;}}
        if (!result.directory && !result.symbolicLink && IsCompressed(target)) {
            auto &c = Compressed(target);
            result.compressed = true;
            result.forks[0].size = c.Size();
            if (c.UsesResource())
                result.forks[1] = Fork{}; // compressed storage is not a user resource fork
        }
        return result;
    }
    void Parent(uint32_t id) {
        ValidateLinkAncestors(id);auto &e = Get(id);
        Check(e.directory, "Destination is not a folder");
        Check(id == 2 || !e.protectedFile || (directoryMetadata&&e.parent==directoryMetadata&&!ValidateLinks(id).empty()), "Cannot add children to protected HFS+ metadata");
        auto r = CatalogRecord(id);
        Check(!(r->value[40] & 2) && !(r->value[41] & 2), "Destination folder is immutable");
    }
    void RemovalParent(uint32_t id) {
        Parent(id);
        auto r = CatalogRecord(id);
        Check(!(r->value[40] & 4) && !(r->value[41] & 4), "Parent folder is append-only");
    }
    uint32_t Existing(uint32_t parent, const std::u16string &name) const {
        const auto key = Key(parent, name);
        const auto position = CatalogPosition(key);
        if (position == catalogRecords.size())
            return 0;
        const auto& record = catalogRecords[position];
        if (U16(record.value.data()) <= 2 && CatalogOrder(record.key, key) == 0)
            return U32(record.value.data() + 8);
        return 0;
    }
    uint32_t ResolveListedFolder(uint32_t parent) {
        const auto target = Resolve(parent);
        Check(Get(target).directory, "Not an HFS+ folder");
        Check(parent != fileMetadata && parent != directoryMetadata,
              "Private HFS+ backing inodes are not browsable");
        return target;
    }
    bool VisibleCatalogEntry(uint32_t id) const {
        return id && id != 2 && id != fileMetadata && id != directoryMetadata;
    }
    Entry ListedEntry(uint32_t id, uint32_t parent) {
        Entry entry;
        try {
            entry = Presented(id);
        } catch (const Error& error) {
            entry = Get(id);
            entry.hardLink = LinkKind(id) != 0;
            entry.readError = error.what();
        }
        entry.parent = parent;
        return entry;
    }
    bool Used(uint32_t n) const { return (bitmap[n / 8] & (0x80 >> (n % 8))) != 0; }
    void AuditAllocation() {
        std::map<uint32_t, const Record *> threads;
        for (auto &record : catalogRecords) {
            if (U16(record.value.data()) < 3)
                continue;
            Check(record.key.size() == 8 && U16(record.key.data() + 6) == 0 && record.value.size() >= 10,
                  "Invalid HFS+ thread record");
            Check(threads.emplace(U32(record.key.data() + 2), &record).second,
                  "Duplicate HFS+ thread record");
        }
        Check(threads.size() == entries.size(), "HFS+ entry/thread counts disagree");
        std::vector<Extent> claims;
        auto claim = [&](const Fork &f) {
            Validate(f);
            for (auto e : f.extents) {
                claims.push_back(e);
                for (uint32_t i = 0; i < e.count; ++i)
                    Check(Used(e.start + i), "HFS+ fork refers to a free block");
            }
        };
        claim(allocation);
        claim(extents);
        claim(catalog);
        for (unsigned off : {352u, 432u}) {
            Fork f = DecodeFork(header.data() + off);
            if (f.blocks) {
                Complete(f, off == 352 ? 8 : 7, 0);
                claim(f);
            }
        }
        LoadAttributes();
        for(const auto& r:attributeRecords)if(U32(r.key.data()+8)==0){auto storage=AttributeStorage(U32(r.key.data()+4),AttributeName(r));if(!storage.inlineValue)claim(storage.fork);}
        uint32_t first = (1536 + bs - 1) / bs, last = (1024 + bs - 1) / bs;
        claims.push_back({0, first});
        claims.push_back({total - last, last});
        for (uint32_t i = 0; i < first; ++i)
            Check(Used(i), "HFS+ boot/header block is free");
        for (uint32_t i = total - last; i < total; ++i)
            Check(Used(i), "HFS+ alternate-header block is free");
        for (auto &pair : entries) {
            const Entry &e = pair.second;
            auto thread = threads.find(e.id);
            Check(thread != threads.end(), "Missing HFS+ thread record");
            auto &value = thread->second->value;
            Check(U16(value.data()) == (e.directory ? 3 : 4) && U32(value.data() + 4) == e.parent &&
                      String(value.data() + 8, value.size() - 8) == e.name,
                  "HFS+ thread does not match its catalog entry");
            if (e.id != 2) {
                auto parent = entries.find(e.parent);
                Check(parent != entries.end() && parent->second.directory,
                      "HFS+ entry has an invalid parent");
                std::set<uint32_t> chain;
                uint32_t id = e.id;
                while (id != 2) {
                    Check(chain.insert(id).second, "HFS+ directory cycle");
                    auto i = entries.find(id);
                    Check(i != entries.end(), "Broken HFS+ parent chain");
                    id = i->second.parent;
                }
            }
            if (!e.directory)
                for (auto &f : e.forks)
                    claim(f);
        }
        std::sort(claims.begin(), claims.end(),
                  [](const Extent &a, const Extent &b) { return a.start < b.start; });
        uint64_t end = 0;
        for (auto e : claims) {
            Check(e.start >= end, "HFS+ forks share allocation blocks");
            end = uint64_t(e.start) + e.count;
        }
        if (journal.enabled) {
            auto covered = [&](uint64_t o, uint64_t n) {
                for (auto e : claims)
                    if (o >= uint64_t(e.start) * bs &&
                        Range(o - uint64_t(e.start) * bs, n, uint64_t(e.count) * bs))
                        return true;
                return false;
            };
            Check((journal.External()||covered(journal.offset, journal.size)) &&
                      covered(uint64_t(U32(header.data() + 12)) * bs, bs),
                  "Journal allocation is not represented by protected files");
        }
    }
    uint32_t CountFreeBlocks() const {
        static constexpr auto setBits = [] {
            std::array<uint8_t, 256> counts{};
            for (size_t n = 1; n < counts.size(); ++n)
                counts[n] = uint8_t(counts[n >> 1] + (n & 1));
            return counts;
        }();
        const uint32_t wholeBytes = total / 8, tail = total % 8;
        uint32_t used = 0;
        for (uint32_t byte = 0; byte < wholeBytes; ++byte)
            used += setBits[bitmap[byte]];
        if (tail)
            used += setBits[bitmap[wholeBytes] & uint8_t(0xff << (8 - tail))];
        return total - used;
    }
    void Mark(uint32_t n, bool used) {
        auto mask = uint8_t(0x80 >> (n % 8));
        auto &bits = bitmap[n / 8];
        if (((bits & mask) != 0) == used)
            return;
        if (used) {
            bits |= mask;
            --freeBlocks;
        } else {
            bits &= uint8_t(~mask);
            ++freeBlocks;
        }
    }
    Fork Allocate(uint64_t size, bool contiguous = false, bool previouslyFree = false) {
        Check(size <= INT64_MAX, "HFS+ fork exceeds Apple's signed 64-bit limit");
        uint64_t count = size / bs + (size % bs != 0);
        Check(count <= total, "HFS+ fork is larger than volume");
        Fork f;
        f.size = size;
        f.blocks = uint32_t(count);
        if (!count)
            return f;
        auto unavailable = [&](uint32_t i) {
            uint8_t mask = uint8_t(0x80 >> (i % 8));
            return Used(i) || (allocationExclusions && ((*allocationExclusions)[i / 8] & mask)) ||
                   (previouslyFree && (committedBitmap[i / 8] & mask));
        };
        if (contiguous) {
            uint32_t run = 0;
            for (uint32_t i = 1; i < total - 1; ++i) {
                run = unavailable(i) ? 0 : run + 1;
                if (run == count) {
                    f.extents.push_back({i - run + 1, run});
                    break;
                }
            }
            Check(!f.extents.empty(), "No contiguous free region for HFS+ metadata expansion");
        } else {
            uint64_t left = count;
            for (uint32_t i = 1; i < total - 1 && left; ++i) {
                if (unavailable(i))
                    continue;
                if (!f.extents.empty() && uint64_t(f.extents.back().start) + f.extents.back().count == i)
                    ++f.extents.back().count;
                else
                    f.extents.push_back({i, 1});
                --left;
            }
            Check(!left, "Not enough free HFS+ blocks");
        }
        for (auto e : f.extents)
            for (uint32_t i = 0; i < e.count; ++i)
                Mark(e.start + i, true);
        return f;
    }
    void Free(const Fork &f) {
        for (auto e : f.extents)
            for (uint32_t i = 0; i < e.count; ++i) {
                Check(Used(e.start + i), "HFS+ allocation bitmap disagrees with fork");
                Mark(e.start + i, false);
            }
    }
    void Overflow(uint32_t id, unsigned which, const Fork &f) {
        overflow.erase(std::remove_if(overflow.begin(), overflow.end(),
                                      [&](const Record &r) {
                                          return U32(r.key.data() + 4) == id &&
                                                 r.key[2] == (which ? 0xff : 0);
                                      }),
                       overflow.end());
        uint32_t logical = 0;
        for (size_t i = 0; i < f.extents.size();) {
            if (i < 8) {
                logical += f.extents[i++].count;
                continue;
            }
            Record r;
            r.key.assign(12, 0);
            P16(r.key.data(), 10);
            r.key[2] = which ? 0xff : 0;
            P32(r.key.data() + 4, id);
            P32(r.key.data() + 8, logical);
            r.value.assign(64, 0);
            for (unsigned n = 0; n < 8 && i < f.extents.size(); ++n, ++i) {
                P32(r.value.data() + n * 8, f.extents[i].start);
                P32(r.value.data() + n * 8 + 4, f.extents[i].count);
                logical += f.extents[i].count;
            }
            overflow.push_back(std::move(r));
        }
    }
    Record *CatalogRecord(uint32_t id) {
        for (auto &r : catalogRecords)
            if (U16(r.value.data()) <= 2 && U32(r.value.data() + 8) == id)
                return &r;
        throw Error("Catalog entry missing");
    }
    void UpdateEntry(const Entry &e) {
        auto r = ChangeCatalogKey(CatalogRecord(e.id), Key(e.parent, e.name));
        auto modified=Now();P32(r->value.data() + 16, Time::Encode(modified,(U16(r->value.data()+2)&0x1000)!=0));entries[e.id].modified=modified;
        memcpy(r->value.data() + 48, e.finder.data(), 32);
        if (!e.directory) {
            EncodeFork(r->value.data() + 88, e.forks[0], bs);
            EncodeFork(r->value.data() + 168, e.forks[1], bs);
        }
    }
    void Thread(const Entry &e) {
        Record thread;
        thread.key = Key(e.id, u"");
        thread.value.assign(10 + e.name.size() * 2, 0);
        P16(thread.value.data(), e.directory ? 3 : 4);
        P32(thread.value.data() + 4, e.parent);
        P16(thread.value.data() + 8, uint16_t(e.name.size()));
        for (size_t i = 0; i < e.name.size(); ++i)
            P16(thread.value.data() + 10 + i * 2, e.name[i]);
        const auto position = CatalogPosition(thread.key);
        if (position < catalogRecords.size() && catalogRecords[position].key == thread.key) {
            Check(U16(catalogRecords[position].value.data()) >= 3, "Expected an HFS+ thread record");
            catalogRecords[position].value = std::move(thread.value);
        } else {
            InsertCatalogRecord(std::move(thread));
        }
    }
    void New(Entry &e) {
        uint64_t next = U32(header.data() + 64);
        Check(next >= 16, "Invalid next HFS+ catalog ID");
        next = (std::max)(next, uint64_t(entries.rbegin()->first) + 1);
        Check(next < 0xffffffff, "HFS+ catalog IDs exhausted");
        uint32_t id = uint32_t(next);
        e.id = id;
        P32(header.data() + 64, id + 1);
        e.created = e.modified = Now();
        Record r;
        r.key = Key(e.parent, e.name);
        r.value.assign(e.directory ? 88 : 248, 0);
        P16(r.value.data(), e.directory ? 1 : 2);
        P16(r.value.data() + 2, (e.directory ? (U16(header.data()) == 0x4858 ? 0x10 : 0) : 2)|((U32(header.data()+4)&0x20000000)?0x1000:0));
        P32(r.value.data() + 8, id);
        for (unsigned i = 12; i <= 28; i += 4)
            P32(r.value.data() + i, Time::Encode(e.created,(U16(r.value.data()+2)&0x1000)!=0));
        const uint16_t permissions=e.directory?0755:e.symbolicLink?0777:0644;
        const uint16_t type=e.directory?0040000:e.symbolicLink?0120000:0100000;
        P16(r.value.data() + 42,type|permissions);
        e.posix=PosixMetadata{0,0,permissions};
        InsertCatalogRecord(std::move(r));
        entries[id] = e;
        UpdateEntry(e);
        Thread(e);
    }
    void Mutable(const Entry &e,bool inodeAccess=false,bool allowSymbolicLink=false,bool copiedImmutable=false,bool allowSpecial=false) {
        ValidateLinkAncestors(e.id);Check((!e.protectedFile||inodeAccess) && e.id >= 16, "Protected HFS+ metadata cannot be modified");
        auto r = CatalogRecord(e.id);
        const unsigned forbidden = copiedImmutable ? 0 : 6;
        Check(!(U16(r->value.data() + 2) & 1) && !(r->value[40] & forbidden) && !(r->value[41] & forbidden),
              "HFS+ entry is locked");
        Check(!e.directory || inodeAccess || !(U16(r->value.data() + 2) & 0x20),
              "Directory hard-link topology cannot be modified");
        if (!e.directory) {
            Check(memcmp(e.finder.data(), "hlnkhfs+", 8) && memcmp(e.finder.data(), "fdrpMACS", 8),
                  "HFS+ hard links cannot be modified");
            const auto type=U16(r->value.data()+42)&0170000;
            Check(type==0100000||(allowSymbolicLink&&e.symbolicLink&&type==0120000)||
                  (allowSpecial&&e.posix&&e.posix->IsSpecial()),
                  "This operation cannot modify the HFS+ entry type");
        }
        if(e.compressed)(void)Compressed(e.id); // Reject missing/malformed compression metadata before mutation.
        // Encryption and hard-link metadata still require their
        // dedicated writers. Ordinary attributes and ACLs are preserved by ID.
        LoadAttributes();
        for(const auto& a:attributeRecords)if(U32(a.key.data()+4)==e.id){auto name=AttributeName(a);
            Check((name!=u"com.apple.decmpfs"||e.compressed)&&name!=u"com.apple.cprotect"&&name!=u"com.apple.system.cprotect"&&(name!=u"com.apple.system.hfs.firstlink"||inodeAccess),"Entry uses unsupported compression/encryption/link metadata");}
    }
    uint64_t GrowMetadata(Fork &f, uint64_t minimum, uint32_t id) {
        uint64_t target = ((minimum + bs - 1) / bs) * bs;
        if (target <= f.size)
            return f.size;
        Fork extra = Allocate(target - uint64_t(f.blocks) * bs, false, true);
        Bytes zeros(65536, 0);
        for (auto e : extra.extents) {
            uint64_t at = uint64_t(e.start) * bs, n = uint64_t(e.count) * bs;
            while (n) {
                size_t part = size_t((std::min)(n, uint64_t(zeros.size())));
                view.Write(at, zeros.data(), part);
                at += part;
                n -= part;
            }
            if (!f.extents.empty() && uint64_t(f.extents.back().start) + f.extents.back().count == e.start)
                f.extents.back().count += e.count;
            else
                f.extents.push_back(e);
        }
        f.blocks += extra.blocks;
        f.size = target;
        Check(id != 3 || f.extents.size() <= 8, "Extents-tree bootstrap requires inline extents");
        Overflow(id, 0, f);
        return f.size;
    }
    void StageForkRange(Fork &f, uint64_t offset, const void *buffer, size_t length) {
        Check(Range(offset, length, f.size), "Metadata write outside fork");
        auto p = static_cast<const uint8_t *>(buffer);
        for (auto e : f.extents) {
            uint64_t bytes = uint64_t(e.count) * bs;
            if (offset >= bytes) {
                offset -= bytes;
                continue;
            }
            size_t n = size_t((std::min)(uint64_t(length), bytes - offset));
            view.Stage(uint64_t(e.start) * bs + offset, p, n);
            offset = 0;
            p += n;
            length -= n;
            if (!length)
                return;
        }
        Check(!length, "Incomplete metadata fork");
    }
    static int ExtentOrder(const Record &a, const Record &b) {
        uint32_t x = U32(a.key.data() + 4), y = U32(b.key.data() + 4);
        if (x != y)
            return x < y ? -1 : 1;
        if (a.key[2] != b.key[2])
            return a.key[2] < b.key[2] ? -1 : 1;
        x = U32(a.key.data() + 8);
        y = U32(b.key.data() + 8);
        return x == y ? 0 : x < y ? -1 : 1;
    }
    template <class Erase, class Set, class Compare>
    static void Difference(const std::vector<Record> &old, const std::vector<Record> &current,
                           Compare compare, Erase erase, Set set) {
        size_t a = 0, b = 0;
        while (a < old.size() || b < current.size()) {
            int order = a == old.size() ? 1 : b == current.size() ? -1 : compare(old[a], current[b]);
            if (order < 0)
                erase(old[a++]);
            else if (order > 0)
                set(current[b++]);
            else {
                if (old[a].value != current[b].value || old[a].key != current[b].key)
                    set(current[b]);
                ++a;
                ++b;
            }
        }
    }
    void UpdateTree(Fork &f, uint32_t id, const std::vector<Record> &old,
                    const std::vector<Record> &records) {
        auto order = [&](const Record &a, const Record &b) {
            return id == 4 ? Order(a, b) : id == 8 ? AttributeOrder(a,b) : ExtentOrder(a, b);
        };
        BTree tree([&](uint64_t o, void *p, size_t n) { ForkRead(view, f, bs, o, p, n); },
                   [&](uint64_t o, const void *p, size_t n) { StageForkRange(f, o, p, n); },
                   [&](uint64_t n) { return GrowMetadata(f, n, id); },
                   [&](const Bytes &a, const Bytes &b) { return order(Record{a, {}}, Record{b, {}}); });
        Difference(
            old, records, order,
            [&](const Record &r) { Check(tree.Erase(r.key), "Missing B-tree record during deletion"); },
            [&](const Record &r) { tree.Set(r); });
        tree.Flush();
    }
    struct Change {
        Record record;
        bool erase;
    };
    template <class Compare>
    static std::vector<Change> Changes(const std::vector<Record> &old, const std::vector<Record> &desired,
                                       Compare compare) {
        std::vector<Change> changes;
        Difference(
            old, desired, compare, [&](const Record &r) { changes.push_back({r, true}); },
            [&](const Record &r) { changes.push_back({r, false}); });
        return changes;
    }
    template <class Compare>
    static std::vector<Record> Merge(const std::vector<Record> &fresh, const std::vector<Change> &changes,
                                     Compare compare) {
        std::vector<Record> result;
        size_t a = 0, b = 0;
        while (a < fresh.size() || b < changes.size()) {
            int order = a == fresh.size()     ? 1
                        : b == changes.size() ? -1
                                              : compare(fresh[a], changes[b].record);
            if (order < 0)
                result.push_back(fresh[a++]);
            else if (order > 0) {
                if (!changes[b].erase)
                    result.push_back(changes[b].record);
                ++b;
            } else {
                if (!changes[b].erase)
                    result.push_back(changes[b].record);
                ++a;
                ++b;
            }
        }
        return result;
    }
    void GrowJournal(uint64_t required, const std::function<void(const char *)> &fault) {
        Check(journal.enabled && journal.start == journal.end, "Cannot enlarge an active journal");
        Check(required < INT64_MAX / 2 && journal.size < INT64_MAX / 2, "No room for a larger journal");
        uint64_t target =
            (std::max)(journal.size * 2, required * 2 + journal.headerSize + journal.listSize + 1024 * 1024);
        target = (target + bs - 1) / bs * bs;
        Check(target < view.length, "Not enough space to enlarge the journal");
        auto catalogChanges = Changes(committedCatalog, catalogRecords,
                                      [&](const Record &a, const Record &b) { return Order(a, b); });
        auto overflowChanges = Changes(committedOverflow, overflow, ExtentOrder);
        auto attributeChanges = Changes(committedAttributes, attributeRecords, AttributeOrder);
        overflowChanges.erase(std::remove_if(overflowChanges.begin(), overflowChanges.end(),
                                             [](const Change &c) {
                                                 auto id = U32(c.record.key.data() + 4);
                                                 return id == 3 || id == 4 || id == 8;
                                             }),
                              overflowChanges.end());
        Bytes desiredBitmap = bitmap;
        auto discardGrowth = [&](const Fork &now, const Fork &old) {
            uint64_t skip = old.blocks;
            for (auto e : now.extents) {
                if (skip >= e.count) {
                    skip -= e.count;
                    continue;
                }
                for (uint32_t n = uint32_t(skip); n < e.count; ++n) {
                    uint32_t block = e.start + n;
                    Check(!(committedBitmap[block / 8] & (0x80 >> (block % 8))),
                          "Metadata growth reused a committed allocation");
                    desiredBitmap[block / 8] &= uint8_t(~(0x80 >> (block % 8)));
                }
                skip = 0;
            }
        };
        discardGrowth(catalog, committedCatalogFork);
        discardGrowth(extents, committedExtentsFork);
        discardGrowth(attributes, committedAttributesFork);
        uint32_t nextId = U32(header.data() + 64);
        {
            Impl preparation(source, true);
            preparation.allocationExclusions = &bitmap;
            uint32_t file = 0;
            for (auto &pair : preparation.entries)
                if (!pair.second.directory) {
                    const auto &fork = pair.second.forks[0];
                    if (fork.extents.size() == 1 && uint64_t(fork.extents[0].start) * bs == journal.offset &&
                        fork.size >= journal.size) {
                        Check(!file, "Ambiguous journal file");
                        file = pair.first;
                    }
                }
            Check(file, "Cannot locate the allocated journal file for enlargement");
            Fork replacement = preparation.Allocate(target, true, true);
            Bytes zeros(65536, 0);
            uint64_t at = uint64_t(replacement.extents[0].start) * bs;
            for (uint64_t n = target; n;) {
                size_t part = size_t((std::min)(n, uint64_t(zeros.size())));
                preparation.view.Write(at, zeros.data(), part);
                at += part;
                n -= part;
            }
            auto &entry = preparation.Get(file);
            Fork old = entry.forks[0];
            entry.forks[0] = replacement;
            preparation.UpdateEntry(entry);
            preparation.Overflow(file, 0, replacement);
            preparation.Free(old);
            auto info = preparation.view.Read(preparation.journal.infoOffset, 180);
            P32(info.data(), U32(info.data()) & ~4u);
            P64(info.data() + 36, uint64_t(replacement.extents[0].start) * bs);
            P64(info.data() + 44, target);
            preparation.view.Stage(preparation.journal.infoOffset, info.data(), info.size());
            Journal mirror(preparation.view);
            mirror.EmptyAt(uint64_t(replacement.extents[0].start) * bs, target, preparation.journal);
            preparation.Commit(fault, &mirror, false);
        }
        // Rebase the still-uncommitted user operation on the independently
        // committed enlargement. New file data was excluded from its allocator.
        Impl fresh(source, true);
        view.patches.clear();
        view.base = fresh.view.base;
        view.length = fresh.view.length;
        catalogRecords = Merge(fresh.catalogRecords, catalogChanges,
                               [&](const Record &a, const Record &b) { return Order(a, b); });
        overflow = Merge(fresh.overflow, overflowChanges, ExtentOrder);
        auto desiredAttributes = Merge(fresh.attributeRecords, attributeChanges, AttributeOrder);
        for (size_t i = 0; i < bitmap.size(); ++i) {
            uint8_t mask = committedBitmap[i] ^ desiredBitmap[i];
            fresh.bitmap[i] = uint8_t((fresh.bitmap[i] & ~mask) | (desiredBitmap[i] & mask));
        }
        header = fresh.header;
        P32(header.data() + 64, (std::max)(nextId, U32(header.data() + 64)));
        bitmap = std::move(fresh.bitmap);
        freeBlocks = CountFreeBlocks();
        allocation = fresh.allocation;
        catalog = fresh.catalog;
        extents = fresh.extents;
        attributes = fresh.attributes;
        journal.Adopt(fresh.journal);
        committedCatalog = std::move(fresh.committedCatalog);
        committedOverflow = std::move(fresh.committedOverflow);
        committedBitmap = std::move(fresh.committedBitmap);
        committedCatalogFork = fresh.committedCatalogFork;
        committedExtentsFork = fresh.committedExtentsFork;
        committedAttributes = fresh.committedAttributes;
        committedAttributesFork = fresh.committedAttributesFork;
        LoadEntries();
        attributeSnapshots.clear();attributeRecords=std::move(desiredAttributes);attributesLoaded=true;attributeIndexValid=false;
        AuditAllocation();
    }
    // Callers validate the request before entering the mutation. Once staging
    // begins, any error invalidates the in-memory state until a fresh mount.
    template<class Stage> auto Mutate(const std::function<void(const char*)>& fault,Stage stage,
                                    Journal* mirror=nullptr) -> decltype(stage()) {
        try {
            if constexpr(std::is_void_v<decltype(stage())>){
                stage();
                Commit(fault,mirror);
            }else{
                auto result=stage();
                Commit(fault,mirror);
                return result;
            }
        }catch(...){
            usable=false;
            throw;
        }
    }

    void Commit(const std::function<void(const char *)> &fault, Journal *mirror = nullptr,
                bool allowGrow = true) {
        Guard();
        std::map<uint32_t, uint32_t> counts, folderCounts;
        std::set<uint32_t> childLinks;
        std::map<uint32_t, std::vector<uint32_t>> directoryParents;
        std::vector<uint32_t> pending;
        uint32_t files = 0, folders = 0;
        for (const auto& item : entries) {
            if (item.first == 2)
                continue;
            const auto& entry = item.second;
            const auto folderLink = LinkKind(item.first) == 2;
            ++counts[entry.parent];
            if (entry.directory || folderLink) {
                ++folders;
                ++folderCounts[entry.parent];
            } else {
                ++files;
            }
            if (folderLink) {
                pending.push_back(entry.parent);
                try { directoryParents[Resolve(item.first)].push_back(entry.parent); }
                catch (const Error&) {}
            }
        }
        while(!pending.empty()){auto parent=pending.back();pending.pop_back();if(parent==1||!childLinks.insert(parent).second)continue;pending.push_back(Get(parent).parent);for(auto visibleParent:directoryParents[parent])pending.push_back(visibleParent);}
        for (auto &record : catalogRecords)
            if (U16(record.value.data()) == 1) {
                auto r = &record;
                uint32_t id = U32(r->value.data() + 8);
                P16(r->value.data()+2,uint16_t((U16(r->value.data()+2)&~0x40u)|(childLinks.count(id)?0x40:0)));
                uint32_t count = counts[id];
                if (U32(r->value.data() + 4) != count) {
                    P32(r->value.data() + 4, count);
                    auto modified=Now();P32(r->value.data() + 16, Time::Encode(modified,(U16(r->value.data()+2)&0x1000)!=0));entries[id].modified=modified;
                }
                if (U16(header.data()) == 0x4858 && (U16(r->value.data() + 2) & 0x10))
                    P32(r->value.data() + 84, folderCounts[id]);
            }
        if(attributesDirty) {
            if(!attributes.size){auto tree=BuildInitialTree({},false,65536,bs);P16(tree.data()+14+20,266);P32(tree.data()+14+38,6);
                attributes=Allocate(tree.size(),false,true);uint64_t logical=0;for(auto e:attributes.extents){auto n=size_t(uint64_t(e.count)*bs);view.Write(uint64_t(e.start)*bs,tree.data()+logical,n);logical+=n;}Overflow(8,0,attributes);}
            std::sort(attributeRecords.begin(),attributeRecords.end(),[](const Record& a,const Record& b){return AttributeOrder(a,b)<0;});
            attributeIndexValid=false;UpdateTree(attributes,8,committedAttributes,attributeRecords);
        }
        UpdateTree(catalog, 4, committedCatalog, catalogRecords);
        std::sort(overflow.begin(), overflow.end(), [](const Record &a, const Record &b) {
            uint32_t ai = U32(a.key.data() + 4), bi = U32(b.key.data() + 4);
            if (ai != bi)
                return ai < bi;
            if (a.key[2] != b.key[2])
                return a.key[2] < b.key[2];
            return U32(a.key.data() + 8) < U32(b.key.data() + 8);
        });
        UpdateTree(extents, 3, committedOverflow, overflow);
        Check(extents.extents.size() <= 8, "Extents tree cannot bootstrap itself");
        for (size_t offset = 0; offset < bitmap.size(); offset += 512) {
            size_t n = (std::min)(size_t(512), bitmap.size() - offset);
            if (memcmp(bitmap.data() + offset, committedBitmap.data() + offset, n))
                StageForkRange(allocation, offset, bitmap.data() + offset, n);
        }
        P32(header.data() + 32, files);
        P32(header.data() + 36, folders);
        P32(header.data() + 48, freeBlocks);
        P32(header.data() + 20, Time::Encode(Now(),(U32(header.data()+4)&0x20000000)!=0));
        P32(header.data() + 68, U32(header.data() + 68) + 1);
        P32(header.data() + 8, journal.enabled ? 0x4846534a : 0x48465658);
        P32(header.data() + 4, U32(header.data() + 4) | 0x100);
        EncodeFork(header.data() + 112, allocation, bs);
        EncodeFork(header.data() + 192, extents, 65536);
        EncodeFork(header.data() + 272, catalog, 65536);
        EncodeFork(header.data() + 352, attributes, 65536);
        view.Stage(1024, header.data(), 512);
        view.Stage(view.length - 1024, header.data(), 512);
        if (mirror)
            journal.CommitReplacement(*mirror, fault);
        else if (!journal.Fits()) {
            Check(allowGrow, "Journal cannot bootstrap enlargement");
            auto needed = journal.RequiredBytes();
            if(journal.External()){
                Check(journal.size<INT64_MAX/2&&needed<INT64_MAX/2,"External journal enlargement exceeds image limits");
                auto target=(std::max)(journal.size*2,needed*2+journal.headerSize+journal.listSize+1024*1024);target=(target+511)/512*512;
                uint64_t where=journal.offset+journal.size;
                if(!Range(where,target,journal.device->Size())){Check(target<=journal.offset,"External journal partition has insufficient space for safe enlargement");where=0;}
                Journal replacement(view);replacement.EmptyAt(where,target,journal);auto info=view.Read(journal.infoOffset,journal.headerSize);P64(info.data()+36,where);P64(info.data()+44,target);view.Stage(journal.infoOffset,info.data(),info.size());journal.CommitReplacement(replacement,fault);compressedReaders.clear();Snapshot();return;
            }
            GrowJournal(needed, fault);
            Commit(fault);
            return;
        } else
            journal.Commit(fault);
        compressedReaders.clear();
        Snapshot();
    }
};
bool Volume::Recognizes(Source &s) {
    return DiskPartitions::FindHfsPlus(s).has_value();
}
Volume::Volume(Source &s, bool w) : p_(new Impl(s, w)) {}
Volume::~Volume() = default;
void Volume::UseExternalJournal(const std::string& uuid){
 auto& p=*p_;p.Guard();Check(p.journal.enabled&&!p.journal.External()&&p.journal.start==p.journal.end,"External journal conversion requires an empty internal journal");Check(uuid.size()==36,"Invalid external journal UUID");
 auto info=p.view.Read(p.journal.infoOffset,p.journal.headerSize);P32(info.data(),2);memset(info.data()+4,0,32);memcpy(info.data()+52,uuid.c_str(),37);P64(info.data()+36,0);
 auto device=p.source.OpenJournal(info);Check(device&&device->Writable()&&device->Size()>=p.journal.size,"External journal is unavailable, read-only or too small");
 Journal replacement(p.view);replacement.EmptyAt(0,p.journal.size,p.journal);replacement.device=device;P64(info.data()+44,replacement.size);
 p.Mutate(fault,[&]{
  // Reclaim only the unique old journal file; the replacement log is already
  // outside this filesystem. Committed blocks remain excluded from allocation.
  uint32_t old=0;for(auto& pair:p.entries){auto& f=pair.second.forks[0];if(!pair.second.directory&&f.extents.size()==1&&uint64_t(f.extents[0].start)*p.bs==p.journal.offset&&f.size>=p.journal.size){Check(!old,"Ambiguous old journal file");old=pair.first;}}
  Check(old>=16,"Cannot identify the old internal journal file");auto entry=p.Get(old);for(unsigned f=0;f<2;++f){p.Free(entry.forks[f]);p.Overflow(old,f,Fork{});}p.ReplaceAttributes(old,{});p.entries.erase(old);p.catalogRecords.erase(std::remove_if(p.catalogRecords.begin(),p.catalogRecords.end(),[&](const Record& r){return (U16(r.value.data())<=2&&U32(r.value.data()+8)==old)||(U16(r.value.data())>=3&&U32(r.key.data()+2)==old);}),p.catalogRecords.end());
  p.view.Stage(p.journal.infoOffset,info.data(),info.size());
 },&replacement);
}
bool Volume::ExternalJournal()const{return p_->journal.External();}
bool Volume::ExpandedTimes() const {return (U32(p_->header.data()+4)&0x20000000)!=0;}
bool Volume::Journaled() const { return p_->journal.enabled; }
bool Volume::IsHfsX() const { return U16(p_->header.data()) == 0x4858; }
bool Volume::CaseSensitive() const { return p_->caseSensitive; }
bool Volume::Writable() const { return p_->write && p_->usable; }
std::u16string Volume::Name() const { return p_->entries.at(2).name; }
uint64_t Volume::FreeBytes() const { return uint64_t(U32(p_->header.data() + 48)) * p_->bs; }
std::vector<Entry> Volume::List(uint32_t parent) const {
    p_->source.CheckMedia();
    Check(p_->usable, "Reopen HFS+ image after error");
    const auto target = p_->ResolveListedFolder(parent);
    const auto end = p_->catalogRecords.end();
    auto record = std::lower_bound(p_->catalogRecords.begin(), end, target,
        [](const Record& entry, uint32_t folder) { return U32(entry.key.data() + 2) < folder; });
    std::vector<Entry> result;
    for (; record != end && U32(record->key.data() + 2) == target; ++record) {
        if (U16(record->value.data()) >= 3)
            continue; // Thread records share the catalog key space.
        const auto id = U32(record->value.data() + 8);
        if (p_->VisibleCatalogEntry(id))
            result.push_back(p_->ListedEntry(id, parent));
    }
    return result;
}
bool Volume::FindChild(uint32_t parent, const std::u16string& name, Entry& found) const {
    p_->source.CheckMedia();
    Check(p_->usable, "Reopen HFS+ image after error");
    const auto target = p_->ResolveListedFolder(parent);
    // A long folded lookup can still match if its extra characters are ignorable.
    if (name.size() > 255) {
        for (const auto& item : p_->entries)
            if (item.second.parent == target && p_->VisibleCatalogEntry(item.first) &&
                !CompareNames(item.second.name, name, p_->caseSensitive)) {
                found = p_->ListedEntry(item.first, parent);
                return true;
            }
        return false;
    }
    const auto id = p_->Existing(target, name);
    if (!p_->VisibleCatalogEntry(id))
        return false;
    found = p_->ListedEntry(id, parent);
    return true;
}

Entry Volume::Stat(uint32_t id) const {
    p_->source.CheckMedia();
    Check(p_->usable, "Reopen HFS+ image after error");
    return p_->Presented(id);
}
void Volume::ValidateSymbolicLinkTarget(const Bytes& target) {
    Check(target.size()>1&&target.size()<=MaxSymbolicLinkPath+1&&target.back()==0&&
          !memchr(target.data(),0,target.size()-1),"Invalid HFS+ symbolic link target");
}
std::optional<Bytes> Volume::SymbolicLinkTarget(uint32_t id) {
    p_->source.CheckMedia();
    Check(p_->usable, "Reopen HFS+ image after error");
    const auto& entry = p_->Get(p_->Resolve(id));
    if (!entry.symbolicLink)
        return {};
    const auto& data = entry.forks[0];
    Check(!entry.compressed && !entry.forks[1].size && data.size && data.size <= MaxSymbolicLinkPath,
          "Invalid HFS+ symbolic link forks");
    // The HFS+ data fork contains the path bytes without a terminator.
    // The shared link transfer expects a NUL-terminated path for APFS.
    Bytes target(size_t(data.size) + 1, 0);
    ForkRead(p_->view, data, p_->bs, 0, target.data(), size_t(data.size));
    ValidateSymbolicLinkTarget(target);
    return target;
}
uint64_t Volume::CompressionResourceSize(uint32_t id) {
    p_->source.CheckMedia();
    Check(p_->usable,"Reopen HFS+ image after error");
    const auto target=p_->Resolve(id);
    return p_->IsCompressed(target)&&p_->Compressed(target).UsesResource()?p_->Get(target).forks[1].size:0;
}
void Volume::ReadCompressionResource(uint32_t id,uint64_t offset,void* buffer,size_t count) {
    Check(CompressionResourceSize(id)!=0,"File has no stored compression resource");
    ForkRead(p_->view,p_->Get(p_->Resolve(id)).forks[1],p_->bs,offset,buffer,count);
}
uint64_t Volume::Read(uint32_t id, unsigned fork, uint64_t offset, void *buffer, size_t n) {
    p_->source.CheckMedia();
    Check(p_->usable, "Reopen HFS+ image after error");
    Check(fork < 2, "Invalid HFS+ fork");
    uint32_t target = p_->Resolve(id);
    auto &e = p_->Get(target);
    Check(!e.directory, "Cannot read a folder fork");
    Check(!e.symbolicLink, "Use the symbolic link target instead of reading file forks");
    Check(!e.posix||!e.posix->IsSpecial(),"A special file has no file forks");
    if (p_->IsCompressed(target)) {
        auto &compressed = p_->Compressed(target);
        if (!fork)
            return compressed.Read(offset, buffer, n);
        if (compressed.UsesResource())
            return 0;
    }
    if (offset >= e.forks[fork].size)
        return 0;
    size_t count = size_t((std::min)(uint64_t(n), e.forks[fork].size - offset));
    ForkRead(p_->view, e.forks[fork], p_->bs, offset, buffer, count);
    return count;
}
uint32_t Volume::MakeDirectory(uint32_t parent, const std::u16string &name, const Attributes *attributes) {
    auto &p = *p_;
    p.Guard();
    parent=p.WriteParent(parent);
    if(attributes)ValidateAttributes(*attributes);
    Entry e;
    e.parent = parent;
    e.name = Normalize(name, p.caseSensitive);
    e.directory = true;
    Check(!p.Existing(parent, e.name), "HFS+ name already exists");
    return p.Mutate(fault,[&]{
        p.New(e);
        if(attributes)p.ReplaceAttributes(e.id,*attributes);
        return e.id;
    });
}
static void SetPosixMetadata(Bytes& record, const PosixMetadata& metadata)
{
    P32(record.data()+32,metadata.uid);
    P32(record.data()+36,metadata.gid);
    record[40]=uint8_t((record[40]&~7)|((metadata.protection&PosixMetadata::SystemImmutable)?2:0)|
                       ((metadata.protection&PosixMetadata::SystemAppendOnly)?4:0)|
                       ((metadata.flags&PosixMetadata::Archived)?1:0));
    record[41]=uint8_t((record[41]&~15)|((metadata.protection&PosixMetadata::UserImmutable)?2:0)|
                       ((metadata.protection&PosixMetadata::UserAppendOnly)?4:0)|
                       ((metadata.flags&PosixMetadata::NoDump)?1:0)|
                       ((metadata.flags&PosixMetadata::Opaque)?8:0));
    const auto type=U16(record.data()+42)&0170000;
    P16(record.data()+42,uint16_t(type|metadata.mode));
}

uint32_t Volume::Put(uint32_t parent, const std::u16string &name, uint64_t dataSize, uint64_t resourceSize,
                     std::function<void(unsigned, uint64_t, void *, size_t)> read,
                     const std::array<uint8_t, 32> *finder, bool overwrite, const std::array<uint32_t,2> *dates,
                     const Attributes *attributes, CompressionMode compression, const std::array<uint64_t,2>*wideDates,
                     std::optional<PosixMetadata> posix,OverwriteScope scope,AdditionalFileDates additionalDates,const Bytes* storedCompression,bool storedResource) {
    return PutFile(parent,name,dataSize,resourceSize,read,finder,overwrite,dates,attributes,compression,wideDates,posix,false,scope,additionalDates,storedCompression,storedResource);
}
void Volume::PutSymbolicLink(uint32_t parent,const std::u16string& name,const Bytes& target,
                             const std::array<uint8_t,32>& finder,bool replace,const Attributes& attributes,
                             const std::array<uint64_t,2>* dates,std::optional<PosixMetadata> posix,AdditionalFileDates additionalDates) {
    ValidateSymbolicLinkTarget(target);
    // APFS and the transfer interface include a terminator. HFS+ stores only
    // the target bytes in its data fork.
    PutFile(parent,name,target.size()-1,0,[&](unsigned,uint64_t at,void* out,size_t size){
        memcpy(out,target.data()+at,size);
    },&finder,replace,nullptr,&attributes,CompressionMode::Uncompressed,dates,posix,true,OverwriteScope::SharedInode,additionalDates,nullptr,false);
}
uint32_t Volume::PutFile(uint32_t parent,const std::u16string& name,uint64_t dataSize,uint64_t resourceSize,
                         const std::function<void(unsigned,uint64_t,void*,size_t)>& read,
                         const std::array<uint8_t,32>* finder,bool overwrite,
                         const std::array<uint32_t,2>* dates,const Attributes* attributes,CompressionMode compression,
                         const std::array<uint64_t,2>* wideDates,std::optional<PosixMetadata> posix,bool symbolicLink,
                         OverwriteScope scope,AdditionalFileDates additionalDates,const Bytes* storedCompression,bool storedResource) {
    auto &p = *p_;
    p.Guard();
    Check(!posix||posix->mode<=PosixMetadata::ModeMask,"Invalid HFS+ POSIX permissions");
    Check(!posix||!posix->IsSpecial()||(!symbolicLink&&!dataSize&&!resourceSize&&
          (compression==CompressionMode::Preserve||compression==CompressionMode::Uncompressed)),
          "Special file copies cannot contain forks");
    parent=p.WriteParent(parent);
    if(attributes)ValidateAttributes(*attributes);
    Entry e;
    e.parent = parent;
    e.name = Normalize(name, p.caseSensitive);
    e.symbolicLink=symbolicLink;
    uint32_t existing = p.Existing(parent, e.name);
    Fork old[2];
    uint32_t visible=existing,detached=0;
    if(scope==OverwriteScope::SelectedName)
        Check(existing&&p.LinkKind(existing)==1,"The HFS+ hard-link destination changed after overwrite approval");
    if(scope==OverwriteScope::SymbolicLink)
        Check(!symbolicLink&&existing&&p.Get(existing).symbolicLink,
              "The HFS+ symbolic link destination changed after overwrite approval");
    if (existing) {
        Check(overwrite, "HFS+ name already exists");
        const bool replaceFileAlias=symbolicLink&&p.LinkKind(existing)==1;
        if((symbolicLink&&!replaceFileAlias)||scope==OverwriteScope::SymbolicLink){
            const auto& target=p.Get(existing);
            Check(!target.directory&&!p.LinkKind(existing),
                  "An HFS+ symbolic link cannot replace a directory");
            Check(!target.symbolicLink||(!target.compressed&&!target.forks[1].size),
                  "Cannot replace an HFS+ symbolic link with unsupported forks");
            p.Mutable(target,false,true);
        }else if(posix&&posix->IsSpecial()){
            const auto& target=p.Get(existing);
            Check(!p.LinkKind(existing)&&target.posix&&target.posix->special==posix->special&&
                  !target.forks[0].size&&!target.forks[1].size,
                  "The HFS+ special file destination changed after overwrite approval");
            p.Mutable(target,false,false,false,true);
        }else{
            existing=p.Content(existing);
        }
        const auto& current=p.Get(existing);
        Check(!current.directory, "Cannot overwrite an HFS+ directory");
        p.RemovalParent(parent);
        if(scope!=OverwriteScope::SharedInode||(symbolicLink&&!current.symbolicLink)){
            // Publish the replacement entry and unlink the old name in the
            // same transaction. Other names of a shared inode stay intact.
            e.name=p.Get(visible).name;
            detached=visible;
            existing=visible=0;
        }else{
            e=current;
            old[0]=e.forks[0];
            old[1]=e.forks[1];
        }
    }
    std::array<uint64_t,2> desiredDates{existing?e.created:Now(),Now()};
    if(dates)desiredDates={(*dates)[0],(*dates)[1]};if(wideDates)desiredDates=*wideDates;
    bool expanded=existing?(U16(p.CatalogRecord(e.id)->value.data()+2)&0x1000)!=0:ExpandedTimes();
    bool convertTimes=false;if(!expanded&&(desiredDates[0]>UINT32_MAX||desiredDates[1]>UINT32_MAX||
        additionalDates.accessed.value_or(0)>UINT32_MAX||additionalDates.changed.value_or(0)>UINT32_MAX)){Check(ExpandedTimes(),"Format this image with expanded timestamps to store dates after February 2040");expanded=convertTimes=true;}
    for(auto date:desiredDates)(void)Time::Encode(date,expanded);
    if(additionalDates.accessed)(void)Time::Encode(*additionalDates.accessed,expanded);
    if(additionalDates.changed)(void)Time::Encode(*additionalDates.changed,expanded);
    if(convertTimes)for(unsigned at=12;at<=28;at+=4){auto raw=U32(p.CatalogRecord(e.id)->value.data()+at);if(raw)(void)Time::Encode(raw,true);}
    auto desired=existing?p.GetAttributes(existing):Attributes{};desired.erase(u"com.apple.decmpfs");
    if(attributes)desired=*attributes;
    std::unique_ptr<EncodedCompression> encoded;
    if(storedCompression){
        Check(compression==CompressionMode::CopyCompressed,"Stored compression requires a compressed copy");
        if(storedResource)ValidateResourceCompression(*storedCompression,dataSize,resourceSize,
            [&](uint64_t offset,void* output,size_t size){read(1,offset,output,size);});
        else ValidateInlineCompression(*storedCompression,dataSize);
        desired[u"com.apple.decmpfs"]=*storedCompression;
    }
    auto method=compression==CompressionMode::CopyCompressed?CompressionMode::Zlib:compression;
    if(method==CompressionMode::Preserve&&e.compressed){
        auto oldAttribute=p.GetAttributes(existing).at(u"com.apple.decmpfs");
        Compression validate(Bytes(oldAttribute),old[1].size,{}); // Validate before inspecting possibly corrupt metadata.
        auto a=oldAttribute.data()+4;uint32_t type=uint32_t(a[0])|uint32_t(a[1])<<8|uint32_t(a[2])<<16|uint32_t(a[3])<<24;
        method=type==3||type==4?CompressionMode::Zlib:type==7||type==8?CompressionMode::Lzvn:type==11||type==12?CompressionMode::Lzfse:type==13||type==14?CompressionMode::Lzbitmap:type==1&&dataSize<=3786?CompressionMode::StoredInline:CompressionMode::Stored;
    }
    if(!storedCompression&&method!=CompressionMode::Preserve&&method!=CompressionMode::Uncompressed){
        encoded.reset(new EncodedCompression(dataSize,[&](uint64_t at,void* output,size_t n){read(0,at,output,n);},method,
            resourceSize?EncodedCompression::Layout::InlineOnly:EncodedCompression::Layout::InlineOrResource));
        if(encoded->Attribute().empty()){
            Check(compression==CompressionMode::Preserve||compression==CompressionMode::CopyCompressed,
                  "Compression needs the resource fork; this file has a genuine resource fork. Decompress it or retain it uncompressed.");
            encoded.reset(); // A transfer must preserve both incoming forks.
        }
    }
    if(encoded)desired[u"com.apple.decmpfs"]=encoded->Attribute();
    const bool compressedOutput=encoded||storedCompression;
    if (finder)
        e.finder = *finder;
    if(symbolicLink)memcpy(e.finder.data(),"slnkrhap",8);
    return p.Mutate(fault,[&]{
        uint64_t sizes[] = {compressedOutput?0:dataSize, encoded&&encoded->UsesResource()?encoded->ResourceSize():resourceSize};
        // Reserve both replacement forks while the old forks are still live.
        // If either allocation fails, the transaction rolls back before writing payload.
        for (unsigned f = 0; f < 2; ++f)
            e.forks[f] = p.Allocate(sizes[f]);
        Bytes buffer(1024 * 1024, 0);
        for (unsigned f = 0; f < 2; ++f) {
            uint64_t logical = 0;
            for (auto extent : e.forks[f].extents) {
                uint64_t bytes = uint64_t(extent.count) * p.bs, physical = uint64_t(extent.start) * p.bs;
                while (bytes) {
                    size_t n = size_t((std::min)(bytes, uint64_t(buffer.size()))),
                           actual = size_t((std::min)(uint64_t(n), sizes[f] - logical));
                    memset(buffer.data(), 0, n);
                    if (actual){
                        if(encoded&&encoded->UsesResource())encoded->ReadResource(logical,buffer.data(),actual);
                        else read(f, logical, buffer.data(), actual);
                    }
                    p.view.Write(physical, buffer.data(), n);
                    physical += n;
                    bytes -= n;
                    logical += actual;
                }
            }
        }
        if (existing) {
            for (auto f : old)
                p.Free(f);
            p.entries[e.id] = e;
            p.UpdateEntry(e);
        } else {
            if(detached)p.Unlink(detached);
            p.New(e);
        }
        auto timeRecord=p.CatalogRecord(e.id);
        if(posix){
            if(posix->IsSpecial()){
                P16(timeRecord->value.data()+42,uint16_t(posix->special)<<12);
                if(IsDevice(posix->special))P32(timeRecord->value.data()+44,posix->device);
            }
            SetPosixMetadata(timeRecord->value,*posix);
            e.posix=posix;
        }
        if(convertTimes){for(unsigned at=12;at<=28;at+=4){auto raw=U32(timeRecord->value.data()+at);P32(timeRecord->value.data()+at,raw?Time::Encode(raw,true):0);}P16(timeRecord->value.data()+2,U16(timeRecord->value.data()+2)|0x1000);}
        e.created=desiredDates[0];e.modified=desiredDates[1];
        P32(timeRecord->value.data()+12,Time::Encode(e.created,expanded));P32(timeRecord->value.data()+16,Time::Encode(e.modified,expanded));
        if(additionalDates.accessed)P32(timeRecord->value.data()+24,Time::Encode(*additionalDates.accessed,expanded));
        if(additionalDates.changed)P32(timeRecord->value.data()+20,Time::Encode(*additionalDates.changed,expanded));
        e.compressed=compressedOutput;p.entries[e.id]=e;
        auto flags=p.CatalogRecord(e.id);flags->value[41]=uint8_t((flags->value[41]&~0x20)|(compressedOutput?0x20:0));
        p.ReplaceAttributes(e.id,desired,true);
        p.Overflow(e.id, 0, e.forks[0]);
        p.Overflow(e.id, 1, e.forks[1]);
        return visible?visible:e.id;
    });
}
void Volume::SetCompression(uint32_t id,bool compressed,std::function<void(uint64_t,uint64_t)> progress){
    auto& p=*p_;p.Guard();Check(!p.Get(p.Content(id)).directory,"Select an ordinary file for compression");
    auto e=Stat(id);Check(e.readError.empty(),"Cannot convert an unreadable compressed file");if(e.compressed==compressed)return;
    SetCompression(id,compressed?CompressionMode::Zlib:CompressionMode::Uncompressed,std::move(progress));
}
void Volume::SetCompression(uint32_t id,CompressionMode method,std::function<void(uint64_t,uint64_t)> progress){
    auto& p=*p_;p.Guard();auto raw=p.Get(p.Content(id));Check(!raw.directory,"Select an ordinary file for compression");
    Check(method!=CompressionMode::Preserve&&method!=CompressionMode::CopyCompressed,
          "Choose a compression method or Uncompressed");
    auto e=Stat(id);Check(e.readError.empty(),"Cannot convert an unreadable compressed file");
    uint64_t done=0,total=e.forks[0].size+e.forks[1].size;std::array<uint64_t,2> dates{e.created,e.modified};
    Put(e.parent,e.name,e.forks[0].size,e.forks[1].size,[&](unsigned fork,uint64_t at,void* output,size_t n){if(progress)progress(done,total);Check(Read(id,fork,at,output,n)==n,"Short file read during compression conversion");done=(std::min)(total,done+uint64_t(n));},&e.finder,true,nullptr,nullptr,method,&dates);
}
void Volume::SetDirectoryMetadata(uint32_t id,const std::array<uint8_t,32>* finder,const std::array<uint64_t,2>* dates,
                                  const Attributes* attributes,std::optional<PosixMetadata> posix,AdditionalFileDates additionalDates){
 auto& p=*p_;p.Guard();id=p.Content(id);auto e=p.Get(id);Check(e.directory,"Select a folder for directory metadata");if(attributes)ValidateAttributes(*attributes);
 Check(!posix||posix->mode<=PosixMetadata::ModeMask,"Invalid HFS+ POSIX permissions");
 auto record=p.CatalogRecord(id);bool expanded=(U16(record->value.data()+2)&0x1000)!=0,convert=false;
 if(!expanded&&((dates&&((*dates)[0]>UINT32_MAX||(*dates)[1]>UINT32_MAX))||
    additionalDates.accessed.value_or(0)>UINT32_MAX||additionalDates.changed.value_or(0)>UINT32_MAX)){
  Check(ExpandedTimes(),"This folder requires expanded timestamps");expanded=convert=true;
 }
 if(dates)for(auto date:*dates)(void)Time::Encode(date,expanded);
 if(additionalDates.accessed)(void)Time::Encode(*additionalDates.accessed,expanded);
 if(additionalDates.changed)(void)Time::Encode(*additionalDates.changed,expanded);
 if(convert)for(unsigned at=12;at<=28;at+=4){auto raw=U32(record->value.data()+at);if(raw)(void)Time::Encode(raw,true);}
 p.Mutate(fault,[&]{
  if(convert){for(unsigned at=12;at<=28;at+=4){auto raw=U32(record->value.data()+at);P32(record->value.data()+at,raw?Time::Encode(raw,true):0);}P16(record->value.data()+2,U16(record->value.data()+2)|0x1000);}
  if(finder){e.finder=*finder;memcpy(record->value.data()+48,e.finder.data(),32);}
  if(posix){SetPosixMetadata(record->value,*posix);e.posix=posix;}
  if(dates){e.created=(*dates)[0];e.modified=(*dates)[1];P32(record->value.data()+12,Time::Encode(e.created,expanded));P32(record->value.data()+16,Time::Encode(e.modified,expanded));}
  if(additionalDates.accessed)P32(record->value.data()+24,Time::Encode(*additionalDates.accessed,expanded));
  if(additionalDates.changed)P32(record->value.data()+20,Time::Encode(*additionalDates.changed,expanded));
  p.entries[id]=e;if(attributes){auto desired=*attributes;auto old=p.GetAttributes(id);if(old.count(u"com.apple.system.hfs.firstlink"))desired[u"com.apple.system.hfs.firstlink"]=old.at(u"com.apple.system.hfs.firstlink");p.ReplaceAttributes(id,desired,true,true);}
 });
}
Attributes Volume::GetAttributes(uint32_t id) const {
    p_->source.CheckMedia();Check(p_->usable,"Reopen HFS+ image after error");auto attributes=p_->GetAttributes(p_->Resolve(id));attributes.erase(u"com.apple.system.hfs.firstlink");return attributes;
}
void Volume::SetAttributes(uint32_t id,const Attributes& attributes) {
    auto& p=*p_;p.Guard();id=p.Content(id);ValidateAttributes(attributes);
    p.Mutate(fault,[&]{
        auto desired=attributes;
        if(p.Get(id).compressed)
            desired[u"com.apple.decmpfs"]=p.Attribute(id,u"com.apple.decmpfs");
        auto old=p.GetAttributes(id);
        if(const auto first=old.find(u"com.apple.system.hfs.firstlink");first!=old.end())
            desired.emplace(first->first,first->second);
        p.ReplaceAttributes(id,desired,true,true);
    });
}
void ValidateAttributes(const Attributes& attributes) {
    for(const auto& item:attributes){const auto& name=item.first;Check(!name.empty()&&name.size()<=127,"HFS+ attribute names must have 1-127 UTF-16 units");
        for(size_t i=0;i<name.size();++i){auto c=name[i];Check(c!=0,"NUL in extended attribute name");if(c>=0xd800&&c<=0xdbff){Check(++i<name.size()&&name[i]>=0xdc00&&name[i]<=0xdfff,"Invalid attribute Unicode");}else Check(c<0xdc00||c>0xdfff,"Invalid attribute Unicode");}
        Check(name!=u"com.apple.FinderInfo"&&name!=u"com.apple.ResourceFork","Finder information and resource forks use their dedicated fields");
        Check(name!=u"com.apple.decmpfs"&&name!=u"com.apple.cprotect"&&name!=u"com.apple.system.cprotect"&&name!=u"com.apple.system.hfs.firstlink"&&name!=u"system.xattrextents","Compression/encryption/link attributes cannot be written by the ordinary attribute editor");

        if(name==u"com.apple.system.Security"){
            constexpr unsigned maxEntries=128,entrySize=24;
            const auto& acl=item.second;
            std::array<uint8_t,44> header{};
            Check(acl.size()>=header.size()&&acl.size()<=header.size()+entrySize*maxEntries,
                  "Invalid Mac ACL length");
            acl.Read(0,header.data(),header.size());
            Check(U32(header.data())==0x012cc16d,"Invalid Mac ACL header (expected disk byte order)");
            const auto count=U32(header.data()+36);
            Check(count==UINT32_MAX||count<=maxEntries,"Mac ACL has too many entries");
            Check(acl.size()==header.size()+(count==UINT32_MAX?0:entrySize*count),
                  "Mac ACL entry count/length mismatch");
        }
    }
}
void Volume::Rename(uint32_t id, uint32_t parent, const std::u16string &name) {
    auto &p = *p_;
    p.Guard();
    parent=p.WriteParent(parent);
    Entry e = p.Get(id);
    if(p.LinkKind(id))p.Content(id);else p.Mutable(e,false,e.symbolicLink);
    p.WriteParent(e.parent,true);p.RemovalParent(e.parent);
    auto normalized = Normalize(name, p.caseSensitive);
    uint32_t existing = p.Existing(parent, normalized);
    Check(!existing || existing == id, "HFS+ name already exists");
    if(p.Get(p.Resolve(id)).directory)Check(!p.DirectoryReaches(id,parent),"Cannot move a folder into itself or a directory hard-link descendant");
    p.Mutate(fault,[&]{
        e.name = normalized;
        e.parent = parent;
        p.entries[id] = e;
        if(p.LinkKind(id)){p.ChangeCatalogKey(p.CatalogRecord(id),Key(e.parent,e.name));auto inode=p.CatalogRecord(p.Resolve(id));P32(inode->value.data()+20,Time::Encode(Now(),(U16(inode->value.data()+2)&0x1000)!=0));}else p.UpdateEntry(e);
        p.Thread(e);
    });
}
uint32_t Volume::MakeHardLink(uint32_t source,uint32_t parent,const std::u16string& name,bool replace,bool copiedImmutable) {
    auto& p=*p_;p.Guard();parent=p.WriteParent(parent);auto normalized=Normalize(name,p.caseSensitive);auto existing=p.Existing(parent,normalized);Check(!existing||replace,"HFS+ name already exists");
    auto visible=p.Get(source);uint32_t inode=p.Content(source,copiedImmutable);auto original=p.Get(inode);Check(inode!=2,"Cannot hard-link the volume root");
    if(original.directory)Check(!p.DirectoryReaches(inode,parent),"Directory hard link would create a cycle");
    if(existing){
        const auto& target=p.Get(existing);
        uint32_t destinationInode=existing;
        if(target.symbolicLink){
            Check(!original.directory&&!target.compressed&&!target.forks[1].size,
                  "Cannot replace this symbolic link with a hard link");
            p.Mutable(target,false,true);
        }else{
            Check(!p.Get(p.Resolve(existing)).directory,"Cannot replace a folder with a hard link");
            destinationInode=p.Content(existing);
        }
        p.RemovalParent(parent);
        if(destinationInode==inode)return existing;
    }
    bool linked=inode!=source;std::vector<uint32_t> links;if(linked)links=p.ValidateLinks(inode);
    Check(!linked||links.size()<32767,"HFS+ supports at most 32767 hard links to an inode");
    bool chained=!linked||(U16(p.CatalogRecord(inode)->value.data()+2)&0x20)!=0;
    return p.Mutate(fault,[&]{
        if(existing)p.Unlink(existing);
        uint32_t metadata=p.Private(original.directory);uint32_t reference=0;
        if(linked)reference=U32(p.CatalogRecord(source)->value.data()+44);
        else {
            reference=original.directory?inode:(std::max)(100u,inode);auto prefix=original.directory?u"dir_":u"iNode";
            for(;;){auto number=std::to_string(reference);std::u16string candidate=prefix;candidate.append(number.begin(),number.end());if(!p.Existing(metadata,candidate)){original.name=candidate;break;}Check(!original.directory&&reference<1073741923,"Hard-link inode name is unavailable");++reference;}
            original.parent=metadata;original.protectedFile=true;p.entries[inode]=original;p.ChangeCatalogKey(p.CatalogRecord(inode),Key(original.parent,original.name));p.Thread(original);
            links.push_back(p.Stub(inode,visible.parent,visible.name,reference,true));
        }
        auto added=p.Stub(inode,parent,normalized,reference,chained);links.push_back(added);p.SyncLinks(inode,links,chained,copiedImmutable);
        return added;
    });
}
void Volume::ValidateCopyTo(uint32_t id,uint32_t parent)const{auto& p=*p_;p.source.CheckMedia();Check(p.usable,"Reopen HFS+ image after error");auto target=p.Resolve(parent);Check(p.Get(target).directory,"Copy destination is not a folder");if(p.Get(p.Resolve(id)).directory)Check(!p.DirectoryReaches(id,target),"Cannot copy a folder into itself or a directory hard-link descendant");}
void Volume::ValidateRemoval(uint32_t id)const {
    auto& p=*p_;p.Guard();auto entry=p.Get(id);if(p.LinkKind(id))p.Content(id);else p.Mutable(entry,false,entry.symbolicLink);p.WriteParent(entry.parent,true);p.RemovalParent(entry.parent);
}
void Volume::Remove(uint32_t id) {
    auto& p=*p_;ValidateRemoval(id);auto e=p.Get(id);bool link=p.LinkKind(id)!=0;uint32_t inode=link?p.Resolve(id):id;auto target=p.Get(inode);std::vector<uint32_t> links;
    if(link)links=p.ValidateLinks(inode);
    if(!link||links.size()==1)for(auto& child:p.entries)Check(child.second.parent!=inode,"HFS+ folder is not empty");
    p.Mutate(fault,[&]{
        if(link){bool chained=(U16(p.CatalogRecord(inode)->value.data()+2)&0x20)!=0;p.Erase(id);links.erase(std::remove(links.begin(),links.end(),id),links.end());if(links.empty())p.Erase(inode);else p.SyncLinks(inode,links,chained);}
        else p.Erase(id);
    });
}
void Volume::Format(Source &source, const std::u16string &name, bool journaled, FormatKind kind, bool expandedTimes) {
    Check(source.Writable(), "Cannot format a read-only image");
    Check(kind == FormatKind::HfsPlus || kind == FormatKind::HfsXCaseSensitive ||
              kind == FormatKind::HfsXCaseInsensitive,
          "Unknown format kind");
    bool hfsx = kind != FormatKind::HfsPlus, sensitive = kind == FormatKind::HfsXCaseSensitive;
    auto normalized = Normalize(name, sensitive);
    uint64_t length = source.Size();
    Check(length >= 8 * 1024 * 1024 && length % 4096 == 0 && length <= INT64_MAX,
          "HFS+ format requires an image of at least 8 MiB, multiple of 4096");
    uint32_t bs = 4096;
    while (length / bs > 0xffffffffULL) {
        Check(bs < 1024 * 1024, "HFS+ image too large");
        bs *= 2;
    }
    Check(length % bs == 0, "Image size must align to allocation blocks");
    uint32_t total = uint32_t(length / bs);
    View view(source);
    Bytes vh(512, 0);
    P16(vh.data(), hfsx ? 0x4858 : 0x482b);
    P16(vh.data() + 2, hfsx ? 5 : 4);
    P32(vh.data() + 4, 0x100 | (journaled ? 0x2000 : 0) | (expandedTimes?0x20000000:0));
    P32(vh.data() + 8, journaled ? 0x4846534a : 0x48465658);
    for (unsigned i = 16; i <= 28; i += 4)
        P32(vh.data() + i, Time::Encode(Now(),expandedTimes));
    P32(vh.data() + 40, bs);
    P32(vh.data() + 44, total);
    P32(vh.data() + 56, bs);
    P32(vh.data() + 60, bs);
    P32(vh.data() + 64, journaled ? 18 : 16);
    P32(vh.data() + 68, 1);
    P64(vh.data() + 72, 1);
    uint32_t next = 1;
    auto reserve = [&](uint64_t n) {
        Fork f;
        f.size = n;
        f.blocks = uint32_t(n / bs + (n % bs != 0));
        if (f.blocks) {
            Check(uint64_t(next) + f.blocks < total - 1, "HFS+ metadata does not fit image");
            f.extents.push_back({next, f.blocks});
            next += f.blocks;
        }
        return f;
    };
    Fork allocation = reserve((uint64_t(total) + 7) / 8), extents = reserve(65536), catalog = reserve(65536),
         jib, journal;
    if (journaled) {
        jib = reserve(bs);
        uint64_t jsize =
            (std::max)(uint64_t(1024 * 1024), (std::min)(uint64_t(8 * 1024 * 1024), length / 32));
        jsize = (jsize + bs - 1) / bs * bs;
        journal = reserve(jsize);
        P32(vh.data() + 12, jib.extents[0].start);
    }
    std::vector<Record> records;
    auto entry = [&](uint32_t id, uint32_t parent, const std::u16string &nm, bool folder, const Fork &f) {
        Record r;
        r.key = Key(parent, nm);
        r.value.assign(folder ? 88 : 248, 0);
        P16(r.value.data(), folder ? 1 : 2);
        P16(r.value.data() + 2, (folder ? (hfsx ? 0x10 : 0) : 2)|(expandedTimes?0x1000:0));
        P32(r.value.data() + 8, id);
        for (unsigned i = 12; i <= 28; i += 4)
            P32(r.value.data() + i, Time::Encode(Now(),expandedTimes));
        P16(r.value.data() + 42, folder ? 0040755 : 0100600);
        if (!folder) {
            EncodeFork(r.value.data() + 88, f, bs);
            P16(r.value.data() + 56, 0x4000);
        }
        records.push_back(r);
        Record t;
        t.key = Key(id, u"");
        t.value.assign(10 + nm.size() * 2, 0);
        P16(t.value.data(), folder ? 3 : 4);
        P32(t.value.data() + 4, parent);
        P16(t.value.data() + 8, uint16_t(nm.size()));
        for (size_t i = 0; i < nm.size(); ++i)
            P16(t.value.data() + 10 + i * 2, nm[i]);
        records.push_back(t);
    };
    entry(2, 1, normalized, true, Fork{});
    if (journaled) {
        entry(16, 2, u".journal_info_block", false, jib);
        entry(17, 2, u".journal", false, journal);
        P32(records[0].value.data() + 4, 2);
        P32(vh.data() + 32, 2);
    }
    std::sort(records.begin(), records.end(), [sensitive](const Record &a, const Record &b) {
        uint32_t x = U32(a.key.data() + 2), y = U32(b.key.data() + 2);
        if (x != y)
            return x < y;
        return CompareNames(String(a.key.data() + 6, a.key.size() - 6),
                            String(b.key.data() + 6, b.key.size() - 6), sensitive) < 0;
    });
    auto tree = BuildInitialTree(records, true, 65536, bs, sensitive);
    catalog.size = tree.size();
    ForkStage(view, catalog, bs, tree);
    tree = BuildInitialTree({}, false, 65536, bs);
    extents.size = tree.size();
    ForkStage(view, extents, bs, tree);
    Bytes bitmap(size_t(allocation.size), 0);
    for (uint32_t i = 0; i < next; ++i)
        bitmap[i / 8] |= uint8_t(0x80 >> (i % 8));
    bitmap[(total - 1) / 8] |= uint8_t(0x80 >> ((total - 1) % 8));
    for (uint64_t i = total; i < uint64_t(bitmap.size()) * 8; ++i)
        bitmap[size_t(i / 8)] |= uint8_t(0x80 >> (i % 8));
    ForkStage(view, allocation, bs, bitmap);
    P32(vh.data() + 48, total - next - 1);
    P32(vh.data() + 52, next);
    EncodeFork(vh.data() + 112, allocation, bs);
    EncodeFork(vh.data() + 192, extents, 65536);
    EncodeFork(vh.data() + 272, catalog, 65536);
    if (journaled) {
        Bytes info(180, 0);
        P32(info.data(), 1);
        P64(info.data() + 36, uint64_t(journal.extents[0].start) * bs);
        P64(info.data() + 44, journal.size);
        view.Stage(uint64_t(jib.extents[0].start) * bs, info.data(), info.size());
        Bytes jh(512, 0);
        P32(jh.data(), 0x4a4e4c78);
        P32(jh.data() + 4, 0x12345678);
        P64(jh.data() + 8, 512);
        P64(jh.data() + 16, 512);
        P64(jh.data() + 24, journal.size);
        P32(jh.data() + 32, 4096);
        P32(jh.data() + 40, 512);
        P32(jh.data() + 36, Sum(jh.data(), 44));
        view.Stage(uint64_t(journal.extents[0].start) * bs, jh.data(), jh.size());
    }
    view.Stage(1024, vh.data(), 512);
    view.Stage(length - 1024, vh.data(), 512);
    view.Apply();
}
} // namespace HfsPlus
