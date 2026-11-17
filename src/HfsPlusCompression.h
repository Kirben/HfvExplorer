// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
namespace HfsPlus {
// Plans zlib decmpfs output in 64 KiB units before any filesystem mutation.
// Re-encoding validates per-unit checksums so a changing input cannot publish
// a resource table that describes different data. Memory is bounded by the table.
class ZlibCompression {
public:
    using Reader=std::function<void(uint64_t,void*,size_t)>;
    ZlibCompression(uint64_t size,Reader);
    const Bytes& Attribute()const{return attribute_;}
    bool UsesResource()const{return !units_.empty();}
    uint64_t ResourceSize()const{return resourceSize_;}
    void ReadResource(uint64_t,void*,size_t);
private:
    struct Unit{uint32_t offset,length,checksum;};
    Reader reader_;Bytes attribute_,prefix_,footer_,cached_;std::vector<Unit> units_;
    uint64_t size_=0,resourceSize_=0,cachedIndex_=UINT64_MAX;
    Bytes Encode(uint64_t);
};
// A bounded random-access decmpfs reader. One decoded 64 KiB unit is cached;
// resource tables and payloads are fetched on demand, never as a whole file.
class Compression {
  public:
    using Reader = std::function<void(uint64_t, void *, size_t)>;
    Compression(Bytes attribute, uint64_t resourceSize, Reader resource);
    uint64_t Size() const { return size_; }
    bool UsesResource() const;
    uint64_t Read(uint64_t offset, void *, size_t);

  private:
    Bytes attribute_, decoded_;
    Reader resource_;
    uint64_t size_ = 0, resourceSize_ = 0, cached_ = UINT64_MAX;
    uint32_t type_ = 0;
    Bytes Resource(uint64_t, size_t);
    void Decode(uint64_t unit);
};
} // namespace HfsPlus
