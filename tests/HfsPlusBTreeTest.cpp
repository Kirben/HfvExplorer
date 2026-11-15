// SPDX-License-Identifier: GPL-2.0-or-later
#include "HfsPlusBTree.h"
#include <cstdio>
#include <random>
using namespace HfsPlus;
using namespace HfsPlus::Disk;
namespace {
struct File {
    Bytes bytes;
    uint64_t written = 0;
    unsigned grows = 0;
    explicit File(unsigned ns, bool variable = true, unsigned maxKey = 10, bool reservedFuture = false)
        : bytes(ns * 4, 0) {
        auto p = bytes.data();
        p[8] = 1;
        P16(p + 10, 3);
        P16(p + 32, uint16_t(ns));
        P16(p + 34, uint16_t(maxKey));
        P32(p + 36, 4);
        P32(p + 40, 3);
        P32(p + 52, variable ? 6 : 2);
        P16(p + ns - 2, 14);
        P16(p + ns - 4, 120);
        P16(p + ns - 6, 248);
        P16(p + ns - 8, uint16_t(ns - 8));
        p[248] = 0x80;
        if (reservedFuture) {
            memset(p + 248, 0xff, ns - 256);
            p[248] = 0x8f;
        }
    }
    std::unique_ptr<BTree> Open() {
        return std::unique_ptr<BTree>(new BTree(
            [&](uint64_t o, void *p, size_t n) {
                Check(Range(o, n, bytes.size()), "Test B-tree read range");
                memcpy(p, bytes.data() + o, n);
            },
            [&](uint64_t o, const void *p, size_t n) {
                Check(Range(o, n, bytes.size()), "Test B-tree write range");
                memcpy(bytes.data() + o, p, n);
                written += n;
            },
            [&](uint64_t n) {
                bytes.resize(size_t(n), 0);
                ++grows;
                return bytes.size();
            },
            [](const Bytes &a, const Bytes &b) {
                auto x = U32(a.data() + 2), y = U32(b.data() + 2);
                return x == y ? 0 : x < y ? -1 : 1;
            }));
    }
};
Bytes Key(uint32_t n) {
    Bytes b(6);
    P16(b.data(), 4);
    P32(b.data() + 2, n);
    return b;
}
void Verify(File &f, const std::map<uint32_t, Bytes> &expected) {
    auto h = f.bytes.data();
    unsigned ns = U16(h + 32);
    uint32_t total = U32(h + 36);
    Check(uint64_t(total) * ns == f.bytes.size(), "Test tree physical length");
    auto records = [&](uint32_t number) {
        Check(number < total, "Validator child range");
        auto p = f.bytes.data() + uint64_t(number) * ns;
        unsigned count = U16(p + 10);
        std::vector<TreeRecord> out;
        for (unsigned i = 0; i < count; ++i) {
            unsigned a = U16(p + ns - 2 * (i + 1)), b = U16(p + ns - 2 * (i + 2));
            Check(a >= 14 && a + 2 <= b && b <= ns - 2 * (count + 1), "Validator record offsets");
            unsigned key = U16(p + a) + 2;
            Check(key >= 6 && key % 2 == 0 && key <= b - a, "Validator key length");
            out.push_back({Bytes(p + a, p + a + key), Bytes(p + a + key, p + b)});
        }
        return out;
    };
    std::set<uint32_t> nodes{0};
    std::map<uint32_t, Bytes> actual;
    std::vector<uint32_t> leaves;
    std::function<Bytes(uint32_t, unsigned)> walk = [&](uint32_t number, unsigned height) {
        Check(number && nodes.insert(number).second, "Validator cycle/duplicate child");
        auto p = f.bytes.data() + uint64_t(number) * ns;
        Check(p[9] == height && int8_t(p[8]) == (height == 1 ? -1 : 0), "Validator height/kind");
        auto r = records(number);
        Check(!r.empty(), "Validator empty node");
        uint32_t previous = 0;
        bool first = true;
        for (auto &e : r) {
            uint32_t key = U32(e.key.data() + 2);
            Check(first || previous < key, "Validator key order");
            first = false;
            previous = key;
            if (height == 1) {
                Check(actual.emplace(key, e.value).second, "Validator duplicate leaf key");
            } else {
                Check(e.value.size() == 4, "Validator index pointer");
                auto low = walk(U32(e.value.data()), height - 1);
                Check(key == U32(low.data() + 2), "Validator stale index lower key");
                if (U32(h + 52) & 4)
                    Check(e.key == low, "Validator stale variable index key bytes");
            }
        }
        if (height == 1)
            leaves.push_back(number);
        return r[0].key;
    };
    uint32_t root = U32(h + 16);
    if (root)
        walk(root, U16(h + 14));
    Check(actual == expected && U32(h + 20) == expected.size(), "Validator record contents/count");
    for (size_t i = 0; i < leaves.size(); ++i) {
        auto p = f.bytes.data() + uint64_t(leaves[i]) * ns;
        Check(U32(p) == (i + 1 == leaves.size() ? 0 : leaves[i + 1]) && U32(p + 4) == (i ? leaves[i - 1] : 0),
              "Validator leaf chain");
    }
    Check(U32(h + 24) == (leaves.empty() ? 0 : leaves.front()) &&
              U32(h + 28) == (leaves.empty() ? 0 : leaves.back()),
          "Validator first/last leaf");
    uint64_t bit = 0;
    uint32_t map = 0, allocated = 0;
    std::set<uint32_t> maps;
    do {
        Check(maps.insert(map).second, "Validator map cycle");
        auto p = f.bytes.data() + uint64_t(map) * ns;
        unsigned index = map ? 0 : 2, start = U16(p + ns - 2 * (index + 1)),
                 end = U16(p + ns - 2 * (index + 2));
        Check(start < end && end < ns, "Validator map bounds");
        if (map)
            Check(nodes.insert(map).second && p[8] == 2, "Validator map overlaps data node");
        for (unsigned i = start; i < end; ++i)
            for (unsigned b = 0; b < 8; ++b, ++bit)
                if (bit < total && (p[i] & (0x80 >> b))) {
                    ++allocated;
                }
        map = U32(p);
    } while (map);
    // Map nodes later in the chain may be represented in an earlier map record.
    uint32_t used = 0;
    bit = 0;
    map = 0;
    do {
        auto p = f.bytes.data() + uint64_t(map) * ns;
        unsigned index = map ? 0 : 2, start = U16(p + ns - 2 * (index + 1)),
                 end = U16(p + ns - 2 * (index + 2));
        for (unsigned i = start; i < end; ++i)
            for (unsigned b = 0; b < 8; ++b, ++bit)
                if (bit < total) {
                    bool set = (p[i] & (0x80 >> b)) != 0;
                    Check(set == bool(nodes.count(uint32_t(bit))), "Validator allocation map mismatch");
                    if (set)
                        ++used;
                }
        map = U32(p);
    } while (map);
    Check(bit >= total && U32(h + 40) == total - used, "Validator free node count");
}
} // namespace
void RunIncrementalBTreeTests() {
    for (unsigned ns : {512u, 4096u})
        for (bool variable : {false, true}) {
            File f(ns, variable, 10, true);
            std::map<uint32_t, Bytes> expected;
            std::mt19937 random(1207);
            for (unsigned batch = 0; batch < 40; ++batch) {
                auto tree = f.Open();
                for (unsigned i = 0; i < 80; ++i) {
                    uint32_t key = random() % 500;
                    if (random() % 3 == 0) {
                        Check(tree->Erase(Key(key)) == bool(expected.erase(key)), "B-tree erase result");
                    } else {
                        Bytes value(2 + 2 * (random() % (ns / 4)), uint8_t(key));
                        tree->Set({Key(key), value});
                        expected[key] = value;
                    }
                }
                tree->Flush();
                Verify(f, expected);
            }
            for (auto &r : expected) {
                auto tree = f.Open();
                Check(tree->Erase(Key(r.first)), "B-tree final erase");
                tree->Flush();
            }
            expected.clear();
            Verify(f, expected);
            Check(U32(f.bytes.data() + 16) == 0, "B-tree root did not collapse");
        }
    // Variable-length lower-bound keys can grow during deletion and split an
    // index even though a leaf record was removed. Replacements also preserve
    // changed raw key bytes when the comparator considers them equivalent.
    {
        File f(4096, true, 516);
        std::map<uint32_t, Bytes> model;
        std::mt19937 random(7719);
        for (unsigned batch = 0; batch < 70; ++batch) {
            auto tree = f.Open();
            for (unsigned i = 0; i < 70; ++i) {
                uint32_t id = random() % 1400;
                if (random() % 3 == 0) {
                    Check(tree->Erase(Key(id)) == bool(model.erase(id)), "Variable-key erase");
                } else {
                    Bytes key = Key(id);
                    key.resize(6 + 2 * (random() % 251), uint8_t(id));
                    P16(key.data(), uint16_t(key.size() - 2));
                    Bytes value(2 + 2 * (random() % 200), uint8_t(batch));
                    tree->Set({key, value});
                    model[id] = value;
                }
            }
            tree->Flush();
            Verify(f, model);
        }
        auto tree = f.Open();
        for (auto &r : model)
            Check(tree->Erase(Key(r.first)), "Variable-key cleanup");
        tree->Flush();
        model.clear();
        Verify(f, model);
    }
    // More nodes than fit in a 512-byte header map. Also check reopening and map
    // nodes represented by other map records, rather than a private node ceiling.
    File large(512);
    std::map<uint32_t, Bytes> expected;
    {
        auto tree = large.Open();
        for (uint32_t i = 0; i < 14000; ++i) {
            Bytes value(200, uint8_t(i));
            tree->Set({Key(i), value});
            expected[i] = value;
        }
        tree->Flush();
    }
    Verify(large, expected);
    Check(U32(large.bytes.data()) != 0 && U32(large.bytes.data() + 36) > 2048,
          "B-tree map chain did not grow");
    uint64_t before = large.written;
    {
        auto tree = large.Open();
        Bytes value(202, 0x77);
        tree->Set({Key(7000), value});
        expected[7000] = value;
        tree->Flush();
    }
    Verify(large, expected);
    Check(large.written - before < 16384, "Small update rewrote the B-tree");
    for (uint32_t base = 0; base < 14000; base += 200) {
        auto tree = large.Open();
        for (uint32_t i = base; i < base + 200; ++i) {
            Check(tree->Erase(Key(i)), "Map-chain deletion");
            expected.erase(i);
        }
        tree->Flush();
    }
    Verify(large, expected);
    // Fill actual 4 KiB nodes beyond the former single-header-map ceiling.
    {
        File catalog(4096, true, 10);
        std::map<uint32_t, Bytes> records;
        auto tree = catalog.Open();
        for (uint32_t i = 0; i < 31200; ++i) {
            Bytes value(2000, uint8_t(i));
            tree->Set({Key(i), value});
            records[i] = value;
        }
        tree->Flush();
        tree.reset();
        Verify(catalog, records);
        Check(U32(catalog.bytes.data()) != 0 && U32(catalog.bytes.data() + 36) > 30720,
              "4 KiB catalog did not exceed the former node ceiling");
    }
    puts("PASS: incremental B-trees: random insert/update/delete, variable records, splits/merges, root "
         "growth/collapse, map chains, reopen validation and bounded small-update writes");
}
