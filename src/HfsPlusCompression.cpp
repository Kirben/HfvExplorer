// SPDX-License-Identifier: GPL-2.0-or-later
// decmpfs disk header: Apple xnu/bsd/sys/decmpfs.h. Resource layouts: the
// independently documented decmpfs readers in Sleuth Kit (tsk/fs/decmpfs.c).
// Our parser is independent of those engines. Codec sources are unmodified
// zlib and Apple's BSD-licensed LZFSE/LZVN decoders (compression/README.md).
#include "HfsPlusCompression.h"
#include "HfsPlusDisk.h"
#define Z_SOLO
#define Z_PREFIX
#include "compression/zlib/zlib.h"
extern "C" {
#include "compression/lzfse/lzvn_decode_base.h"
#include "compression/libzbitmap/libzbitmap.h"
}
#include <cstdlib>
namespace HfsPlus {
namespace {
using namespace Disk;
uint32_t LE32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t LE64(const uint8_t *p) { return LE32(p) | uint64_t(LE32(p + 4)) << 32; }
void *ZAlloc(void *, unsigned n, unsigned size) { return calloc(n, size); }
void ZFree(void *, void *p) { free(p); }
Bytes Expand(const Bytes &input, size_t expected, uint32_t type) {
    Check(input.size() && input.size() <= 131072, "Invalid decmpfs compressed unit size");
    bool raw = type == 1 || type == 9 || type == 10 || ((type == 3 || type == 4) && (input[0] & 15) == 15) ||
               ((type == 7 || type == 8) && input[0] == 6) ||
               ((type == 11 || type == 12 || type == 13 || type == 14) && input[0] == 0xff);
    if (raw) {
        size_t skip = type == 1 ? 0 : 1;
        if (type == 9 || type == 10)
            Check(input[0] == 0xcc, "Invalid decmpfs stored-data marker");
        Check(input.size() - skip == expected, "Incorrect decmpfs stored-data length");
        return Bytes(input.begin() + skip, input.end());
    }
    // One extra byte prevents a decoder's 'destination full' return from being
    // mistaken for a complete stream with exactly the requested output length.
    Bytes output(expected + 1);
    if (type == 3 || type == 4) {
        z_stream s{};
        s.zalloc = ZAlloc;
        s.zfree = ZFree;
        Check(inflateInit(&s) == Z_OK, "Cannot initialize zlib decoder");
        s.next_in = const_cast<Bytef *>(input.data());
        s.avail_in = uInt(input.size());
        s.next_out = output.data();
        s.avail_out = uInt(output.size());
        int result = inflate(&s, Z_FINISH);
        // Native type-4 resource units may use Z_SYNC_FLUSH rather than
        // end a zlib stream. Exact output and exhausted input are required;
        // explicit data/checksum/dictionary errors remain failures.
        bool complete =
            result == Z_STREAM_END || (type == 4 && (result == Z_OK || result == Z_BUF_ERROR) && !s.avail_in);
        bool valid = complete && s.total_in == input.size() && s.total_out == expected;
        inflateEnd(&s);
        Check(valid, "Invalid or truncated decmpfs zlib stream");
    } else if (type == 7 || type == 8) {
        lzvn_decoder_state s{};
        s.src = input.data();
        s.src_end = s.src + input.size();
        s.dst_begin = s.dst = output.data();
        s.dst_end = s.dst + output.size();
        lzvn_decode(&s);
        Check(s.end_of_stream && (type == 8 || s.src == s.src_end) &&
                  size_t(s.dst - output.data()) == expected,
              "Invalid or truncated decmpfs LZVN stream");
    } else if (type == 11 || type == 12) {
        auto s = std::unique_ptr<lzfse_decoder_state>(new lzfse_decoder_state{});
        s->src_begin = s->src = input.data();
        s->src_end = s->src + input.size();
        s->dst_begin = s->dst = output.data();
        s->dst_end = s->dst + output.size();
        Check(lzfse_decode(s.get()) == LZFSE_STATUS_OK && s->end_of_stream && s->src == s->src_end &&
                  size_t(s->dst - output.data()) == expected,
              "Invalid or truncated decmpfs LZFSE stream");
    } else if (type == 13 || type == 14) {
        // The reference API permits trailing data and a sizing-only decode.
        // Preflight all 24-bit chunk lengths ourselves: no missing terminator,
        // ignored trailing bytes or output beyond this decmpfs unit is allowed.
        Check(input.size() >= 10 && !memcmp(input.data(), "ZBM\x09", 4), "Invalid LZBITMAP stream magic");
        size_t offset = 4, declared = 0;
        bool ended = false;
        auto u24 = [](const uint8_t *p) {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
        };
        while (offset < input.size()) {
            Check(input.size() - offset >= 6, "Truncated LZBITMAP chunk header");
            uint32_t length = u24(input.data() + offset), decoded = u24(input.data() + offset + 3);
            Check(length >= 6 && length <= input.size() - offset && decoded <= 32768 &&
                      decoded <= expected - declared,
                  "Invalid LZBITMAP chunk length");
            declared += decoded;
            offset += length;
            if (!decoded) {
                Check(length == 6 && offset == input.size(), "Invalid LZBITMAP end marker or trailing data");
                ended = true;
                break;
            }
        }
        Check(ended && declared == expected, "Incomplete LZBITMAP stream");
        size_t decoded = 0;
        Check(zbm_decompress(output.data(), output.size(), input.data(), input.size(), &decoded) == 0 &&
                  decoded == expected,
              "Invalid LZBITMAP compressed data");
    } else {
        throw Error("Unsupported decmpfs compression type " + std::to_string(type));
    }
    output.resize(expected);
    return output;
}
} // namespace
Compression::Compression(Bytes attribute, uint64_t resourceSize, Reader resource)
    : attribute_(std::move(attribute)), resource_(std::move(resource)), resourceSize_(resourceSize) {
    Check(attribute_.size() >= 16 && LE32(attribute_.data()) == 0x636d7066,
          "Missing or invalid com.apple.decmpfs header");
    type_ = LE32(attribute_.data() + 4);
    size_ = LE64(attribute_.data() + 8);
    Check(size_ <= INT64_MAX, "decmpfs logical file size exceeds Apple limit");
    Check(type_ == 1 || type_ == 3 || type_ == 4 || type_ == 7 || type_ == 8 || type_ == 9 || type_ == 10 ||
              type_ == 11 || type_ == 12 || type_ == 13 || type_ == 14,
          ("Unsupported decmpfs compression type " + std::to_string(type_)).c_str());
    Check(UsesResource() || size_ <= 65536, "Oversized inline decmpfs file");
    if (UsesResource())
        Check(attribute_.size() == 16, "Unexpected inline data in resource-based decmpfs header");
}
bool Compression::UsesResource() const {
    return type_ == 4 || type_ == 8 || type_ == 10 || type_ == 12 || type_ == 14;
}
Bytes Compression::Resource(uint64_t offset, size_t n) {
    Check(Range(offset, n, resourceSize_), "decmpfs resource offset outside fork");
    Bytes b(n);
    resource_(offset, b.data(), n);
    return b;
}
void Compression::Decode(uint64_t unit) {
    Check(unit <= size_ / 65536, "Invalid decmpfs unit index");
    size_t expected = size_t((std::min)(uint64_t(65536), size_ - unit * 65536));
    Bytes input;
    if (!UsesResource()) {
        Check(unit == 0, "Invalid inline decmpfs offset");
        input.assign(attribute_.begin() + 16, attribute_.end());
    } else {
        uint64_t count = size_ / 65536 + (size_ % 65536 != 0), offset = 0, length = 0;
        if (type_ == 4) {
            auto h = Resource(0, 16);
            uint64_t data = U32(h.data()), dataSize = U32(h.data() + 8);
            Check(data >= 16 && dataSize >= 8 && Range(data, dataSize, resourceSize_),
                  "Invalid decmpfs resource data region");
            auto prefix = Resource(data, 8);
            uint64_t bytes = U32(prefix.data());
            Check(bytes + 4 <= dataSize && LE32(prefix.data() + 4) == count &&
                      count <= (bytes >= 4 ? (bytes - 4) / 8 : 0),
                  "Invalid decmpfs zlib block table");
            uint64_t tableEnd = 4 + count * 8;
            auto record = Resource(data + 8 + unit * 8, 8);
            offset = LE32(record.data());
            length = LE32(record.data() + 4);
            Check(offset >= tableEnd && Range(offset, length, bytes), "Invalid decmpfs zlib unit range");
            if (unit) {
                auto prev = Resource(data + unit * 8, 8);
                Check(uint64_t(LE32(prev.data())) + LE32(prev.data() + 4) <= offset,
                      "Overlapping decmpfs zlib units");
            }
            offset += data + 4;
        } else {
            Check(count < UINT32_MAX / 4, "decmpfs offset table exceeds format limit");
            uint64_t tableEnd = (count + 1) * 4;
            auto first = Resource(0, 4);
            Check(LE32(first.data()) == tableEnd && tableEnd <= resourceSize_,
                  "Invalid decmpfs resource offset table");
            auto pair = Resource(unit * 4, 8);
            offset = LE32(pair.data());
            uint64_t end = LE32(pair.data() + 4);
            Check(offset >= tableEnd && end >= offset && end <= resourceSize_,
                  "Invalid decmpfs resource unit offsets");
            length = end - offset;
        }
        Check(length && length <= 131072, "Invalid decmpfs resource unit size");
        input = Resource(offset, size_t(length));
    }
    auto output = Expand(input, expected, type_);
    decoded_ = std::move(output);
    cached_ = unit;
}
uint64_t Compression::Read(uint64_t offset, void *buffer, size_t n) {
    if (offset >= size_ || !n)
        return 0;
    size_t count = size_t((std::min)(uint64_t(n), size_ - offset)), left = count;
    auto p = static_cast<uint8_t *>(buffer);
    while (left) {
        uint64_t unit = offset / 65536;
        if (cached_ != unit)
            Decode(unit);
        size_t skip = size_t(offset % 65536), part = (std::min)(left, decoded_.size() - skip);
        Check(part, "Short decmpfs decoded unit");
        memcpy(p, decoded_.data() + skip, part);
        p += part;
        offset += part;
        left -= part;
    }
    return count;
}
} // namespace HfsPlus
