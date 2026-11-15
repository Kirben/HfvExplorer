// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlusDisk.h"
namespace HfsPlus {
namespace Disk {
struct TreeRecord {
    Bytes key, value;
};
// Incremental HFS+ B+tree editor. Offsets are logical fork offsets; the caller
// journals writes and owns physical allocation. Existing node sizes and map
// chains are preserved. Only changed nodes/maps/header sectors are staged.
class BTree {
  public:
    using Read = std::function<void(uint64_t, void *, size_t)>;
    using Write = std::function<void(uint64_t, const void *, size_t)>;
    using Grow = std::function<uint64_t(uint64_t)>;
    using Compare = std::function<int(const Bytes &, const Bytes &)>;

  private:
    struct Node {
        uint32_t next = 0, prev = 0;
        int kind = 0;
        unsigned height = 0;
        std::vector<TreeRecord> records;
    };
    struct Map {
        uint32_t node;
        uint64_t first;
        size_t offset, length;
        Bytes bytes;
        bool dirty = false;
    };
    Read read_;
    Write write_;
    Grow grow_;
    Compare compare_;
    Bytes header_;
    unsigned size_ = 0, maxKey_ = 0;
    bool variable_ = false;
    uint32_t root_ = 0, total_ = 0, free_ = 0, leaves_ = 0, first_ = 0, last_ = 0, candidate_ = 1;
    unsigned depth_ = 0;
    std::vector<Map> maps_;
    std::map<uint32_t, Node> nodes_;
    std::set<uint32_t> dirty_, zero_;
    bool headerDirty_ = false;
    uint16_t Offset(const Bytes &b, unsigned record) const {
        return U16(b.data() + size_ - 2 * (record + 1));
    }
    size_t KeySpace(const TreeRecord &r, int kind) const {
        return ((kind == 0 && !variable_ ? size_t(maxKey_) + 2 : r.key.size()) + 1) & ~size_t(1);
    }
    size_t Used(const Node &n) const {
        size_t used = 14 + 2 * (n.records.size() + 1);
        for (auto &r : n.records)
            used += KeySpace(r, n.kind) + r.value.size();
        return used;
    }
    bool Fits(const Node &n) const { return Used(n) <= size_ && n.records.size() <= 65535; }
    Bytes Encode(const Node &n) const {
        Check(Fits(n), "HFS+ B-tree record does not fit its node");
        Bytes b(size_, 0);
        P32(b.data(), n.next);
        P32(b.data() + 4, n.prev);
        b[8] = uint8_t(n.kind);
        b[9] = uint8_t(n.height);
        P16(b.data() + 10, uint16_t(n.records.size()));
        size_t offset = 14;
        unsigned i = 0;
        for (auto &r : n.records) {
            P16(b.data() + size_ - 2 * (++i), uint16_t(offset));
            size_t keys = KeySpace(r, n.kind);
            Check(r.key.size() <= keys, "Invalid fixed HFS+ index key");
            memcpy(b.data() + offset, r.key.data(), r.key.size());
            if (n.kind == 0 && !variable_)
                P16(b.data() + offset, uint16_t(maxKey_));
            offset += keys;
            memcpy(b.data() + offset, r.value.data(), r.value.size());
            offset += r.value.size();
        }
        P16(b.data() + size_ - 2 * (i + 1), uint16_t(offset));
        return b;
    }
    Node &Get(uint32_t number) {
        Check(number && number < total_ && Allocated(number), "Invalid HFS+ B-tree child number");
        auto found = nodes_.find(number);
        if (found != nodes_.end())
            return found->second;
        Bytes b(size_);
        read_(uint64_t(number) * size_, b.data(), b.size());
        Node n;
        n.next = U32(b.data());
        n.prev = U32(b.data() + 4);
        n.kind = int8_t(b[8]);
        n.height = b[9];
        unsigned count = U16(b.data() + 10);
        Check((n.kind == -1 || n.kind == 0) && n.height && count && count < (size_ - 14) / 4,
              "Invalid HFS+ B-tree node descriptor");
        for (unsigned i = 0; i < count; ++i) {
            unsigned a = Offset(b, i), end = Offset(b, i + 1);
            Check(a >= 14 && a + 2 <= end && end <= size_ - 2 * (count + 1),
                  "Invalid HFS+ B-tree record range");
            unsigned key = U16(b.data() + a) + 2, padded = (key + 1) & ~1u;
            Check(key >= 4 && key <= maxKey_ + 2 && padded <= end - a, "Invalid HFS+ B-tree key length");
            TreeRecord r{Bytes(b.begin() + a, b.begin() + a + key),
                         Bytes(b.begin() + a + padded, b.begin() + end)};
            Check(n.kind != 0 || r.value.size() == 4, "Invalid HFS+ index pointer");
            if (!n.records.empty())
                Check(compare_(n.records.back().key, r.key) < 0, "Unordered HFS+ B-tree node");
            n.records.push_back(std::move(r));
        }
        return nodes_.emplace(number, std::move(n)).first->second;
    }
    Map &MapFor(uint32_t number) {
        for (auto &m : maps_)
            if (number >= m.first && uint64_t(number) - m.first < m.length * 8)
                return m;
        throw Error("HFS+ B-tree map chain is too short");
    }
    bool Allocated(uint32_t number) {
        auto &m = MapFor(number);
        uint64_t bit = number - m.first;
        return (m.bytes[m.offset + size_t(bit / 8)] & (0x80 >> (bit % 8))) != 0;
    }
    void Mark(uint32_t number, bool used) {
        auto &m = MapFor(number);
        uint64_t bit = number - m.first;
        auto &byte = m.bytes[m.offset + size_t(bit / 8)];
        uint8_t mask = uint8_t(0x80 >> (bit % 8));
        Check(bool(byte & mask) != used, "HFS+ B-tree allocation bit mismatch");
        if (used)
            byte |= mask;
        else
            byte &= uint8_t(~mask);
        m.dirty = true;
        headerDirty_ = true;
        if (used) {
            Check(free_ > 0, "Invalid HFS+ free node count");
            --free_;
        } else {
            ++free_;
            candidate_ = (std::min)(candidate_, number);
        }
    }
    void Expand() {
        uint32_t old = total_;
        uint64_t bytes = grow_((uint64_t(total_) + 16) * size_);
        Check(bytes % size_ == 0 && bytes / size_ > old && bytes / size_ <= 0xffffffffULL,
              "Invalid HFS+ B-tree growth result");
        total_ = uint32_t(bytes / size_);
        free_ += total_ - old;
        headerDirty_ = true;
        uint64_t covered = maps_.back().first + maps_.back().length * 8;
        uint32_t reserve = old;
        for (uint64_t number = old; number < (std::min)(uint64_t(total_), covered); ++number) {
            auto &m = MapFor(uint32_t(number));
            uint64_t bit = number - m.first;
            auto &byte = m.bytes[m.offset + size_t(bit / 8)];
            uint8_t mask = uint8_t(0x80 >> (bit % 8));
            if (byte & mask) {
                byte &= uint8_t(~mask);
                m.dirty = true;
            }
        }
        while (covered < total_) {
            Check(reserve < total_, "HFS+ B-tree has no room for a map node");
            uint32_t number = reserve++;
            Map m{number, covered, 14, size_t(size_ - 20), Bytes(size_, 0), true};
            m.bytes[8] = 2;
            P16(m.bytes.data() + 10, 1);
            P16(m.bytes.data() + size_ - 2, 14);
            P16(m.bytes.data() + size_ - 4, uint16_t(size_ - 6));
            Map &previous = maps_.back();
            P32(previous.bytes.data(), number);
            previous.dirty = true;
            P32(m.bytes.data() + 4, previous.node);
            maps_.push_back(std::move(m));
            covered = maps_.back().first + maps_.back().length * 8;
            Mark(number, true);
        }
    }
    uint32_t Allocate(int kind, unsigned height) {
        if (!free_)
            Expand();
        for (uint64_t step = 0; step < total_ - 1; ++step) {
            uint32_t number = 1 + uint32_t((uint64_t(candidate_ - 1) + step) % (total_ - 1));
            if (!Allocated(number)) {
                Mark(number, true);
                candidate_ = number == total_ - 1 ? 1 : number + 1;
                Node n;
                n.kind = kind;
                n.height = height;
                nodes_[number] = std::move(n);
                dirty_.insert(number);
                zero_.erase(number);
                return number;
            }
        }
        throw Error("HFS+ B-tree free node count disagrees with map");
    }
    void Release(uint32_t number) {
        Mark(number, false);
        dirty_.erase(number);
        nodes_.erase(number);
        zero_.insert(number);
    }
    TreeRecord Pointer(uint32_t number) {
        Node &n = Get(number);
        Check(!n.records.empty(), "Empty HFS+ index child");
        TreeRecord r;
        r.key = n.records.front().key;
        r.value.resize(4);
        P32(r.value.data(), number);
        return r;
    }
    size_t Child(const Node &n, const Bytes &key) {
        size_t i = 0;
        while (i + 1 < n.records.size() && compare_(n.records[i + 1].key, key) <= 0)
            ++i;
        return i;
    }
    void Dirty(uint32_t n) {
        dirty_.insert(n);
        headerDirty_ = true;
    }
    size_t Cut(const Node &combined) {
        size_t best = 0, difference = SIZE_MAX, leftPayload = 0;
        size_t payload = Used(combined) - 14 - 2 * (combined.records.size() + 1);
        for (size_t i = 1; i < combined.records.size(); ++i) {
            const auto &record = combined.records[i - 1];
            leftPayload += KeySpace(record, combined.kind) + record.value.size();
            size_t x = 14 + leftPayload + 2 * (i + 1);
            size_t y = 14 + payload - leftPayload + 2 * (combined.records.size() - i + 1);
            if (x > size_ || y > size_)
                continue;
            size_t d = x > y ? x - y : y - x;
            if (d < difference) {
                difference = d;
                best = i;
            }
        }
        Check(best != 0, "HFS+ B-tree record cannot be split into nodes");
        return best;
    }
    uint32_t Split(uint32_t number) {
        Node &n = Get(number);
        if (Fits(n))
            return 0;
        size_t cut = Cut(n);
        uint32_t right = Allocate(n.kind, n.height);
        Node &r = Get(right);
        r.records.assign(n.records.begin() + cut, n.records.end());
        n.records.erase(n.records.begin() + cut, n.records.end());
        r.next = n.next;
        r.prev = number;
        if (n.next) {
            Get(n.next).prev = right;
            Dirty(n.next);
        }
        n.next = right;
        if (n.kind == -1 && last_ == number)
            last_ = right;
        Dirty(number);
        Dirty(right);
        return right;
    }
    uint32_t Insert(uint32_t number, const TreeRecord &record, unsigned height) {
        Node &n = Get(number);
        Check(n.height == height, "Invalid HFS+ B-tree height");
        if (n.kind == -1) {
            Check(height == 1, "Invalid HFS+ leaf height");
            auto i = std::lower_bound(
                n.records.begin(), n.records.end(), record,
                [&](const TreeRecord &a, const TreeRecord &b) { return compare_(a.key, b.key) < 0; });
            if (i != n.records.end() && !compare_(i->key, record.key))
                *i = record;
            else {
                n.records.insert(i, record);
                Check(leaves_ != 0xffffffff, "HFS+ leaf record count exhausted");
                ++leaves_;
            }
        } else {
            Check(height > 1, "Invalid HFS+ index height");
            size_t i = Child(n, record.key);
            uint32_t child = U32(n.records[i].value.data()), right = Insert(child, record, height - 1);
            n.records[i] = Pointer(child);
            if (right)
                n.records.insert(n.records.begin() + i + 1, Pointer(right));
        }
        Dirty(number);
        return Split(number);
    }
    void Unlink(uint32_t number) {
        Node n = Get(number);
        if (n.prev) {
            Get(n.prev).next = n.next;
            Dirty(n.prev);
        }
        if (n.next) {
            Get(n.next).prev = n.prev;
            Dirty(n.next);
        }
        if (n.kind == -1) {
            if (first_ == number)
                first_ = n.next;
            if (last_ == number)
                last_ = n.prev;
        }
        Release(number);
    }
    void Balance(Node &parent, size_t index) {
        if (parent.records.size() < 2)
            return;
        size_t left = index ? index - 1 : 0, right = left + 1;
        uint32_t a = U32(parent.records[left].value.data()), b = U32(parent.records[right].value.data());
        Node &x = Get(a);
        Node &y = Get(b);
        Check(x.height == y.height && x.kind == y.kind && x.next == b && y.prev == a,
              "Invalid HFS+ B-tree sibling chain");
        if (Used(x) >= size_ / 2 && Used(y) >= size_ / 2)
            return;
        Node combined = x;
        combined.records.insert(combined.records.end(), y.records.begin(), y.records.end());
        if (Fits(combined)) {
            x.records = std::move(combined.records);
            x.next = y.next;
            if (y.next) {
                Get(y.next).prev = a;
                Dirty(y.next);
            }
            if (x.kind == -1 && last_ == b)
                last_ = a;
            Release(b);
            parent.records.erase(parent.records.begin() + right);
            Dirty(a);
        } else {
            size_t cut = Cut(combined);
            x.records.assign(combined.records.begin(), combined.records.begin() + cut);
            y.records.assign(combined.records.begin() + cut, combined.records.end());
            Dirty(a);
            Dirty(b);
            parent.records[right] = Pointer(b);
        }
        parent.records[left] = Pointer(a);
    }
    struct Removed {
        bool found = false;
        uint32_t split = 0;
    };
    Removed Remove(uint32_t number, const Bytes &key, unsigned height) {
        Node &n = Get(number);
        Check(n.height == height, "Invalid HFS+ B-tree height");
        if (n.kind == -1) {
            Check(height == 1, "Invalid HFS+ leaf height");
            auto i =
                std::lower_bound(n.records.begin(), n.records.end(), key,
                                 [&](const TreeRecord &a, const Bytes &b) { return compare_(a.key, b) < 0; });
            if (i == n.records.end() || compare_(i->key, key))
                return {};
            n.records.erase(i);
            --leaves_;
        } else {
            Check(height > 1, "Invalid HFS+ index height");
            size_t i = Child(n, key);
            uint32_t child = U32(n.records[i].value.data());
            auto result = Remove(child, key, height - 1);
            if (!result.found)
                return {};
            if (Get(child).records.empty()) {
                Unlink(child);
                n.records.erase(n.records.begin() + i);
                if (i && i == n.records.size())
                    --i;
            } else {
                n.records[i] = Pointer(child);
                if (result.split)
                    n.records.insert(n.records.begin() + i + 1, Pointer(result.split));
            }
            if (!n.records.empty())
                Balance(n, (std::min)(i, n.records.size() - 1));
        }
        Dirty(number);
        return {true, n.records.empty() ? 0 : Split(number)};
    }
    void RootSplit(uint32_t right) {
        if (!right)
            return;
        uint32_t old = root_;
        root_ = Allocate(0, ++depth_);
        Get(root_).records = {Pointer(old), Pointer(right)};
        Dirty(root_);
    }

