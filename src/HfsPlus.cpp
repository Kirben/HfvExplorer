// SPDX-License-Identifier: GPL-2.0-or-later
// On-disk structures are decoded explicitly, following Apple TN1150. No native
// structs, compiler packing assumptions, or third-party filesystem engine.
#include "HfsPlusDisk.h"
#include "HfsPlusBTree.h"
#include <ctime>
namespace HfsPlus {
namespace {
using namespace Disk;
uint32_t Now() { return uint32_t(uint64_t(time(nullptr)) + 2082844800ULL); }
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
uint64_t Locate(Source &s, uint64_t &length) {
    length = s.Size();
    if (s.Size() < 2048)
        return UINT64_MAX;
    auto vh = Disk::Read(s, 1024, 512);
    uint16_t sig = U16(vh.data());
    if (sig == 0x482b || sig == 0x4858)
        return 0;
    if (sig == 0x4244 && U16(vh.data() + 124) == 0x482b) {
        uint32_t bs = U32(vh.data() + 20);
        uint64_t b = uint64_t(U16(vh.data() + 28)) * 512 + uint64_t(U16(vh.data() + 126)) * bs;
        length = uint64_t(U16(vh.data() + 128)) * bs;
        if (Range(b, length, s.Size()) && length >= 2048) {
            auto embedded = Disk::Read(s, b + 1024, 2);
            Check(U16(embedded.data()) != 0x4858, "HFSX must not be embedded in an HFS wrapper");
            return b;
        }
    }
    auto dd = Disk::Read(s, 0, 512);
    uint32_t step = U16(dd.data()) == 0x4552 ? U16(dd.data() + 2) : 512;
    if (step < 512 || step > 4096 || (step & (step - 1)))
        return UINT64_MAX;
    if (!Range(step, 512, s.Size()))
        return UINT64_MAX;
    auto first = Disk::Read(s, step, 512);
    if (U16(first.data()) != 0x504d)
        return UINT64_MAX;
    uint32_t count = U32(first.data() + 4);
    Check(count && count <= 4096, "Invalid Apple partition count");
    for (uint32_t i = 1; i <= count; ++i) {
        Check(Range(uint64_t(i) * step, 512, s.Size()), "Truncated Apple partition map");
        auto p = Disk::Read(s, uint64_t(i) * step, 512);
        if (U16(p.data()) != 0x504d)
            throw Error("Invalid Apple partition entry");
        if (memcmp(p.data() + 48, "Apple_HFS", 10) && memcmp(p.data() + 48, "Apple_HFSX", 11))
            continue;
        uint64_t base = uint64_t(U32(p.data() + 8)) * step, n = uint64_t(U32(p.data() + 12)) * step;
        if (n < 2048 || !Range(base, n, s.Size()))
            continue;
        auto h = Disk::Read(s, base + 1024, 512);
        if (U16(h.data()) == 0x482b || U16(h.data()) == 0x4858) {
            length = n;
            return base;
        }
    }
    return UINT64_MAX;
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
    uint32_t bs = 0, total = 0;
    bool write = false, usable = true, caseSensitive = false;
    Fork allocation, extents, catalog;
    std::vector<Record> catalogRecords, overflow;
    std::vector<Record> committedCatalog, committedOverflow;
    Bytes committedBitmap;
    Fork committedCatalogFork, committedExtentsFork;
    const Bytes *allocationExclusions = nullptr;
    std::map<uint32_t, Entry> entries;
    explicit Impl(Source &s, bool w) : source(s), view(s), journal(view), write(w) {
        Check(!w || s.Writable(), "Cannot open a read-only image for writing");
        view.base = Locate(s, view.length);
        Check(view.base != UINT64_MAX, "No HFS+ volume found");
        LoadHeader();
        // A relocated journal can be discovered through either committed redo
        // copy. Follow JIB after each replay; read-only mounts use the overlay.
        std::set<uint64_t> journalLocations;
        for (;;) {
            Journal current(view);
            current.Open(header, w);
            journal.Adopt(current);
            if (!journal.enabled)
                break;
            auto info = view.Read(journal.infoOffset, 180);
            uint64_t next = U64(info.data() + 36);
            if (next == journal.offset && U64(info.data() + 44) == journal.size)
                break;
            Check(journalLocations.insert(journal.offset).second, "Cyclic relocated journal chain");
        }
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
        uint8_t compare = 0;
        catalogRecords = ReadTree(view, catalog, bs, 516, &compare);
        caseSensitive = compare == 0xbc;
        Check(compare == 0xcf || compare == 0xbc, "Unknown HFS+ filename comparison");
        Check(U16(header.data()) == 0x4858 || !caseSensitive,
              "Binary filename comparison requires an HFSX volume");
        LoadEntries();
        AuditAllocation();
        Snapshot();
    }
    void Snapshot() {
        committedCatalog = catalogRecords;
        committedOverflow = overflow;
        committedBitmap = bitmap;
        committedCatalogFork = catalog;
        committedExtentsFork = extents;
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
            Check(!(attributes & 0x60000000), "Encrypted or expanded-timestamp HFS+ volumes are unsupported");
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
    int Order(const Record &a, const Record &b) const {
        uint32_t ap = U32(a.key.data() + 2), bp = U32(b.key.data() + 2);
        if (ap != bp)
            return ap < bp ? -1 : 1;
        return CompareNames(String(a.key.data() + 6, a.key.size() - 6),
                            String(b.key.data() + 6, b.key.size() - 6), caseSensitive);
    }
    void LoadEntries() {
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
            e.id = U32(r.value.data() + 8);
            e.created = U32(r.value.data() + 12);
            e.modified = U32(r.value.data() + 16);
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
    void Parent(uint32_t id) {
        auto &e = Get(id);
        Check(e.directory, "Destination is not a folder");
        Check(id == 2 || !e.protectedFile, "Cannot add children to protected HFS+ metadata");
        auto r = CatalogRecord(id);
        Check(!(r->value[40] & 2) && !(r->value[41] & 2), "Destination folder is immutable");
    }
    void RemovalParent(uint32_t id) {
        Parent(id);
        auto r = CatalogRecord(id);
        Check(!(r->value[40] & 4) && !(r->value[41] & 4), "Parent folder is append-only");
    }
    uint32_t Existing(uint32_t parent, const std::u16string &name) const {
        for (auto &pair : entries)
            if (pair.second.parent == parent && !CompareNames(pair.second.name, name, caseSensitive))
                return pair.first;
        return 0;
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
            Check(covered(journal.offset, journal.size) &&
                      covered(uint64_t(U32(header.data() + 12)) * bs, bs),
                  "Journal allocation is not represented by protected files");
        }
    }
    void Mark(uint32_t n, bool used) {
        auto mask = uint8_t(0x80 >> (n % 8));
        if (used)
            bitmap[n / 8] |= mask;
        else
            bitmap[n / 8] &= uint8_t(~mask);
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
        auto r = CatalogRecord(e.id);
        r->key = Key(e.parent, e.name);
        P32(r->value.data() + 16, Now());
        memcpy(r->value.data() + 48, e.finder.data(), 32);
        if (!e.directory) {
            EncodeFork(r->value.data() + 88, e.forks[0], bs);
            EncodeFork(r->value.data() + 168, e.forks[1], bs);
        }
    }
    void Thread(const Entry &e) {
        catalogRecords.erase(std::remove_if(catalogRecords.begin(), catalogRecords.end(),
                                            [&](const Record &r) {
                                                return U32(r.key.data() + 2) == e.id &&
                                                       U16(r.value.data()) >= 3;
                                            }),
                             catalogRecords.end());
        Record thread;
        thread.key = Key(e.id, u"");
        thread.value.assign(10 + e.name.size() * 2, 0);
        P16(thread.value.data(), e.directory ? 3 : 4);
        P32(thread.value.data() + 4, e.parent);
        P16(thread.value.data() + 8, uint16_t(e.name.size()));
        for (size_t i = 0; i < e.name.size(); ++i)
            P16(thread.value.data() + 10 + i * 2, e.name[i]);
        catalogRecords.push_back(std::move(thread));
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
        P16(r.value.data() + 2, e.directory ? (U16(header.data()) == 0x4858 ? 0x10 : 0) : 2);
        P32(r.value.data() + 8, id);
        for (unsigned i = 12; i <= 28; i += 4)
            P32(r.value.data() + i, e.created);
        P16(r.value.data() + 42, e.directory ? 0040755 : 0100644);
        catalogRecords.push_back(std::move(r));
        entries[id] = e;
        UpdateEntry(e);
        Thread(e);
    }
    void Mutable(const Entry &e) {
        Check(!e.protectedFile && e.id >= 16, "Protected HFS+ metadata cannot be modified");
        auto r = CatalogRecord(e.id);
        Check(!(U16(r->value.data() + 2) & 1) && !(r->value[40] & 6) && !(r->value[41] & 6),
              "HFS+ entry is locked");
        Check(!(r->value[41] & 0x20), "Compressed HFS+ files cannot be modified");
        Check(!e.directory || !(U16(r->value.data() + 2) & 0x60),
              "Directory hard-link topology cannot be modified");
        if (!e.directory) {
            Check(memcmp(e.finder.data(), "hlnkhfs+", 8) && memcmp(e.finder.data(), "fdrpMACS", 8),
                  "HFS+ hard links cannot be modified");
            Check((U16(r->value.data() + 42) & 0170000) == 0100000,
                  "Only ordinary HFS+ files can be modified");
        }
        // A catalog-only writer must not discard compression, ACLs or extended
        // attributes whose semantics affect the fork being replaced/deleted.
        Fork attrs = DecodeFork(header.data() + 352);
        if (attrs.size) {
            Complete(attrs, 8, 0);
            auto records = ReadTree(view, attrs, bs, 522);
            for (auto &a : records)
                if (a.key.size() >= 8 && U32(a.key.data() + 4) == e.id)
                    throw Error("Entry has extended attributes; modification is not supported yet");
        }
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
            return id == 4 ? Order(a, b) : ExtentOrder(a, b);
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
        overflowChanges.erase(std::remove_if(overflowChanges.begin(), overflowChanges.end(),
                                             [](const Change &c) {
                                                 auto id = U32(c.record.key.data() + 4);
                                                 return id == 3 || id == 4;
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
        for (size_t i = 0; i < bitmap.size(); ++i) {
            uint8_t mask = committedBitmap[i] ^ desiredBitmap[i];
            fresh.bitmap[i] = uint8_t((fresh.bitmap[i] & ~mask) | (desiredBitmap[i] & mask));
        }
        header = fresh.header;
        P32(header.data() + 64, (std::max)(nextId, U32(header.data() + 64)));
        bitmap = std::move(fresh.bitmap);
        allocation = fresh.allocation;
        catalog = fresh.catalog;
        extents = fresh.extents;
        journal.Adopt(fresh.journal);
        committedCatalog = std::move(fresh.committedCatalog);
        committedOverflow = std::move(fresh.committedOverflow);
        committedBitmap = std::move(fresh.committedBitmap);
        committedCatalogFork = fresh.committedCatalogFork;
        committedExtentsFork = fresh.committedExtentsFork;
        LoadEntries();
        AuditAllocation();
    }
    void Commit(const std::function<void(const char *)> &fault, Journal *mirror = nullptr,
                bool allowGrow = true) {
        Guard();
        std::map<uint32_t, uint32_t> counts, folderCounts;
        for (auto &e : entries)
            if (e.first != 2) {
                ++counts[e.second.parent];
                if (e.second.directory || !memcmp(e.second.finder.data(), "fdrpMACS", 8))
                    ++folderCounts[e.second.parent];
            }
        for (auto &record : catalogRecords)
            if (U16(record.value.data()) == 1) {
                auto r = &record;
                uint32_t id = U32(r->value.data() + 8);
                uint32_t count = counts[id];
                if (U32(r->value.data() + 4) != count) {
                    P32(r->value.data() + 4, count);
                    P32(r->value.data() + 16, Now());
                }
                if (U16(header.data()) == 0x4858 && (U16(r->value.data() + 2) & 0x10))
                    P32(r->value.data() + 84, folderCounts[id]);
            }
        std::sort(catalogRecords.begin(), catalogRecords.end(),
                  [&](const Record &a, const Record &b) { return Order(a, b) < 0; });
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
        uint32_t free = 0, files = 0, folders = 0;
        for (uint32_t i = 0; i < total; ++i)
            if (!Used(i))
                ++free;
        for (auto &p : entries)
            if (p.first != 2) {
                if (p.second.directory)
                    ++folders;
                else
                    ++files;
            }
        P32(header.data() + 32, files);
        P32(header.data() + 36, folders);
        P32(header.data() + 48, free);
        P32(header.data() + 20, Now());
        P32(header.data() + 68, U32(header.data() + 68) + 1);
        P32(header.data() + 8, journal.enabled ? 0x4846534a : 0x48465658);
        P32(header.data() + 4, U32(header.data() + 4) | 0x100);
        EncodeFork(header.data() + 112, allocation, bs);
        EncodeFork(header.data() + 192, extents, 65536);
        EncodeFork(header.data() + 272, catalog, 65536);
        view.Stage(1024, header.data(), 512);
        view.Stage(view.length - 1024, header.data(), 512);
        if (mirror)
            journal.CommitReplacement(*mirror, fault);
        else if (!journal.Fits()) {
            Check(allowGrow, "Journal cannot bootstrap enlargement");
            auto needed = journal.RequiredBytes();
            GrowJournal(needed, fault);
            Commit(fault);
            return;
        } else
            journal.Commit(fault);
        Snapshot();
    }
};
bool Volume::Recognizes(Source &s) {
    uint64_t n;
    return Locate(s, n) != UINT64_MAX;
}
Volume::Volume(Source &s, bool w) : p_(new Impl(s, w)) {}
Volume::~Volume() = default;
bool Volume::Journaled() const { return p_->journal.enabled; }
bool Volume::IsHfsX() const { return U16(p_->header.data()) == 0x4858; }
bool Volume::CaseSensitive() const { return p_->caseSensitive; }
bool Volume::Writable() const { return p_->write && p_->usable; }
std::u16string Volume::Name() const { return p_->entries.at(2).name; }
uint64_t Volume::FreeBytes() const { return uint64_t(U32(p_->header.data() + 48)) * p_->bs; }
std::vector<Entry> Volume::List(uint32_t parent) const {
    Check(p_->usable, "Reopen HFS+ image after error");
    std::vector<Entry> result;
    for (auto &pair : p_->entries)
        if (pair.second.parent == parent && pair.first != 2)
            result.push_back(pair.second);
    std::sort(result.begin(), result.end(), [&](const Entry &a, const Entry &b) {
        return CompareNames(a.name, b.name, p_->caseSensitive) < 0;
    });
    return result;
}
Entry Volume::Stat(uint32_t id) const { return p_->Get(id); }
uint64_t Volume::Read(uint32_t id, unsigned fork, uint64_t offset, void *buffer, size_t n) {
    Check(p_->usable, "Reopen HFS+ image after error");
    Check(fork < 2, "Invalid HFS+ fork");
    auto &e = p_->Get(id);
    Check(!e.directory, "Cannot read a folder fork");
    auto record = p_->CatalogRecord(id);
    Check(!(record->value[41] & 0x20),
          "Compressed HFS+ files require decompression, which is not implemented yet");
    Check(memcmp(e.finder.data(), "hlnkhfs+", 8) && memcmp(e.finder.data(), "fdrpMACS", 8),
          "HFS+ hard-link resolution is not implemented yet");
    if (offset >= e.forks[fork].size)
        return 0;
    size_t count = size_t((std::min)(uint64_t(n), e.forks[fork].size - offset));
    ForkRead(p_->view, e.forks[fork], p_->bs, offset, buffer, count);
    return count;
}
uint32_t Volume::MakeDirectory(uint32_t parent, const std::u16string &name) {
    auto &p = *p_;
    p.Guard();
    p.Parent(parent);
    Entry e;
    e.parent = parent;
    e.name = Normalize(name, p.caseSensitive);
    e.directory = true;
    Check(!p.Existing(parent, e.name), "HFS+ name already exists");
    try {
        p.New(e);
        p.Commit(fault);
        return e.id;
    } catch (...) {
        p.usable = false;
        throw;
    }
}
uint32_t Volume::Put(uint32_t parent, const std::u16string &name, uint64_t dataSize, uint64_t resourceSize,
                     std::function<void(unsigned, uint64_t, void *, size_t)> read,
                     const std::array<uint8_t, 32> *finder, bool overwrite) {
    auto &p = *p_;
    p.Guard();
    p.Parent(parent);
    Entry e;
    e.parent = parent;
    e.name = Normalize(name, p.caseSensitive);
    uint32_t existing = p.Existing(parent, e.name);
    Fork old[2];
    if (existing) {
        Check(overwrite, "HFS+ name already exists");
        e = p.Get(existing);
        Check(!e.directory, "Cannot overwrite an HFS+ directory");
        p.Mutable(e);
        p.RemovalParent(parent);
        old[0] = e.forks[0];
        old[1] = e.forks[1];
    }
    if (finder)
        e.finder = *finder;
    try {
        uint64_t sizes[] = {dataSize, resourceSize};
        Bytes buffer(1024 * 1024, 0);
        for (unsigned f = 0; f < 2; ++f) {
            e.forks[f] = p.Allocate(sizes[f]);
            uint64_t logical = 0;
            for (auto extent : e.forks[f].extents) {
                uint64_t bytes = uint64_t(extent.count) * p.bs, physical = uint64_t(extent.start) * p.bs;
                while (bytes) {
                    size_t n = size_t((std::min)(bytes, uint64_t(buffer.size()))),
                           actual = size_t((std::min)(uint64_t(n), sizes[f] - logical));
                    memset(buffer.data(), 0, n);
                    if (actual)
                        read(f, logical, buffer.data(), actual);
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
        } else
            p.New(e);
        p.Overflow(e.id, 0, e.forks[0]);
        p.Overflow(e.id, 1, e.forks[1]);
        p.Commit(fault);
        return e.id;
    } catch (...) {
        p.usable = false;
        throw;
    }
}
void Volume::Rename(uint32_t id, uint32_t parent, const std::u16string &name) {
    auto &p = *p_;
    p.Guard();
    p.Parent(parent);
    Entry e = p.Get(id);
    p.Mutable(e);
    p.RemovalParent(e.parent);
    auto normalized = Normalize(name, p.caseSensitive);
    uint32_t existing = p.Existing(parent, normalized);
    Check(!existing || existing == id, "HFS+ name already exists");
    for (uint32_t i = parent; i != 1; i = p.Get(i).parent)
        Check(i != id, "Cannot move a folder into itself");
    try {
        e.name = normalized;
        e.parent = parent;
        p.entries[id] = e;
        p.UpdateEntry(e);
        p.Thread(e);
        p.Commit(fault);
    } catch (...) {
        p.usable = false;
        throw;
    }
}
void Volume::Remove(uint32_t id) {
    auto &p = *p_;
    p.Guard();
    Entry e = p.Get(id);
    p.Mutable(e);
    p.RemovalParent(e.parent);
    for (auto &c : p.entries)
        Check(c.second.parent != id, "HFS+ folder is not empty");
    try {
        if (!e.directory)
            for (unsigned f = 0; f < 2; ++f) {
                p.Free(e.forks[f]);
                p.Overflow(id, f, Fork{});
            }
        p.entries.erase(id);
        p.catalogRecords.erase(
            std::remove_if(p.catalogRecords.begin(), p.catalogRecords.end(),
                           [&](const Record &r) {
                               return (U16(r.value.data()) <= 2 && U32(r.value.data() + 8) == id) ||
                                      (U16(r.value.data()) >= 3 && U32(r.key.data() + 2) == id);
                           }),
            p.catalogRecords.end());
        p.Commit(fault);
    } catch (...) {
        p.usable = false;
        throw;
    }
}
void Volume::Format(Source &source, const std::u16string &name, bool journaled, FormatKind kind) {
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
    P32(vh.data() + 4, 0x100 | (journaled ? 0x2000 : 0));
    P32(vh.data() + 8, journaled ? 0x4846534a : 0x48465658);
    for (unsigned i = 16; i <= 28; i += 4)
        P32(vh.data() + i, Now());
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
        P16(r.value.data() + 2, folder ? (hfsx ? 0x10 : 0) : 2);
        P32(r.value.data() + 8, id);
        for (unsigned i = 12; i <= 28; i += 4)
            P32(r.value.data() + i, Now());
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
