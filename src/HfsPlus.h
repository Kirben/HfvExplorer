// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>
namespace HfsPlus {
using Bytes = std::vector<uint8_t>;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Every offset is relative to the image, never a native physical-device path.
class Source {
  public:
    virtual ~Source() = default;
    virtual uint64_t Size() const = 0;
    virtual bool Writable() const = 0;
    virtual void Read(uint64_t, void *, size_t) = 0;
    virtual void Write(uint64_t, const void *, size_t) = 0;
    virtual void Flush() = 0;
};
std::unique_ptr<Source> OpenImage(const wchar_t *, bool write);
std::unique_ptr<Source> CreateImage(const wchar_t *, uint64_t size);
struct Extent {
    uint32_t start = 0, count = 0;
};
struct Fork {
    uint64_t size = 0;
    uint32_t blocks = 0;
    std::vector<Extent> extents;
};
struct Entry {
    uint32_t id = 0, parent = 0;
    std::u16string name;
    bool directory = false, protectedFile = false;
    Fork forks[2];
    std::array<uint8_t, 32> finder{};
    uint32_t created = 0, modified = 0;
};
// Unicode 3.2 canonical decomposition, with the exclusions required by TN1150.
std::u16string Normalize(const std::u16string &, bool caseSensitive = false);
int CompareNames(const std::u16string &, const std::u16string &, bool caseSensitive = false);
class Volume {
  public:
    enum class FormatKind { HfsPlus, HfsXCaseSensitive, HfsXCaseInsensitive };
    static bool Recognizes(Source &);
    static void Format(Source &, const std::u16string &name, bool journaled = true,
                       FormatKind kind = FormatKind::HfsPlus);
    explicit Volume(Source &, bool write = false);
    ~Volume();
    std::vector<Entry> List(uint32_t parent = 2) const;
    Entry Stat(uint32_t id) const;
    uint64_t Read(uint32_t id, unsigned fork, uint64_t offset, void *, size_t);
    uint32_t MakeDirectory(uint32_t parent, const std::u16string &name);
    // Streaming, copy-on-write replacement. Source callback must return exact bytes.
    uint32_t Put(uint32_t parent, const std::u16string &name, uint64_t dataSize, uint64_t resourceSize,
                 std::function<void(unsigned, uint64_t, void *, size_t)> read,
                 const std::array<uint8_t, 32> *finder = nullptr, bool overwrite = false);
    void Rename(uint32_t id, uint32_t parent, const std::u16string &name);
    void Remove(uint32_t id);
    bool Journaled() const;
    bool IsHfsX() const;
    bool CaseSensitive() const;
    bool Writable() const;
    std::u16string Name() const;
    uint64_t FreeBytes() const;
    // Test fault boundary: callback runs after each durable commit phase.
    std::function<void(const char *)> fault;

  private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace HfsPlus
