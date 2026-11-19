// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "TransferConversion.h"
#include <algorithm>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
extern "C" {
#include "hfs/binhex.h"
#include "hfs/macbinary.h"
}

namespace TransferConversion {
namespace {
using namespace MacBrowser;

void Check(bool ok, const char* message) {
    if (!ok) throw HfsPlus::Error(message);
}
void Cancel(Storage::Job& job) {
    if (job.cancelled()) throw Storage::Cancelled();
}

class Spool {
    int fd_ = -1;
    uint64_t size_ = 0;
public:
    Spool() {
        wchar_t folder[MAX_PATH], file[MAX_PATH];
        auto length = GetTempPathW(MAX_PATH, folder);
        Check(length && length < MAX_PATH && GetTempFileNameW(folder, L"hfc", 0, file),
              "Cannot make conversion staging file");
        fd_ = _wopen(file, _O_RDWR | _O_BINARY | _O_TEMPORARY | _O_NOINHERIT);
        if (fd_ < 0) {
            DeleteFileW(file);
            throw HfsPlus::Error("Cannot open conversion staging file");
        }
    }
    Spool(const Spool&) = delete;
    ~Spool() { _close(fd_); }
    int Descriptor() const { return fd_; }
    uint64_t Size() const { return size_; }
    void Measure() {
        auto length = _filelengthi64(fd_);
        Check(length >= 0, "Cannot measure conversion stream");
        size_ = static_cast<uint64_t>(length);
    }
    void Write(const void* bytes, size_t count) {
        Check(_write(fd_, bytes, static_cast<unsigned>(count)) == static_cast<int>(count),
              "Cannot write conversion staging data");
        size_ += count;
    }
    void Seek(uint64_t offset) {
        Check(offset <= INT64_MAX && _lseeki64(fd_, static_cast<int64_t>(offset), SEEK_SET) >= 0,
              "Cannot seek conversion staging file");
    }
    void Read(uint64_t offset, void* output, size_t count) {
        Check(offset <= size_ && count <= size_ - offset, "Invalid conversion stream range");
        Seek(offset);
        Check(_read(fd_, output, static_cast<unsigned>(count)) == static_cast<int>(count),
              "Cannot read conversion staging data");
    }
};

template<class Sink>
void Stream(Volume& volume, uint32_t id, unsigned fork, uint64_t size, Storage::Job& job, Sink consume) {
    std::vector<uint8_t> block(65536);
    for (uint64_t offset = 0; offset < size;) {
        Cancel(job);
        const auto count = size_t((std::min)(uint64_t(block.size()), size - offset));
        volume.Read(id, fork, offset, block.data(), count);
        consume(block.data(), count);
        offset += count;
    }
    Cancel(job);
}

uint16_t Word(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Long(const uint8_t* p) { return uint32_t(Word(p)) << 16 | Word(p + 2); }
void Word(uint8_t* p, uint16_t value) { p[0] = uint8_t(value >> 8); p[1] = uint8_t(value); }
void Long(uint8_t* p, uint32_t value) { Word(p, uint16_t(value >> 16)); Word(p + 2, uint16_t(value)); }
uint64_t Padded(uint64_t size) { return (size + 127) & ~uint64_t(127); }

std::u16string MacName(const uint8_t* name, size_t size) {
    Check(size && !memchr(name, 0, size) && !memchr(name, ':', size), "Invalid encoded Macintosh filename");
    wchar_t wide[64]{};
    const auto count = MultiByteToWideChar(10000, 0, reinterpret_cast<const char*>(name),
                                           int(size), wide, ARRAYSIZE(wide));
    Check(count > 0, "Cannot decode Macintosh filename");
    return U16(std::wstring(wide, count));
}
std::string EncodedName(const std::u16string& name, size_t maximum) {
    auto wide = Wide(name);
    char bytes[64]{};
    BOOL lossy = FALSE;
    auto count = WideCharToMultiByte(10000, 0, wide.data(), int(wide.size()), bytes, int(maximum),
                                     nullptr, &lossy);
    Check(count > 0 && !lossy && !memchr(bytes, 0, count) && !memchr(bytes, ':', count),
          "Filename cannot be represented in the conversion format");
    return std::string(bytes, count);
}

Reader ReadSpool(std::shared_ptr<Spool> spool, uint64_t resourceOffset = 0) {
    return [spool, resourceOffset](unsigned fork, uint64_t offset, void* data, size_t count) {
        spool->Read((fork ? resourceOffset : 0) + offset, data, count);
    };
}

Reader Text(bool importing, Volume& volume, Entry& entry, Storage::Job& job) {
    auto spool = std::make_shared<Spool>();
    bool pendingCR = false;
    bool ended = false;
    Stream(volume, entry.id, 0, entry.forks[0].size, job, [&](const uint8_t* bytes, size_t count) {
        std::vector<uint8_t> converted;
        converted.reserve(count * 2 + 1);
        for (size_t i = 0; i < count && !ended; ++i) {
            auto byte = bytes[i];
            if (!importing) {
                if (byte == '\r') byte = '\n';
                if (byte == '\n') converted.push_back('\r');
                converted.push_back(byte);
            } else {
                if (pendingCR) {
                    converted.push_back('\r');
                    pendingCR = false;
                    if (byte == '\n') continue;
                }
                if (byte == '\r') pendingCR = true;
                else if (byte == 0x1a) ended = true; // Legacy DOS text EOF.
                else converted.push_back(byte == '\n' ? '\r' : byte);
            }
        }
        if (!converted.empty()) spool->Write(converted.data(), converted.size());
    });
    if (pendingCR) {
        const uint8_t cr = '\r';
        spool->Write(&cr, 1);
    }
    if (importing) {
        const std::u16string extension = u".txt";
        if (entry.name.size() > extension.size() &&
            entry.name.compare(entry.name.size() - extension.size(), extension.size(), extension) == 0)
            entry.name.resize(entry.name.size() - extension.size());
        memcpy(entry.finder.data(), "TEXTUNIX", 8);
    } else entry.name += u".txt";
    entry.forks[0].size = spool->Size();
    return [spool, &volume, id = entry.id](unsigned fork, uint64_t offset, void* data, size_t count) {
        if (fork) volume.Read(id, fork, offset, data, count);
        else spool->Read(offset, data, count);
    };
}

void Carrier(Entry& entry, const char16_t* extension, uint64_t size) {
    entry.name += extension;
    entry.forks[0].size = size;
    entry.forks[1].size = 0;
    entry.finder = {};
    entry.protectedFile = false;
    entry.portableContainer = true;
}

Reader MacBinary(char mode, bool importing, Volume& volume, Entry& entry) {
    std::array<uint8_t, 128> header{};
    const auto id = entry.id;
    if (importing) {
        const auto length = entry.forks[0].size;
        Check(length >= header.size(), "Truncated MacBinary header");
        volume.Read(id, 0, 0, header.data(), header.size());
        const auto version=macbinary_version(header.data());
        Check(version,"Invalid MacBinary signature, version or header CRC");
        const bool v1=version==MacBinaryI;
        const bool roman=version!=MacBinaryIII||!(header[106]&0x7f);
        Check(roman||std::all_of(header.begin()+2,header.begin()+2+header[1],[](auto byte){return byte<128;}),
              "MacBinary filename uses an unsupported Macintosh script");
        const uint64_t data = Long(header.data() + 83), resource = Long(header.data() + 87);
        const uint64_t start = 128 + (v1 ? 0 : Padded(Word(header.data() + 120)));
        const uint64_t resourceAt = start + Padded(data);
        Check(start + data <= length && (!resource || resourceAt + resource <= length),
              "Truncated MacBinary forks");
        entry.name = MacName(header.data() + 2, header[1]);
        entry.forks[0].size = data;
        entry.forks[1].size = resource;
        entry.finder = {};
        memcpy(entry.finder.data(), header.data() + 65, 8);
        entry.finder[8] = header[73] & ~3;
        entry.finder[9] = v1 ? 0 : (header[101] & ~1);
        memcpy(entry.finder.data() + 10, header.data() + 75, 4);
        if(version==MacBinaryIII){
            memcpy(entry.finder.data()+14,header.data()+79,2);
            entry.finder[24]=header[106];
            entry.finder[25]=header[107];
        }
        entry.protectedFile = (header[81] & 1) != 0;
        entry.created = Long(header.data() + 91);
        entry.modified = Long(header.data() + 95);
        entry.datesValid = true;
        return [&volume, id, start, resourceAt](unsigned fork, uint64_t offset, void* out, size_t count) {
            volume.Read(id, 0, (fork ? resourceAt : start) + offset, out, count);
        };
    }
    auto name = EncodedName(entry.name, 63);
    const uint64_t data = entry.forks[0].size, resource = entry.forks[1].size;
    Check(data <= UINT32_MAX && resource <= UINT32_MAX && entry.created <= UINT32_MAX &&
              entry.modified <= UINT32_MAX, "File size or dates exceed MacBinary fields");
    header[1] = uint8_t(name.size());
    memcpy(header.data() + 2, name.data(), name.size());
    memcpy(header.data() + 65, entry.finder.data(), 8);
    header[73] = entry.finder[8];
    memcpy(header.data() + 75, entry.finder.data() + 10, 4);
    header[81] = entry.protectedFile ? 1 : 0;
    Long(header.data() + 83, uint32_t(data));
    Long(header.data() + 87, uint32_t(resource));
    Long(header.data() + 91, uint32_t(entry.created));
    Long(header.data() + 95, uint32_t(entry.modified));
    const auto version=mode=='5'?MacBinaryI:mode=='3'?MacBinaryIII:MacBinaryII;
    if(version!=MacBinaryI)header[101]=entry.finder[9];
    if(version==MacBinaryIII){
        memcpy(header.data()+79,entry.finder.data()+14,2);
        if(!(entry.finder[24]&0x7f))header[106]=entry.finder[24];
        header[107]=entry.finder[25];
    }
    macbinary_finish_header(header.data(),version);
    const uint64_t resourceAt = 128 + Padded(data);
    Carrier(entry, u".bin", resourceAt + Padded(resource));
    return [&volume, id, header, data, resource, resourceAt](unsigned, uint64_t at, void* out, size_t count) {
        auto bytes = static_cast<uint8_t*>(out);
        memset(bytes, 0, count);
        auto overlap = [&](uint64_t start, uint64_t length, auto read) {
            auto low = (std::max)(at, start), high = (std::min)(at + count, start + length);
            if (low < high) read(low - start, bytes + (low - at), size_t(high - low));
        };
        overlap(0, header.size(), [&](uint64_t offset, void* p, size_t n) { memcpy(p, header.data() + offset, n); });
        overlap(128, data, [&](uint64_t offset, void* p, size_t n) { volume.Read(id, 0, offset, p, n); });
        overlap(resourceAt, resource, [&](uint64_t offset, void* p, size_t n) { volume.Read(id, 1, offset, p, n); });
    };
}

void BinCheck(bool okay) {
    Check(okay, bh_error ? bh_error : "Invalid BinHex stream");
}
struct BinSession {
    bool importing;
    bool active = false;
    ~BinSession() { if (active) Finish(); }
    int Finish() {
        active = false;
        return importing ? bh_close() : bh_end();
    }
};

Reader BinHex(bool importing, Volume& volume, Entry& entry, Storage::Job& job) {
    auto output = std::make_shared<Spool>();
    if (importing) {
        Spool input;
        Stream(volume, entry.id, 0, entry.forks[0].size, job, [&](auto bytes, auto n) { input.Write(bytes, n); });
        input.Seek(0);
        BinSession session{true};
        BinCheck(bh_open(input.Descriptor()) == 0);
        session.active = true;
        const auto read = [](void* bytes, int count) { BinCheck(bh_read(bytes, count) == count); };
        uint8_t length = 0, name[64]{}, fields[18]{};
        read(&length, 1);
        Check(length > 0 && length <= 63, "Invalid BinHex filename length");
        read(name, length + 1);
        Check(name[length] == 0, "Invalid BinHex filename terminator");
        entry.name = MacName(name, length);
        read(fields, sizeof(fields));
        BinCheck(bh_readcrc() == 0);
        entry.finder = {};
        memcpy(entry.finder.data(), fields, 10);
        entry.forks[0].size = Long(fields + 10);
        entry.forks[1].size = Long(fields + 14);
        std::vector<uint8_t> block(65536);
        for (unsigned fork = 0; fork < 2; ++fork) {
            for (uint64_t remaining = entry.forks[fork].size; remaining;) {
                Cancel(job);
                auto size = int((std::min)(remaining, uint64_t(block.size())));
                read(block.data(), size);
                output->Write(block.data(), size);
                remaining -= size;
            }
            BinCheck(bh_readcrc() == 0);
        }
        BinCheck(session.Finish() == 0);
        return ReadSpool(output, entry.forks[0].size);
    }
    auto name = EncodedName(entry.name, 63);
    Check(entry.forks[0].size <= UINT32_MAX && entry.forks[1].size <= UINT32_MAX,
          "File size exceeds BinHex fork fields");
    BinSession session{false};
    BinCheck(bh_start(output->Descriptor()) == 0);
    session.active = true;
    const auto insert = [](void* bytes, int count) { BinCheck(bh_insert(bytes, count) == 0); };
    uint8_t length = uint8_t(name.size()), fields[18]{};
    memcpy(fields, entry.finder.data(), 10);
    Long(fields + 10, uint32_t(entry.forks[0].size));
    Long(fields + 14, uint32_t(entry.forks[1].size));
    insert(&length, 1);
    insert(name.data(), int(name.size()) + 1);
    insert(fields, sizeof(fields));
    BinCheck(bh_insertcrc() == 0);
    for (unsigned fork = 0; fork < 2; ++fork) {
        Stream(volume, entry.id, fork, entry.forks[fork].size, job,
               [&](auto bytes, auto n) { insert(bytes, int(n)); });
        BinCheck(bh_insertcrc() == 0);
    }
    BinCheck(session.Finish() == 0);
    output->Measure();
    Carrier(entry, u".hqx", output->Size());
    return ReadSpool(output);
}
}

bool ContainerMode(char mode) { return mode=='m'||mode=='5'||mode=='3'||mode=='b'; }
bool Supported(char mode) { return ContainerMode(mode)||mode=='t'; }

bool RecognizesContainer(char mode,Volume& volume,const Entry& entry)
{
    std::array<uint8_t,128> header{};
    const auto bytes=size_t((std::min)(entry.forks[0].size,uint64_t(header.size())));
    if(!bytes)return false;
    volume.Read(entry.id,0,0,header.data(),bytes);
    if(mode=='m')return bytes==header.size()&&macbinary_version(header.data())!=0;
    constexpr std::string_view preamble="(This file must be converted with BinHex 4.0)";
    const std::string_view text(reinterpret_cast<const char*>(header.data()),bytes);
    return text.find(preamble)!=text.npos;
}

Reader Adapt(char mode, bool importing, Volume& source, Entry& entry, Storage::Job& job) {
    if (entry.directory) return {};
    Cancel(job);
    auto reader = mode == 't' ? Text(importing, source, entry, job) :
                  mode == 'b' ? BinHex(importing, source, entry, job) : MacBinary(mode, importing, source, entry);
    entry.hardLink = false;
    entry.compressed = false;
    entry.linkCount = 1;
    return reader;
}
}
