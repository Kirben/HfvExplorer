// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
namespace HfsPlus {
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
