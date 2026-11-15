// SPDX-License-Identifier: GPL-2.0-or-later
#include "HfsPlusDisk.h"
#include <cstdio>
using namespace HfsPlus;
void RunIncrementalBTreeTests();
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
void containers() {
    Memory raw(16 * 1024 * 1024);
    Volume::Format(raw, u"Containers", true);
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
        fails([&] { v.Rename(folder, 2, u"Linked subtree"); },
              "Unsupported directory hard-link topology was changed");
    }
    must(disk.writes == writes, "Rejected hard-link topology edit changed the image");
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
int wmain(int argc, wchar_t **argv) {
    try {
        auto saveMemory = [](Memory &memory, const wchar_t *path) {
            auto out = CreateImage(path, memory.Size());
            out->Write(0, memory.bytes.data(), memory.bytes.size());
            out->Flush();
        };
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
        RunIncrementalBTreeTests();
        journalResizeTests();
        resizeIoErrors();
        {
            auto fixture = largeCatalog();
        }
        basic(false);
        basic(true);
        crashes();
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