  public:
    BTree(Read read, Write write, Grow grow, Compare compare)
        : read_(std::move(read)), write_(std::move(write)), grow_(std::move(grow)),
          compare_(std::move(compare)) {
        Bytes initial(120);
        read_(0, initial.data(), initial.size());
        size_ = U16(initial.data() + 32);
        Check(size_ >= 512 && size_ <= 32768 && (size_ & (size_ - 1)) == 0, "Invalid HFS+ B-tree node size");
        header_.resize(size_);
        read_(0, header_.data(), header_.size());
        Check(header_[8] == 1 && U16(header_.data() + 10) == 3 && Offset(header_, 0) == 14,
              "Invalid HFS+ B-tree header");
        depth_ = U16(header_.data() + 14);
        root_ = U32(header_.data() + 16);
        leaves_ = U32(header_.data() + 20);
        first_ = U32(header_.data() + 24);
        last_ = U32(header_.data() + 28);
        maxKey_ = U16(header_.data() + 34);
        total_ = U32(header_.data() + 36);
        free_ = U32(header_.data() + 40);
        variable_ = (U32(header_.data() + 52) & 4) != 0;
        Check(total_ && free_ < total_ && depth_ <= 64 && root_ < total_ && (!depth_ == !root_),
              "Invalid HFS+ B-tree header counts");
        unsigned start = Offset(header_, 2), end = Offset(header_, 3);
        Check(start >= 120 && start < end && end <= size_ - 8, "Invalid HFS+ header map record");
        maps_.push_back({0, 0, start, end - start, header_, false});
        std::set<uint32_t> visited{0};
        uint32_t next = U32(header_.data()), previous = 0;
        uint64_t covered = uint64_t(end - start) * 8;
        while (next) {
            Check(next < total_ && visited.insert(next).second, "Invalid HFS+ map chain");
            Bytes b(size_);
            read_(uint64_t(next) * size_, b.data(), b.size());
            Check(b[8] == 2 && b[9] == 0 && U16(b.data() + 10) == 1 && U32(b.data() + 4) == previous,
                  "Invalid HFS+ map node");
            unsigned a = Offset(b, 0), z = Offset(b, 1);
            Check(a >= 14 && a < z && z <= size_ - 4, "Invalid HFS+ map record");
            maps_.push_back({next, covered, a, z - a, std::move(b), false});
            covered += uint64_t(z - a) * 8;
            previous = next;
            next = U32(maps_.back().bytes.data());
        }
        Check(covered >= total_ && Allocated(0), "Incomplete HFS+ B-tree bitmap");
        for (auto &m : maps_)
            Check(Allocated(m.node), "Unallocated HFS+ B-tree map node");
    }
    void Set(const TreeRecord &r) {
        Check(r.key.size() >= 4 && r.key.size() <= maxKey_ + 2, "Invalid HFS+ insertion key");
        Node test;
        test.kind = -1;
        test.records = {r};
        Check(Fits(test), "HFS+ record is too large for node size");
        if (!root_) {
            root_ = Allocate(-1, 1);
            depth_ = 1;
            first_ = last_ = root_;
        }
        RootSplit(Insert(root_, r, depth_));
    }
    bool Erase(const Bytes &key) {
        if (!root_)
            return false;
        auto result = Remove(root_, key, depth_);
        if (!result.found)
            return false;
        RootSplit(result.split);
        while (root_) {
            Node &n = Get(root_);
            if (n.records.empty()) {
                uint32_t old = root_;
                root_ = 0;
                depth_ = 0;
                first_ = last_ = 0;
                Unlink(old);
                break;
            }
            if (n.kind == 0 && n.records.size() == 1) {
                uint32_t old = root_;
                root_ = U32(n.records[0].value.data());
                --depth_;
                Release(old);
                Get(root_).prev = Get(root_).next = 0;
                Dirty(root_);
            } else
                break;
        }
        return true;
    }
    void Flush() {
        for (auto number : zero_) {
            Bytes b(size_, 0);
            write_(uint64_t(number) * size_, b.data(), b.size());
        }
        for (auto number : dirty_) {
            Bytes b = Encode(Get(number));
            write_(uint64_t(number) * size_, b.data(), b.size());
        }
        if (headerDirty_) {
            auto &b = maps_[0].bytes;
            P16(b.data() + 14, uint16_t(depth_));
            P32(b.data() + 16, root_);
            P32(b.data() + 20, leaves_);
            P32(b.data() + 24, first_);
            P32(b.data() + 28, last_);
            P32(b.data() + 36, total_);
            P32(b.data() + 40, free_);
            maps_[0].dirty = true;
        }
        for (auto &m : maps_)
            if (m.dirty) {
                write_(uint64_t(m.node) * size_, m.bytes.data(), m.bytes.size());
                m.dirty = false;
            }
        dirty_.clear();
        zero_.clear();
        headerDirty_ = false;
    }
};
} // namespace Disk
} // namespace HfsPlus
