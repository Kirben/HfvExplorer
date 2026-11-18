// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include "StorageOutcome.h"
#include <exception>
#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
namespace HfsPlus {
namespace Disk {
inline void Check(bool ok, const char *text) {
    if (!ok)
        throw Error(text);
}
inline uint16_t U16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t U32(const uint8_t *p) { return uint32_t(U16(p)) << 16 | U16(p + 2); }
inline uint64_t U64(const uint8_t *p) { return uint64_t(U32(p)) << 32 | U32(p + 4); }
inline void P16(uint8_t *p, uint16_t n) {
    p[0] = uint8_t(n >> 8);
    p[1] = uint8_t(n);
}
inline void P32(uint8_t *p, uint32_t n) {
    P16(p, uint16_t(n >> 16));
    P16(p + 2, uint16_t(n));
}
inline void P64(uint8_t *p, uint64_t n) {
    P32(p, uint32_t(n >> 32));
    P32(p + 4, uint32_t(n));
}
inline bool Range(uint64_t o, uint64_t n, uint64_t limit) { return o <= limit && n <= limit - o; }
inline Bytes Read(Source &s, uint64_t o, size_t n) {
    Check(Range(o, n, s.Size()), "Read outside image");
    Bytes b(n);
    s.Read(o, b.data(), n);
    return b;
}
inline uint32_t Sum(const uint8_t *p, size_t n) {
    uint32_t c = 0;
    while (n--)
        c = (c << 8) ^ (c + *p++);
    return ~c;
}
using Sector = std::array<uint8_t, 512>;
using Patches = std::map<uint64_t, Sector>;
// Bounded view plus an in-memory metadata transaction. User data is written only
// into newly allocated blocks and flushed before any metadata becomes visible.
struct View {
    Source &source;
    uint64_t base = 0, length = 0;
    Patches patches;
    explicit View(Source &s) : source(s), length(s.Size()) {}
    void Read(uint64_t offset, void *buffer, size_t count) {
        Check(Range(offset, count, length), "Read outside HFS+ volume");
        auto p = static_cast<uint8_t *>(buffer);
        while (count) {
            uint64_t sector = offset / 512;
            size_t skip = size_t(offset % 512), n = (std::min)(count, 512 - skip);
            auto i = patches.lower_bound(sector);
            if (i == patches.end() || i->first != sector) {
                n = i == patches.end() ? count : size_t((std::min)(uint64_t(count), i->first * 512 - offset));
                source.Read(base + offset, p, n);
            } else
                memcpy(p, i->second.data() + skip, n);
            offset += n;
            p += n;
            count -= n;
        }
    }
    Bytes Read(uint64_t offset, size_t n) {
        Bytes b(n);
        Read(offset, b.data(), n);
        return b;
    }
    void Stage(uint64_t offset, const void *buffer, size_t count) {
        Check(Range(offset, count, length), "Write outside HFS+ volume");
        auto p = static_cast<const uint8_t *>(buffer);
        while (count) {
            uint64_t sector = offset / 512;
            size_t skip = size_t(offset % 512), n = (std::min)(count, 512 - skip);
            auto i = patches.find(sector);
            if (i == patches.end()) {
                Sector b;
                Read(sector * 512, b.data(), 512);
                i = patches.emplace(sector, b).first;
            }
            memcpy(i->second.data() + skip, p, n);
            offset += n;
            p += n;
            count -= n;
        }
    }
    void Write(uint64_t offset, const void *p, size_t n) {
        Check(Range(offset, n, length), "Write outside HFS+ volume");
        source.Write(base + offset, p, n);
    }
    void Apply() {
        // A recovered volume header may change length after patches were staged.
        // Validate the entire batch before the first home-block write.
        Check(Range(base, length, source.Size()), "Invalid HFS+ view range");
        for (const auto &p : patches)
            Check(p.first <= UINT64_MAX / 512 && Range(p.first * 512, 512, length),
                  "Staged metadata outside HFS+ volume");
        for (auto &p : patches)
            Write(p.first * 512, p.second.data(), 512);
        source.Flush();
        patches.clear();
    }
};
struct Journal {
    View &view;
    std::shared_ptr<Source> device; // UUID-validated external journal partition, when present
    uint64_t offset = 0, size = 0, start = 0, end = 0, infoOffset = 0;
    uint32_t headerSize = 0, listSize = 0, sequence = 0;
    bool little = false, enabled = false, needsInit = false;
    Bytes header;
    explicit Journal(View &v) : view(v) {}
    uint16_t R16(const uint8_t *p) const { return little ? uint16_t(p[1]) << 8 | p[0] : U16(p); }
    uint32_t R32(const uint8_t *p) const { return little ? uint32_t(R16(p + 2)) << 16 | R16(p) : U32(p); }
    uint64_t R64(const uint8_t *p) const { return little ? uint64_t(R32(p + 4)) << 32 | R32(p) : U64(p); }
    void W16(uint8_t *p, uint16_t n) {
        if (little) {
            p[0] = uint8_t(n);
            p[1] = uint8_t(n >> 8);
        } else
            P16(p, n);
    }
    void W32(uint8_t *p, uint32_t n) {
        if (little) {
            W16(p, uint16_t(n));
            W16(p + 2, uint16_t(n >> 16));
        } else
            P32(p, n);
    }
    void W64(uint8_t *p, uint64_t n) {
        if (little) {
            W32(p, uint32_t(n));
            W32(p + 4, uint32_t(n >> 32));
        } else
            P64(p, n);
    }
    bool External()const{return bool(device);}
    Bytes LogRead(uint64_t at,size_t n){if(!device)return view.Read(offset+at,n);Check(Range(offset+at,n,device->Size()),"External journal read range");Bytes data(n);device->Read(offset+at,data.data(),n);return data;}
    void LogWrite(uint64_t at,const void* p,size_t n){if(device){Check(device->Writable(),"External journal is read-only");device->Write(offset+at,p,n);}else view.Write(offset+at,p,n);}
    void FlushLog(){if(device)device->Flush();else view.source.Flush();}
    uint64_t Advance(uint64_t pos, uint64_t n) const {
        return headerSize + (pos - headerSize + n) % (size - headerSize);
    }
    uint64_t Distance(uint64_t a, uint64_t b) const { return b >= a ? b - a : (size - a) + (b - headerSize); }
    void RingRead(uint64_t &pos, void *buffer, size_t n) {
        auto p = static_cast<uint8_t *>(buffer);
        while (n) {
            size_t part = size_t((std::min)(uint64_t(n), size - pos));
            auto bytes=LogRead(pos,part);memcpy(p,bytes.data(),part);
            pos = Advance(pos, part);
            n -= part;
            p += part;
        }
    }
    void RingWrite(uint64_t &pos, const void *buffer, size_t n) {
        auto p = static_cast<const uint8_t *>(buffer);
        while (n) {
            size_t part = size_t((std::min)(uint64_t(n), size - pos));
            LogWrite(pos, p, part);
            pos = Advance(pos, part);
            n -= part;
            p += part;
        }
    }
    bool Overlaps(uint64_t o, uint64_t n) const { return !External() && o < offset + size && offset < o + n; }
    void StoreHeader() {
        W64(header.data() + 8, start);
        W64(header.data() + 16, end);
        W32(header.data() + 44, sequence);
        W32(header.data() + 36, 0);
        W32(header.data() + 36, Sum(header.data(), 44));
        LogWrite(0, header.data(), header.size());
        FlushLog();
    }
    void Open(const Bytes &vh, bool writable) {
        if (!(U32(vh.data() + 4) & 0x2000))
            return;
        enabled = true;
        uint32_t bs = U32(vh.data() + 40), jib = U32(vh.data() + 12);
        infoOffset = uint64_t(jib) * bs;
        Check(jib && uint64_t(jib) * bs < view.length, "Invalid journal info block");
        auto info = view.Read(uint64_t(jib) * bs, 180);
        uint32_t flags = U32(info.data());
        Check((flags&3)==1||(flags&3)==2,"Invalid journal device flags");
        if((flags&3)==2)device=view.source.OpenJournal(info);else device.reset();
        Check((flags&3)!=2||bool(device),"External journal resolver returned no device");
        offset = U64(info.data() + 36);
        size = U64(info.data() + 44);
        Check(offset % 512 == 0 && size >= 8192 && size % 512 == 0 && Range(offset, size, External()?device->Size():view.length),
              "Invalid journal range");
        Check(External()||(!Range(uint64_t(jib) * bs - offset, 180, size) && !(offset <= 1024 && 1024 < offset + size)),
              "Journal overlaps volume metadata");
        if (flags & 4) {
            needsInit = true;
            headerSize = 512;
            listSize = 4096;
            start = end = 512;
            little = true;
            header.assign(512, 0);
            W32(header.data(), 0x4a4e4c78);
            W32(header.data() + 4, 0x12345678);
            W64(header.data() + 24, size);
            W32(header.data() + 32, listSize);
            W32(header.data() + 40, headerSize);
            return;
        }
        header = LogRead(0, 512);
        little = U32(header.data() + 4) == 0x78563412;
        Check(R32(header.data()) == 0x4a4e4c78 && R32(header.data() + 4) == 0x12345678,
              "Invalid journal signature");
        uint32_t checksum = R32(header.data() + 36);
        W32(header.data() + 36, 0);
        Check(Sum(header.data(), 44) == checksum, "Journal header checksum mismatch");
        W32(header.data() + 36, checksum);
        start = R64(header.data() + 8);
        end = R64(header.data() + 16);
        headerSize = R32(header.data() + 40);
        listSize = R32(header.data() + 32);
        sequence = R32(header.data() + 44);
        Check(R64(header.data() + 24) == size && headerSize >= 512 && headerSize <= 4096 &&
                  (headerSize & (headerSize - 1)) == 0 && listSize >= 512 && listSize <= 1024 * 1024 &&
                  listSize % headerSize == 0,
              "Invalid journal geometry");
        Check(start >= headerSize && start < size && end >= headerSize && end < size &&
                  start % headerSize == 0 && end % headerSize == 0,
              "Invalid journal cursors");
        header = LogRead(0, headerSize);
        if (start == end)
            return;
        // Validate the entire active log before touching any home block. A damaged log
        // never results in a partial replay. Read-only mounts retain an overlay only.
        Patches recovered;
        uint64_t pos = start, remaining = Distance(start, end);
        uint32_t last = 0;
        while (remaining) {
            Check(remaining >= listSize, "Truncated journal block list");
            Bytes list(listSize);
            RingRead(pos, list.data(), list.size());
            auto copy = list;
            uint32_t sum = R32(copy.data() + 8);
            W32(copy.data() + 8, 0);
            Check(Sum(copy.data(), 32) == sum, "Journal transaction checksum mismatch");
            unsigned count = R16(list.data() + 2);
            uint32_t used = R32(list.data() + 4), flags2 = R32(list.data() + 12), seq = R32(list.data() + 28);
            Check(count >= 1 && count <= (listSize - 16) / 16 && used >= listSize && used <= remaining &&
                      used % headerSize == 0,
                  "Invalid journal transaction size");
            Check(!last || !seq || seq == last || seq == last + 1, "Journal sequence discontinuity");
            last = seq;
            uint64_t consumed = listSize;
            for (unsigned i = 1; i < count; ++i) {
                auto b = list.data() + 16 + i * 16;
                uint64_t number = R64(b);
                uint32_t n = R32(b + 8), ck = R32(b + 12);
                Check(n && n % headerSize == 0 && n <= used - consumed, "Invalid journal data length");
                Bytes data(n);
                RingRead(pos, data.data(), n);
                consumed += n;
                if (number == UINT64_MAX)
                    continue;
                Check(number <= UINT64_MAX / headerSize && Range(number * headerSize, n, view.length) &&
                          !Overlaps(number * headerSize, n),
                      "Journal target outside volume or inside journal metadata");
                Check(!(flags2 & 1) || !ck || Sum(data.data(), n) == ck, "Journal data checksum mismatch");
                for (size_t j = 0; j < n; j += 512) {
                    Sector sector;
                    memcpy(sector.data(), data.data() + j, 512);
                    recovered[number * (headerSize / 512) + j / 512] = sector;
                }
            }
            Check(consumed == used, "Journal transaction byte count mismatch");
            remaining -= used;
        }
        Check(pos == end, "Journal replay did not reach end");
        // Later journals override earlier sectors while preserving earlier replay
        // patches not repeated after a journal relocation.
        for (auto &patch : recovered)
            view.patches[patch.first] = std::move(patch.second);
        sequence = last ? last : sequence;
        if (writable) {
            view.Apply();
            start = end;
            StoreHeader();
        }
    }
    void Adopt(const Journal &j) {
        device=j.device;
        offset = j.offset;
        size = j.size;
        start = j.start;
        end = j.end;
        infoOffset = j.infoOffset;
        headerSize = j.headerSize;
        listSize = j.listSize;
        sequence = j.sequence;
        little = j.little;
        enabled = j.enabled;
        needsInit = j.needsInit;
        header = j.header;
    }
    void EmptyAt(uint64_t where, uint64_t bytes, const Journal &model) {
        Adopt(model);
        offset = where;
        size = bytes;
        start = end = headerSize;
        needsInit = false;
        header.assign(headerSize, 0);
        W32(header.data(), 0x4a4e4c78);
        W32(header.data() + 4, 0x12345678);
        W64(header.data() + 24, size);
        W32(header.data() + 32, listSize);
        W32(header.data() + 40, headerSize);
    }
    void Prune() {
        for (auto i = view.patches.begin(); i != view.patches.end();) {
            Sector previous;
            view.source.Read(view.base + i->first * 512, previous.data(), 512);
            if (previous == i->second)
                i = view.patches.erase(i);
            else
                ++i;
        }
    }
    std::map<uint64_t, Bytes> Blocks() {
        // Emit one metadata transaction, with as many block lists as necessary.
        // Use the native journal sector size, preserving untouched bytes on 2K/4K media.
        std::map<uint64_t, Bytes> blocks;
        for (auto &p : view.patches) {
            uint64_t block = p.first * 512 / headerSize;
            auto i = blocks.find(block);
            if (i == blocks.end()) {
                Bytes bytes(headerSize);
                view.Read(block * headerSize, bytes.data(), headerSize);
                i = blocks.emplace(block, std::move(bytes)).first;
            }
            memcpy(i->second.data() + (p.first * 512) % headerSize, p.second.data(), 512);
        }
        return blocks;
    }
    uint64_t RequiredBytes() {
        if (!enabled)
            return 0;
        std::set<uint64_t> blocks;
        for (auto &p : view.patches)
            blocks.insert(p.first * 512 / headerSize);
        size_t capacity = (listSize - 16) / 16 - 1;
        return uint64_t(blocks.size()) * headerSize + ((blocks.size() + capacity - 1) / capacity) * listSize;
    }
    bool Fits() {
        Prune();
        return !enabled || RequiredBytes() < size - headerSize;
    }
    void WriteLog(const std::function<void(const char *)> &fault, bool* publishing=nullptr) {
        Check(enabled && start == end, "Journal must be empty before committing");
        auto blocks = Blocks();
        size_t capacity = (listSize - 16) / 16 - 1;
        Check(RequiredBytes() < size - headerSize, "Journal has insufficient physical capacity");
        if (needsInit) {
            StoreHeader();
            Bytes info(180);
            view.source.Read(view.base + infoOffset, info.data(), info.size());
            P32(info.data(), U32(info.data()) & ~4u);
            view.Write(infoOffset, info.data(), info.size());
            view.source.Flush();
            needsInit = false;
        }
        uint64_t pos = end;
        auto i = blocks.begin();
        ++sequence;
        bool firstList = true;
        while (i != blocks.end()) {
            size_t n = (std::min)(capacity, size_t(std::distance(i, blocks.end())));
            Bytes list(listSize, 0);
            W16(list.data(), uint16_t(capacity + 1));
            W16(list.data() + 2, uint16_t(n + 1));
            W32(list.data() + 4, uint32_t(listSize + n * headerSize));
            W32(list.data() + 12, firstList ? 3 : 1);
            firstList = false;
            W32(list.data() + 28, sequence);
            auto first = i;
            for (size_t j = 1; j <= n; ++j, ++i) {
                auto p = list.data() + 16 + j * 16;
                W64(p, i->first);
                W32(p + 8, headerSize);
                W32(p + 12, Sum(i->second.data(), i->second.size()));
            }
            W32(list.data() + 8, Sum(list.data(), 32));
            RingWrite(pos, list.data(), list.size());
            for (auto j = first; j != i; ++j)
                RingWrite(pos, j->second.data(), j->second.size());
        }
        FlushLog();
        if (fault)
            fault("journal-data-flushed");
        end = pos;
        if(publishing)*publishing=true;
        StoreHeader();
        if (fault)
            fault("journal-committed");
    }
    // Prepare a complete redo transaction in the unreachable replacement log,
    // then atomically switch JIB (one device sector). This is the commit point.
    // Before the switch the old empty journal and filesystem remain authoritative;
    // afterwards native replay opens the new log directly. In particular, never
    // make an old journal replay change JIB: a native kernel could otherwise keep
    // using the old journal object after replay. No private recovery format.
    struct CommitEvidence {
        bool complete = false;
        bool publicationStarted = false;
        ~CommitEvidence() {
            if (!complete)
                Storage::OutcomeEvidence::NoteRollback(publicationStarted
                    ? Storage::Rollback::Unverified : Storage::Rollback::Restored);
        }
    };
    void CommitReplacement(Journal &replacement, const std::function<void(const char *)> &fault) {
        CommitEvidence evidence;
        Prune();
        Check(start == end && replacement.Fits(), "Replacement journal cannot encode relocation");
        view.source.Flush();
        if (fault)
            fault("journal-resize-data-flushed");
        replacement.StoreHeader();
        replacement.WriteLog([&](const char *at) {
            if (fault && strcmp(at, "journal-committed") == 0)
                fault("journal-resize-new-log-committed");
        });
        Check(infoOffset % headerSize == 0, "Unaligned journal info block");
        auto info = view.Read(infoOffset, headerSize);
        evidence.publicationStarted = true;
        view.Write(infoOffset, info.data(), info.size());
        if (fault)
            fault("journal-resize-jib-written");
        view.source.Flush();
        if (fault)
            fault("journal-resize-jib-committed");
        for (auto &patch : view.patches)
            view.Write(patch.first * 512, patch.second.data(), 512);
        view.source.Flush();
        view.patches.clear();
        if (fault)
            fault("journal-resize-home-flushed");
        replacement.start = replacement.end;
        replacement.StoreHeader();
        Adopt(replacement);
        if (fault)
            fault("journal-resize-cleared");
        evidence.complete=true;
    }
    void Commit(const std::function<void(const char *)> &fault) {
        Prune();
        if (view.patches.empty())
            return;
        Check(view.source.Writable(), "Image is read-only");
        CommitEvidence evidence;
        view.source.Flush();
        if (fault)
            fault("data-flushed");
        if (!enabled) {
            // Non-journaled volumes cannot provide crash-atomic metadata. Mark them dirty
            // before the first home write, then publish clean headers only after flush.
            auto primary = view.patches.find(2), alternate = view.patches.find((view.length - 1024) / 512);
            Check(primary != view.patches.end() && alternate != view.patches.end(),
                  "Missing transaction volume headers");
            Sector cleanPrimary = primary->second, cleanAlternate = alternate->second;
            auto dirty = view.Read(1024, 512);
            P32(dirty.data() + 4, U32(dirty.data() + 4) & ~0x100u);
            evidence.publicationStarted = true;
            view.Write(1024, dirty.data(), 512);
            view.source.Flush();
            view.patches.erase(2);
            view.patches.erase((view.length - 1024) / 512);
            view.Apply();
            view.Write(view.length - 1024, cleanAlternate.data(), 512);
            view.source.Flush();
            view.Write(1024, cleanPrimary.data(), 512);
            view.source.Flush();
            if (fault)
                fault("home-flushed");
            evidence.complete=true;
            return;
        }
        WriteLog(fault,&evidence.publicationStarted);
        view.Apply();
        if (fault)
            fault("home-flushed");
        start = end;
        StoreHeader();
        if (fault)
            fault("journal-cleared");
        evidence.complete=true;
    }
};
} // namespace Disk
} // namespace HfsPlus
