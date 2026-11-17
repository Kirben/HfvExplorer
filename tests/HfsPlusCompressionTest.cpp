// SPDX-License-Identifier: GPL-2.0-or-later
#include "HfsPlusBTree.h"
#include "HfsPlusCompression.h"
#include "HfsPlusCompressionFixtures.h"
#include <cstdio>
#include <random>
using namespace HfsPlus;
using namespace HfsPlus::Disk;
namespace {
void Little(uint8_t *p, uint64_t n, unsigned bytes = 4) {
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = uint8_t(n >> (8 * i));
}
Bytes Plain(size_t n) {
    Bytes b(n);
    for (size_t i = 0; i < n; ++i)
        b[i] = uint8_t('A' + (i * 13 + i / 97) % 26);
    return b;
}
template <size_t N> Bytes Vector(const uint8_t (&v)[N]) { return Bytes(v, v + N); }
Bytes Encoded(uint32_t type, unsigned n) {
    if (type == 1)
        return Plain(n);
    if (type == 9 || type == 10) {
        auto b = Plain(n);
        b.insert(b.begin(), 0xcc);
        return b;
    }
    if (n == 8197) {
        if (type == 3)
            return Vector(fixture_z_8197);
        if (type == 7)
            return Vector(fixture_vn_8197);
        if (type == 13)
            return Vector(fixture_zbm_8197);
        return Vector(fixture_fse_8197);
    }
    if (n == 65536) {
        if (type == 4)
            return Vector(fixture_z_65536);
        if (type == 8)
            return Vector(fixture_vn_65536);
        if (type == 14)
            return Vector(fixture_zbm_65536);
        return Vector(fixture_fse_65536);
    }
    Check(n == 12345, "Fixture size");
    if (type == 4)
        return Vector(fixture_z_12345);
    if (type == 8)
        return Vector(fixture_vn_12345);
    if (type == 14)
        return Vector(fixture_zbm_12345);
    return Vector(fixture_fse_12345);
}
Bytes Header(uint32_t type, uint64_t size, const Bytes &payload = {}) {
    Bytes b(16);
    Little(b.data(), 0x636d7066);
    Little(b.data() + 4, type);
    Little(b.data() + 8, size, 8);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
Bytes Resource(uint32_t type, bool storedTail = false, bool nativeFraming = false) {
    Bytes first = Encoded(type, 65536), last = Encoded(type, 12345);
    if (nativeFraming && type == 4)
        first = Vector(fixture_zsync_65536);
    if (nativeFraming && type == 8)
        first.insert(first.end(), 93, 0x81);
    if (storedTail && type != 10) {
        last = Plain(12345);
        if (type == 4 || type == 8)
            last.insert(last.begin(), type == 4 ? 0xff : 6);
        else
            last.insert(last.begin(), 0xff);
    }
    Bytes b;
    if (type == 4) {
        b.resize(280);
        P32(b.data(), 256);
        P32(b.data() + 4, uint32_t(280 + first.size() + last.size()));
        P32(b.data() + 8, uint32_t(24 + first.size() + last.size()));
        P32(b.data() + 256, uint32_t(20 + first.size() + last.size()));
        Little(b.data() + 260, 2);
        Little(b.data() + 264, 20);
        Little(b.data() + 268, first.size());
        Little(b.data() + 272, 20 + first.size());
        Little(b.data() + 276, last.size());
    } else {
        b.resize(12);
        Little(b.data(), 12);
        Little(b.data() + 4, 12 + first.size());
        Little(b.data() + 8, 12 + first.size() + last.size());
    }
    b.insert(b.end(), first.begin(), first.end());
    b.insert(b.end(), last.begin(), last.end());
    return b;
}
template <class F> void Reject(F action, const char *message) {
    try {
        action();
    } catch (const Error &) {
        return;
    }
    throw Error(message);
}
void Codecs() {
    for (uint32_t type : {1u, 3u, 7u, 9u, 11u, 13u, 4u, 8u, 10u, 12u, 14u}) {
        bool resource = type % 2 == 0;
        Bytes expected = Plain(resource ? 65536 : 8197);
        if (resource) {
            auto tail = Plain(12345);
            expected.insert(expected.end(), tail.begin(), tail.end());
        }
        for (bool storedTail : {false, true}) {
            Bytes storage = resource ? Resource(type, storedTail) : Bytes{};
            auto attr = Header(type, expected.size(), resource ? Bytes{} : Encoded(type, 8197));
            uint64_t reads = 0;
            Compression c(attr, storage.size(), [&](uint64_t o, void *p, size_t n) {
                Check(Range(o, n, storage.size()), "Fixture resource read bounds");
                memcpy(p, storage.data() + o, n);
                ++reads;
            });
            Check(c.Size() == expected.size() && c.UsesResource() == resource, "Compression description");
            Bytes actual(expected.size());
            // Cross-block, random and final short reads, plus both EOF cases.
            for (uint64_t o :
                 {uint64_t(0), uint64_t(8000), uint64_t(65530), uint64_t(65536), uint64_t(77777)}) {
                auto count = c.Read(o, actual.data(), actual.size());
                uint64_t want = o >= expected.size() ? 0 : expected.size() - o;
                Check(count == want && (!count || !memcmp(actual.data(), expected.data() + o, size_t(count))),
                      "Compressed random read differs");
            }
            Check(!c.Read(UINT64_MAX, nullptr, 1) && !c.Read(0, nullptr, 0), "Compression EOF/zero read");
            if (resource) {
                c.Read(65536, actual.data(), 1);
                auto before = reads;
                c.Read(65537, actual.data(), 1);
                Check(reads == before, "Decoded-unit cache missed");
            }
            // A complete simulated export must produce the logical bytes.
            for (uint64_t o = 0; o < expected.size();) {
                size_t n = size_t((std::min)(uint64_t(4093), expected.size() - o));
                Check(c.Read(o, actual.data() + o, n) == n, "Compressed export short read");
                o += n;
            }
            Check(actual == expected, "Compressed export bytes");
        }
    }
    for (uint32_t type : {3u, 7u, 11u, 13u}) {
        auto stored = Plain(8197);
        stored.insert(stored.begin(), type == 7 ? 6 : 0xff);
        Compression c(Header(type, 8197, stored), 0, {});
        Bytes out(8197);
        Check(c.Read(0, out.data(), out.size()) == out.size() && out == Plain(8197), "Inline stored marker");
    }
    for (uint32_t type : {4u, 8u}) {
        auto b = Resource(type, false, true);
        Compression c(Header(type, 77881), b.size(),
                      [&](uint64_t o, void *p, size_t n) { memcpy(p, b.data() + o, n); });
        Bytes out(77881), expected = Plain(65536), tail = Plain(12345);
        expected.insert(expected.end(), tail.begin(), tail.end());
        Check(c.Read(0, out.data(), out.size()) == out.size() && out == expected,
              "Native flush-delimited zlib / padded LZVN resource framing");
    }
    for (uint32_t type : {3u, 7u, 11u, 13u}) {
        auto bytes = Encoded(type, 8197);
        for (unsigned cut : {1u, 2u, 8u}) {
            Bytes truncated(bytes.begin(), bytes.end() - cut);
            Compression c(Header(type, 8197, truncated), 0, {});
            Bytes out(8197);
            Reject([&] { c.Read(0, out.data(), out.size()); }, "Truncated codec stream accepted");
        }
        bytes.push_back(0);
        Compression c(Header(type, 8197, bytes), 0, {});
        Bytes out(8197);
        Reject([&] { c.Read(0, out.data(), out.size()); }, "Trailing codec garbage accepted");
        Compression wrong(Header(type, 8196, Encoded(type, 8197)), 0, {});
        Reject([&] { wrong.Read(0, out.data(), out.size()); }, "Oversized codec output accepted");
    }
    for (uint32_t type : {0u, 2u, 5u, 15u, UINT32_MAX})
        Reject([&] { Compression c(Header(type, 100), 0, {}); }, "Unknown compression type accepted");
    Reject([&] { Compression c(Bytes(15), 0, {}); }, "Truncated decmpfs header accepted");
    Reject([&] { Compression c(Header(3, UINT64_MAX), 0, {}); }, "Oversized decoded length accepted");
    for (uint32_t type : {4u, 8u, 10u, 12u, 14u}) {
        auto b = Resource(type);
        if (type == 4)
            Little(b.data() + 264, 0);
        else
            Little(b.data() + 4, 1);
        Compression c(Header(type, 77881), b.size(),
                      [&](uint64_t o, void *p, size_t n) { memcpy(p, b.data() + o, n); });
        Bytes out(77881);
        Reject([&] { c.Read(0, out.data(), out.size()); }, "Malformed resource table accepted");
    }
    // An offset above 2 GiB is table-indexed without allocating the logical file.
    uint64_t logical = (uint64_t(1) << 31) + 77881, count = (logical + 65535) / 65536,
             table = (count + 1) * 4;
    Bytes block = Encoded(8, 65536);
    uint64_t reads = 0;
    Compression large(Header(8, logical), table + block.size(), [&](uint64_t o, void *p, size_t n) {
        ++reads;
        memset(p, 0, n);
        if (o == 0 && n == 4)
            Little(static_cast<uint8_t *>(p), table);
        else if (o == (uint64_t(1) << 31) / 65536 * 4 && n == 8) {
            Little(static_cast<uint8_t *>(p), table);
            Little(static_cast<uint8_t *>(p) + 4, table + block.size());
        } else {
            Check(o == table && n == block.size(), "Unexpected sparse resource read");
            memcpy(p, block.data(), n);
        }
    });
    Bytes out(100);
    Check(large.Read(uint64_t(1) << 31, out.data(), out.size()) == out.size() &&
              !memcmp(out.data(), Plain(100).data(), out.size()) && reads == 3,
          "Large compressed seek");
}
// Directly construct a ZBM stream with a raw chunk followed by a bitmap
// back-reference chunk. History spans the 32 KiB ZBM chunks within a decmpfs
// unit, but must never read before the beginning of that unit.
void LzBitmapEdges() {
    auto append24 = [](Bytes &b, uint32_t n) {
        for (unsigned i = 0; i < 3; ++i)
            b.push_back(uint8_t(n >> (i * 8)));
    };
    Bytes raw{'Z', 'B', 'M', 9};
    auto plain = Plain(65536);
    for (unsigned i = 0; i < 2; ++i) {
        append24(raw, 32774);
        append24(raw, 32768);
        raw.insert(raw.end(), plain.begin() + i * 32768, plain.begin() + (i + 1) * 32768);
    }
    raw.insert(raw.end(), {6, 0, 0, 0, 0, 0});
    Compression stored(Header(13, 65536, raw), 0, {});
    Bytes out(65536);
    Check(stored.Read(0, out.data(), out.size()) == out.size() && out == plain,
          "Raw multichunk LZBITMAP stream");
    Bytes chunk;
    append24(chunk, 33);
    append24(chunk, 16);
    for (unsigned i = 0; i < 3; ++i)
        append24(chunk, 15);
    chunk.push_back(0x33);
    chunk.resize(33, 0);
    Bytes history{'Z', 'B', 'M', 9, 14, 0, 0, 8, 0, 0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    history.insert(history.end(), chunk.begin(), chunk.end());
    history.insert(history.end(), {6, 0, 0, 0, 0, 0});
    Compression repeated(Header(13, 24, history), 0, {});
    out.resize(24);
    Check(repeated.Read(0, out.data(), out.size()) == 24 &&
              std::string(out.begin(), out.end()) == "abcdefghabcdefghabcdefgh",
          "LZBITMAP cross-chunk history");
    Bytes noHistory{'Z', 'B', 'M', 9};
    noHistory.insert(noHistory.end(), chunk.begin(), chunk.end());
    noHistory.insert(noHistory.end(), {6, 0, 0, 0, 0, 0});
    Compression invalid(Header(13, 16, noHistory), 0, {});
    Reject([&] { invalid.Read(0, out.data(), out.size()); }, "LZBITMAP back-reference before unit accepted");
    auto vector = Encoded(13, 8197);
    for (unsigned kind = 0; kind < 7; ++kind) {
        auto b = vector;
        switch (kind) {
        case 0:
            b[0] = 0;
            break;
        case 1:
            b[4] = b[5] = b[6] = 0;
            break;
        case 2:
            b[4] = b[5] = b[6] = 255;
            break;
        case 3:
            b[7] = 1;
            b[8] = 128;
            b[9] = 0;
            break; // 32769 decoded bytes
        case 4:
            b.resize(b.size() - 6);
            break;
        case 5:
            b.insert(b.end(), 1, 0);
            break;
        case 6:
            b[b.size() - 6] = 7;
            break;
        }
        Compression bad(Header(13, 8197, b), 0, {});
        out.resize(8197);
        Reject([&] { bad.Read(0, out.data(), out.size()); }, "Malformed LZBITMAP frame accepted");
    }
    // Exercise decoder metadata as well as the outer framing. Successful
    // mutations need not match the original: this format has no checksum.
    std::mt19937 random(33061);
    unsigned refused = 0;
    for (unsigned i = 0; i < 2048; ++i) {
        auto b = vector;
        size_t offset = 10 + random() % (b.size() - 16);
        b[offset] ^= uint8_t(1u << (random() % 8));
        Compression c(Header(13, 8197, b), 0, {});
        Bytes destination(8199, 0xa5);
        try {
            Check(c.Read(0, destination.data() + 1, 8197) == 8197, "LZBITMAP mutated output length");
        } catch (const Error &) {
            ++refused;
        }
        Check(destination.front() == 0xa5 && destination.back() == 0xa5, "LZBITMAP output canary changed");
    }
    Check(refused > 100, "LZBITMAP corruption paths were not exercised");
}
struct Image : Source {
    Bytes data;
    unsigned writes = 0;
    Image() : data(32 * 1024 * 1024) {}
    uint64_t Size() const override { return data.size(); }
    bool Writable() const override { return true; }
    void Read(uint64_t o, void *p, size_t n) override {
        Check(Range(o, n, data.size()), "Image read");
        memcpy(p, data.data() + o, n);
    }
    void Write(uint64_t o, const void *p, size_t n) override {
        Check(Range(o, n, data.size()), "Image write");
        memcpy(data.data() + o, p, n);
        ++writes;
    }
    void Flush() override {}
    uint8_t *Value(uint32_t id) {
        auto vh = data.data() + 1024;
        auto bs = U32(vh + 40);
        auto at = [&](uint64_t offset) {
            for (unsigned i = 0; i < 8; ++i) {
                uint64_t bytes = uint64_t(U32(vh + 292 + i * 8)) * bs;
                if (offset < bytes)
                    return data.data() + uint64_t(U32(vh + 288 + i * 8)) * bs + offset;
                offset -= bytes;
            }
            throw Error("Fixture catalog offset");
        };
        auto ns = U16(at(0) + 32);
        uint32_t leaf = U32(at(0) + 24);
        while (leaf) {
            auto node = at(uint64_t(leaf) * ns);
            for (unsigned i = 0; i < U16(node + 10); ++i) {
                auto r = node + U16(node + ns - 2 * (i + 1));
                auto value = r + ((U16(r) + 3) & ~1u);
                if (U16(value) <= 2 && U32(value + 8) == id)
                    return value;
            }
            leaf = U32(node);
        }
        throw Error("Fixture catalog ID");
    }
};
uint32_t Put(Volume &v, uint32_t parent, const std::u16string &name, const Bytes &data = {},
             const Bytes &resource = {}, const char *finder = nullptr) {
    std::array<uint8_t, 32> info{};
    if (finder)
        memcpy(info.data(), finder, 8);
    return v.Put(
        parent, name, data.size(), resource.size(),
        [&](unsigned f, uint64_t o, void *p, size_t n) {
            const auto &b = f ? resource : data;
            memcpy(p, b.data() + o, n);
        },
        &info);
}
Bytes AttrKey(uint32_t id, uint32_t block = 0) {
    std::u16string name = u"com.apple.decmpfs";
    Bytes b(14 + name.size() * 2);
    P16(b.data(), uint16_t(b.size() - 2));
    P32(b.data() + 4, id);
    P32(b.data() + 8, block);
    P16(b.data() + 12, uint16_t(name.size()));
    for (size_t i = 0; i < name.size(); ++i)
        P16(b.data() + 14 + i * 2, uint16_t(name[i]));
    return b;
}
void Attributes(Image &image, uint32_t carrier, const std::vector<TreeRecord> &records) {
    auto record = image.Value(carrier);
    Bytes fork(record + 88, record + 168);
    auto vh = image.data.data() + 1024;
    uint32_t bs = U32(vh + 40);
    uint64_t size = U64(fork.data()), base = uint64_t(U32(fork.data() + 16)) * bs;
    Check(U32(fork.data() + 20) * uint64_t(bs) == size, "Attribute fixture contiguous allocation");
    memcpy(vh + 352, fork.data(), 80);
    memcpy(image.data.data() + image.Size() - 1024 + 352, fork.data(), 80);
    memset(record + 88, 0, 80);
    memset(image.data.data() + base, 0, size_t(size));
    auto h = image.data.data() + base;
    unsigned ns = 8192;
    h[8] = 1;
    P16(h + 10, 3);
    P16(h + 32, ns);
    P16(h + 34, 522);
    P32(h + 36, uint32_t(size / ns));
    P32(h + 40, uint32_t(size / ns) - 1);
    P32(h + 52, 6);
    P16(h + ns - 2, 14);
    P16(h + ns - 4, 120);
    P16(h + ns - 6, 248);
    P16(h + ns - 8, ns - 8);
    h[248] = 0x80;
    BTree tree(
        [&](uint64_t o, void *p, size_t n) {
            Check(Range(o, n, size), "Attr tree read");
            memcpy(p, image.data.data() + base + o, n);
        },
        [&](uint64_t o, const void *p, size_t n) {
            Check(Range(o, n, size), "Attr tree write");
            memcpy(image.data.data() + base + o, p, n);
        },
        [&](uint64_t) -> uint64_t { throw Error("Attr fixture too small"); },
        [](const Bytes &a, const Bytes &b) {
            auto ai = U32(a.data() + 4), bi = U32(b.data() + 4);
            if (ai != bi)
                return ai < bi ? -1 : 1;
            auto x = U32(a.data() + 8), y = U32(b.data() + 8);
            return x == y ? 0 : x < y ? -1 : 1;
        });
    for (auto &r : records)
        tree.Set(r);
    tree.Flush();
}
Bytes Inline(const Bytes &attribute) {
    Bytes b(16 + attribute.size() + (attribute.size() % 2), 0);
    P32(b.data(), 0x10);
    P32(b.data() + 12, uint32_t(attribute.size()));
    memcpy(b.data() + 16, attribute.data(), attribute.size());
    return b;
}
Image Filesystem(Volume::FormatKind kind, bool bitmap = false) {
    Image disk;
    Volume::Format(disk, u"Compressed links", true, kind);
    uint32_t data = 0, inlineFile = 0, link = 0, alias = 0, broken = 0, folder = 0, child = 0, carrier = 0,
             metadata = 0, dirMetadata = 0;
    {
        Volume v(disk, true);
        metadata = v.MakeDirectory(2, std::u16string(4, 0x2400) + u"HFS+ Private Data");
        dirMetadata = v.MakeDirectory(2, u".HFS+ Private Directory Data\r");
        data = Put(v, metadata, u"iNode7000", {}, Resource(bitmap ? 14 : 4, true));
        inlineFile = Put(v, 2, u"Inline", {}, Bytes{1, 2, 3});
        link = Put(v, 2, u"Linked", {}, {}, "hlnkhfs+");
        Put(v, 2, u"Linked again", {}, {}, "hlnkhfs+");
        broken = Put(v, 2, u"Broken", {}, {}, "hlnkhfs+");
        folder = v.MakeDirectory(dirMetadata, u"dir_8000");
        child = Put(v, folder, u"Child", Plain(999), Bytes{4, 5});
        alias = Put(v, 2, u"Linked folder", {}, {}, "fdrpMACS");
        carrier = Put(v, 2, u"Attribute storage", Bytes(131072));
    }
    for (auto id : {data, inlineFile})
        disk.Value(id)[41] |= 0x20;
    uint32_t date = U32(disk.Value(2) + 12);
    for (uint32_t id : {link, broken, alias}) {
        P32(disk.Value(id) + 12, date);
        P32(disk.Value(id) + 44, id == alias ? 8000 : id == broken ? 9999 : 7000);
    }
    {
        Volume v(disk);
        for (auto &e : v.List())
            if (e.name == u"Linked again") {
                P32(disk.Value(e.id) + 12, date);
                P32(disk.Value(e.id) + 44, 7000);
            }
    }
    P16(disk.Value(alias) + 2, U16(disk.Value(alias) + 2) | 0x20);
    Attributes(
        disk, carrier,
        {{AttrKey(data), Inline(Header(bitmap ? 14 : 4, 77881))},
         {AttrKey(inlineFile), Inline(Header(bitmap ? 13 : 7, 8197, Encoded(bitmap ? 13 : 7, 8197)))}});
    unsigned before = disk.writes;
    {
        Volume v(disk);
        auto entry = v.Stat(link);
        Check(entry.hardLink && entry.compressed && entry.id == link && entry.resolvedId == data &&
                  entry.name == u"Linked" && entry.forks[0].size == 77881 && !entry.forks[1].size,
              "Resolved compressed hard-link metadata");
        Bytes expected = Plain(65536), tail = Plain(12345);
        expected.insert(expected.end(), tail.begin(), tail.end());
        Bytes actual(expected.size());
        Check(v.Read(link, 0, 0, actual.data(), actual.size()) == actual.size() && actual == expected,
              "Compressed hard-link export");
        for (auto &e : v.List()) {
            Check(e.id != metadata && e.id != dirMetadata, "Private backing directory exposed");
            if (e.name == u"Linked again")
                Check(v.Read(e.id, 0, 65530, actual.data(), 20) == 20 &&
                          !memcmp(actual.data(), expected.data() + 65530, 20),
                      "Shared inode content");
            if (e.id == broken)
                Check(!e.readError.empty(), "Broken link not marked");
        }
        Check(v.Stat(inlineFile).forks[0].size == 8197 && v.Stat(inlineFile).forks[1].size == 3,
              "Inline logical forks");
        actual.resize(8197);
        Check(v.Read(inlineFile, 0, 0, actual.data(), actual.size()) == actual.size() &&
                  actual == Plain(8197),
              "Inline attribute decoding");
        actual.resize(3);
        Check(v.Read(inlineFile, 1, 0, actual.data(), 3) == 3 && actual == Bytes({1, 2, 3}),
              "User resource fork preserved");
        auto dir = v.Stat(alias);
        Check(dir.directory && dir.hardLink && dir.resolvedId == folder, "Directory hard link not resolved");
        auto children = v.List(alias);
        Check(children.size() == 1 && children[0].id == child && children[0].parent == alias,
              "Directory hard-link listing");
        actual.resize(999);
        Check(v.Read(child, 0, 0, actual.data(), actual.size()) == actual.size() && actual == Plain(999),
              "Directory link child content");
        Reject([&] { v.Read(broken, 0, 0, nullptr, 0); }, "Dangling hard link exported");
        Reject([&] { v.List(metadata); }, "Private inode browsing allowed");
    }
    Check(disk.writes == before, "Reading compressed/link content wrote image");
    {
        Volume v(disk, true);
        Reject([&] { v.Remove(link); }, "Hard link removed without link accounting");
        Reject([&] { v.Rename(data, 2, u"Unsafe"); }, "Backing inode moved");
        Image editable=disk;Volume edit(editable,true);edit.Put(2,u"Inline",0,0,[](unsigned,uint64_t,void*,size_t){},nullptr,true);Check(edit.Stat(inlineFile).compressed&&!edit.Stat(inlineFile).forks[0].size,"Compressed overwrite failed");
        Reject([&] { v.MakeDirectory(alias, u"Unsafe"); }, "Directory hard-link write allowed");
        v.MakeDirectory(2, u"Unrelated edit");
    }
    {
        Volume v(disk);
        Bytes b(8197);
        Check(v.Read(inlineFile, 0, 0, b.data(), b.size()) == b.size() && b == Plain(8197),
              "Compression lost after unrelated transaction");
    }
    {
        Image spoof = disk;
        P32(spoof.Value(link) + 12, date + 1);
        Volume v(spoof);
        Check(!v.Stat(link).hardLink && !v.Stat(link).forks[0].size,
              "Finder-signature lookalike incorrectly resolved as hard link");
    }
    {
        Image cycle = disk;
        memcpy(cycle.Value(data) + 48, "hlnkhfs+", 8);
        P32(cycle.Value(data) + 12, date);
        P32(cycle.Value(data) + 44, 7000);
        Volume v(cycle);
        Reject([&] { v.Read(data, 0, 0, nullptr, 0); }, "Self-referencing inode accepted");
        Reject([&] { v.Read(link, 0, 0, nullptr, 0); }, "Link-to-link backing inode accepted");
    }
    {
        Image ordinary = disk;
        P16(ordinary.Value(alias) + 2, U16(ordinary.Value(alias) + 2) & ~0x20u);
        Volume v(ordinary, true);
        Check(!v.Stat(alias).directory && !v.Stat(alias).hardLink,
              "Finder alias incorrectly treated as a directory hard link");
        v.MakeDirectory(2, u"Count check");
        if (kind != Volume::FormatKind::HfsPlus)
            Check(U32(ordinary.Value(2) + 84) == 4, "Ordinary Finder alias counted as a directory hard link");
    }
    // Fork-backed decmpfs attributes, rather than only inline B-tree values.
    Image external;
    Volume::Format(external, u"Fork attributes", false, kind);
    uint32_t file, store, payload;
    {
        Volume v(external, true);
        file = Put(v, 2, u"External");
        store = Put(v, 2, u"Tree", Bytes(131072));
        auto value = Header(11, 8197, Encoded(11, 8197));
        value.resize(9 * 4096);
        payload = Put(v, 2, u"Value", value);
    }
    external.Value(file)[41] |= 0x20;
    Bytes attr(88);
    P32(attr.data(), 0x20);
    memcpy(attr.data() + 8, external.Value(payload) + 88, 80);
    uint32_t first = U32(attr.data() + 24);
    P64(attr.data() + 8, Header(11, 8197, Encoded(11, 8197)).size());
    memset(attr.data() + 24, 0, 64);
    for (unsigned i = 0; i < 8; ++i) {
        P32(attr.data() + 24 + i * 8, first + i);
        P32(attr.data() + 28 + i * 8, 1);
    }
    Bytes continuation(72);
    P32(continuation.data(), 0x30);
    P32(continuation.data() + 8, first + 8);
    P32(continuation.data() + 12, 1);
    memset(external.Value(payload) + 88, 0, 80);
    Attributes(external, store, {{AttrKey(file), attr}, {AttrKey(file, 8), continuation}});
    {
        Volume v(external);
        Bytes b(8197);
        Check(v.Read(file, 0, 0, b.data(), b.size()) == b.size() && b == Plain(8197),
              "Fork-backed attribute read");
    }
    return disk;
}

void VerifyContent(Volume& v,uint32_t id,const Bytes& data,const Bytes& resource={}){
 auto e=v.Stat(id);if(e.forks[0].size!=data.size()||e.forks[1].size!=resource.size())throw Error("Converted logical fork lengths: got "+std::to_string(e.forks[0].size)+"/"+std::to_string(e.forks[1].size)+" expected "+std::to_string(data.size())+"/"+std::to_string(resource.size()));
 for(unsigned f=0;f<2;++f){auto& expected=f?resource:data;Bytes got(expected.size());Check(v.Read(id,f,0,got.data(),got.size())==got.size()&&got==expected,"Converted fork contents");if(expected.size()>65536){Bytes cross((std::min)(size_t(33),expected.size()-65520));Check(v.Read(id,f,65520,cross.data(),cross.size())==cross.size()&&!memcmp(cross.data(),expected.data()+65520,cross.size()),"Converted cross-unit random read");}}
}
void WritableFilesystem(Volume::FormatKind kind,bool journaled){
 Image disk;Volume::Format(disk,u"Compressed writer",journaled,kind);uint32_t id=0;
 HfsPlus::Attributes attrs{{u"user.keep",Bytes{1,2,3}}};
 for(unsigned n:{0u,1u,3786u,65536u,65537u,77881u,200000u}){
  auto data=Plain(n);std::array<uint8_t,32> finder{};finder[0]='T';std::array<uint32_t,2> dates{3000000000,3000000123};
  {Volume v(disk,true);id=v.Put(2,u"File",data.size(),0,[&](unsigned,uint64_t at,void* p,size_t bytes){memcpy(p,data.data()+at,bytes);},&finder,id!=0,&dates,&attrs,Volume::CompressionMode::Uncompressed);v.SetCompression(id,true);VerifyContent(v,id,data);Check(v.Stat(id).compressed&&v.GetAttributes(id).at(u"user.keep")==Bytes({1,2,3})&&v.Stat(id).finder==finder&&v.Stat(id).created==dates[0]&&v.Stat(id).modified==dates[1],"Compression lost metadata");}
  {Volume v(disk,true);VerifyContent(v,id,data);auto before=v.FreeBytes();v.SetCompression(id,false);Check(!v.Stat(id).compressed&&!v.GetAttributes(id).count(u"com.apple.decmpfs"),"Decompression left flags/attribute");VerifyContent(v,id,data);v.SetCompression(id,true);auto edited=Plain(n+123);v.Put(2,u"File",edited.size(),0,[&](unsigned,uint64_t at,void* p,size_t bytes){memcpy(p,edited.data()+at,bytes);},nullptr,true);Check(v.Stat(id).compressed,"Overwrite did not retain compression");VerifyContent(v,id,edited);v.Rename(id,2,u"Renamed");v.SetAttributes(id,{{u"user.changed",Bytes{9}}});Check(v.Stat(id).compressed&&v.GetAttributes(id).count(u"com.apple.decmpfs"),"Metadata edit removed compressed storage");v.Remove(id);id=0;}
 }
 // Incompressible units use stored markers; a real resource fork is never reused.
 Bytes random(150000);std::mt19937 rng(7123);for(auto& b:random)b=uint8_t(rng());Bytes resource{7,8,9};
 {Volume v(disk,true);id=Put(v,2,u"Random",random);v.SetCompression(id,true);VerifyContent(v,id,random);Check(v.Stat(id).compressed,"Incompressible resource encoding");v.Put(2,u"Random",random.size(),resource.size(),[&](unsigned f,uint64_t at,void* p,size_t n){memcpy(p,(f?resource:random).data()+at,n);},nullptr,true);Check(!v.Stat(id).compressed,"Conflicting genuine resource fork was lost");VerifyContent(v,id,random,resource);unsigned writes=disk.writes;Reject([&]{v.SetCompression(id,true);},"Compression stole a genuine resource fork");Check(disk.writes==writes,"Resource conflict wrote image");VerifyContent(v,id,random,resource);}
 {Volume v(disk,true);auto small=Plain(8197);id=v.Put(2,u"Inline resource",small.size(),resource.size(),[&](unsigned f,uint64_t at,void* p,size_t n){memcpy(p,(f?resource:small).data()+at,n);});v.SetCompression(id,true);VerifyContent(v,id,small,resource);v.SetCompression(id,false);VerifyContent(v,id,small,resource);}
 // Cancellation/source errors happen before metadata publication.
 {Volume v(disk,true);auto data=Plain(150000);id=Put(v,2,u"Cancel",data);unsigned calls=0;Reject([&]{v.SetCompression(id,true,[&](uint64_t,uint64_t){if(++calls==2)throw Error("Cancel fixture");});},"Compression cancellation ignored");}
 {Volume v(disk);Check(!v.Stat(id).compressed,"Cancelled conversion published flags");VerifyContent(v,id,Plain(150000));}
 // Reported partial payload writes and flush failures must retain the old file.
 struct FaultImage:Image{bool partial=false,flush=false;void Write(uint64_t at,const void* p,size_t n)override{if(partial){partial=false;Image::Write(at,p,n/2);throw Error("Partial compressed payload write");}Image::Write(at,p,n);}void Flush()override{if(flush){flush=false;throw Error("Compressed payload flush failure");}}};
 for(unsigned action=0;action<3;++action)for(bool partial:{false,true}){FaultImage failed;static_cast<Image&>(failed)=disk;if(action){Volume v(failed,true);v.SetCompression(id,true);}failed.partial=partial;failed.flush=!partial;
  {Volume v(failed,true);Reject([&]{if(action==2){auto next=Plain(170123);v.Put(2,u"Cancel",next.size(),0,[&](unsigned,uint64_t at,void* p,size_t n){memcpy(p,next.data()+at,n);},nullptr,true);}else v.SetCompression(id,action==0);},"Compression I/O failure ignored");}
  failed.partial=failed.flush=false;Volume recovered(failed);VerifyContent(recovered,id,Plain(150000));Check(recovered.Stat(id).compressed==(action!=0),"Failed conversion published partial state");
 }
 if(journaled){
  Image baseline=disk;
  for(unsigned action=0;action<3;++action){Image start=baseline;if(action){Volume v(start,true);v.SetCompression(id,true);}
   for(const char* phase:{"data-flushed","journal-data-flushed","journal-committed","home-flushed","journal-cleared"}){Image failed=start;{Volume v(failed,true);v.fault=[&](const char* at){if(!strcmp(at,phase))throw Error("Compressed journal interruption");};Reject([&]{if(action==2){auto next=Plain(170123);v.Put(2,u"Cancel",next.size(),0,[&](unsigned,uint64_t at,void* p,size_t n){memcpy(p,next.data()+at,n);},nullptr,true);}else v.SetCompression(id,action==0);},"Missing compression fault boundary");}
    unsigned writes=failed.writes;Volume recovered(failed,false);auto e=recovered.Stat(id);VerifyContent(recovered,id,Plain(action==2&&e.forks[0].size==170123?170123:150000));Check(failed.writes==writes,"Read-only compression replay wrote source");auto a=recovered.GetAttributes(id);Check(e.compressed==bool(a.count(u"com.apple.decmpfs")),"Recovered mixed compression metadata");}
  }
 }
}

void EditNativeCodecs(){
 for(uint32_t type:{1u,3u,4u,7u,8u,9u,10u,11u,12u,13u,14u}){
  Image disk;Volume::Format(disk,u"Native compressed edit",true);bool resource=type%2==0;unsigned inlineSize=type==1||type==9?1024:8197;auto expected=Plain(resource?65536:inlineSize);if(resource){auto last=Plain(12345);expected.insert(expected.end(),last.begin(),last.end());}
  uint32_t file,carrier;{Volume v(disk,true);file=Put(v,2,u"Native",{},resource?Resource(type):Bytes{});carrier=Put(v,2,u"Attribute carrier",Bytes(65536));}
  disk.Value(file)[41]|=0x20;Attributes(disk,carrier,{{AttrKey(file),Inline(Header(type,expected.size(),resource?Bytes{}:Encoded(type,inlineSize)))}});
  {Volume v(disk,true);v.SetCompression(file,false);VerifyContent(v,file,expected);v.SetCompression(file,true);VerifyContent(v,file,expected);auto next=Plain(90000);v.Put(2,u"Native",next.size(),0,[&](unsigned,uint64_t at,void* p,size_t n){memcpy(p,next.data()+at,n);},nullptr,true);VerifyContent(v,file,next);}
  {Volume v(disk);Check(v.Stat(file).compressed,"Edited native codec not compressed");VerifyContent(v,file,Plain(90000));}
 }
}
void WriterCodecTests(){
 // Check emitted framing independently of the filesystem reader.
 Bytes plain=Plain(160000);ZlibCompression plan(plain.size(),[&](uint64_t at,void* p,size_t n){memcpy(p,plain.data()+at,n);});Check(plan.UsesResource(),"Expected type-4 encoding");Bytes encoded(size_t(plan.ResourceSize()));plan.ReadResource(0,encoded.data(),encoded.size());auto map=U32(encoded.data()+4);Check(U32(encoded.data())==256&&U32(encoded.data()+8)==map-256&&U32(encoded.data()+12)==50&&map+50==encoded.size()&&!memcmp(encoded.data()+map+30,"cmpf",4)&&U16(encoded.data()+map+38)==1,"Invalid emitted resource map");
 Compression decoded(plan.Attribute(),encoded.size(),[&](uint64_t at,void* p,size_t n){memcpy(p,encoded.data()+at,n);});Bytes got(plain.size());Check(decoded.Read(0,got.data(),got.size())==got.size()&&got==plain,"Emitted type-4 decoding");
 bool change=false;ZlibCompression changed(plain.size(),[&](uint64_t at,void* p,size_t n){memcpy(p,plain.data()+at,n);if(change)static_cast<uint8_t*>(p)[0]^=1;});change=true;Reject([&]{changed.ReadResource(0,encoded.data(),encoded.size());},"Changing compression input accepted");
}
} // namespace
void RunHfsPlusCompressionTests() {
    WriterCodecTests();
    EditNativeCodecs();
    for(auto kind:{Volume::FormatKind::HfsPlus,Volume::FormatKind::HfsXCaseSensitive,Volume::FormatKind::HfsXCaseInsensitive})for(bool journaled:{false,true})WritableFilesystem(kind,journaled);
    puts("PASS: compressed creation/edit/decompression, metadata/real resource preservation, stored units, cancellation, source changes and journal interruption recovery in HFS+ and both HFSX modes");
    Codecs();
    LzBitmapEdges();
    for (auto kind : {Volume::FormatKind::HfsPlus, Volume::FormatKind::HfsXCaseSensitive,
                      Volume::FormatKind::HfsXCaseInsensitive})
        for (bool bitmap : {false, true})
            Filesystem(kind, bitmap);
    puts("PASS: decmpfs stored/zlib/LZVN/LZFSE/LZBITMAP inline and resource decoding, random reads above 2 "
         "GiB, "
         "corruption rejection, logical fork sizes, file/directory hard-link resolution, shared compressed "
         "inodes, dangling links, private inode protection and unrelated writes in HFS+ and both HFSX modes");
}

void CreateHfsPlusFeatureFixture(const wchar_t *path) {
    auto memory = Filesystem(Volume::FormatKind::HfsPlus);
    auto out = CreateImage(path, memory.Size());
    out->Write(0, memory.data.data(), memory.data.size());
    out->Flush();
}

void CreateHfsPlusCompressionEncodingFixture(const wchar_t* prefix){
 for(unsigned mode=0;mode<3;++mode){Bytes plain=Plain(mode?180001:8197);if(mode==2){std::mt19937 rng(56123);for(auto& b:plain)b=uint8_t(rng());}ZlibCompression encoder(plain.size(),[&](uint64_t at,void* p,size_t n){memcpy(p,plain.data()+at,n);});auto save=[&](const wchar_t* suffix,const Bytes& bytes){auto path=std::wstring(prefix)+std::to_wstring(mode)+suffix;FILE* out=_wfopen(path.c_str(),L"wb");Check(out!=nullptr,"Create encoder fixture");bool ok=fwrite(bytes.data(),1,bytes.size(),out)==bytes.size();fclose(out);Check(ok,"Write encoder fixture");};save(L".raw",plain);save(L".attr",encoder.Attribute());Bytes resource(size_t(encoder.ResourceSize()));if(!resource.empty())encoder.ReadResource(0,resource.data(),resource.size());save(L".rsrc",resource);}
}
