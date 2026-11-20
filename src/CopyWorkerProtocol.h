// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"
#include "WorkerEndpoint.h"

namespace CopyWorkerProtocol {
using FileOperations::Location;
using FileOperations::Kind;
using FileOperations::LegacyConversion;
using FileOperations::EditClassicProperties;
using FileOperations::EditBasicMetadata;
using FileOperations::ChangeCompression;
using FileOperations::ForkSelector;
using MacBrowser::Entry;
using MacBrowser::WorkerOrigin;

constexpr uint64_t protocol = 15;
constexpr DWORD maximumFrame = 1024 * 1024;
enum Message : DWORD {
    RequestPacket = 1,
    File,
    Progress,
    Conflict,
    Choice,
    Cancel,
    Fixture,
    Mapping,
    IoGate,
    AttributeSet,
    AttributeHeader,
    AttributeChunk,
    AffectedStorage,
    SessionMount,
    SessionCommand,
    SessionEntry,
    SessionBytes,
    SessionResult,
    SessionProgress,
    SessionContinue,
    SessionReadRequest,
    Finished
};
void Check(bool ok, const char *message);
struct Data {
    std::vector<uint8_t> bytes;
    size_t at = 0;
    void put(uint64_t value) {
        for (unsigned i = 0; i < 8; ++i)
            bytes.push_back(uint8_t(value >> (i * 8)));
    }
    uint64_t get() {
        Check(at <= bytes.size() && bytes.size() - at >= 8, "Truncated copy-worker packet");
        uint64_t out = 0;
        for (unsigned i = 0; i < 8; ++i)
            out |= uint64_t(bytes[at++]) << (i * 8);
        return out;
    }
    bool flag() {
        auto value = get();
        Check(value <= 1, "Invalid copy-worker boolean");
        return value != 0;
    }
    void put(const std::string &s) {
        put(uint64_t(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
    std::string text() {
        auto n = get();
        Check(n <= bytes.size() - at, "Invalid copy-worker string length");
        std::string out(reinterpret_cast<const char *>(bytes.data() + at), size_t(n));
        at += size_t(n);
        return out;
    }
    void wide(const std::wstring &s) {
        put(std::string(reinterpret_cast<const char *>(s.data()), s.size() * sizeof(wchar_t)));
    }
    std::wstring wide() {
        auto s = text();
        Check(s.size() % 2 == 0, "Invalid copy-worker UTF-16 string");
        std::wstring out(s.size() / 2, 0);
        if (!s.empty())
            memcpy(&out[0], s.data(), s.size());
        return out;
    }
    void u16(const std::u16string &s) { wide(MacBrowser::Wide(s)); }
    std::u16string u16() { return MacBrowser::U16(wide()); }
    void end() { Check(at == bytes.size(), "Trailing copy-worker packet data"); }
};
struct Endpoint {
    Location location;
    WorkerOrigin origin;
    std::vector<Entry> path;
};
struct WireEndpoint {
    Location::Kind kind = Location::Kind::None;
    Location location;
    WorkerOrigin origin;
    uint32_t selectedId = 0;
    std::vector<Entry> path;
};
struct WireConversion {
    bool directory=false,move=false;
    FileOperations::ConversionTarget target=FileOperations::ConversionTarget::NamedPath;
    LegacyCopyOptions options;
};
struct WireRequest {
    Kind kind = Kind::Copy;
    HANDLE parent = nullptr;
    unsigned fault = 0;
    std::optional<WireConversion> legacy;
    ForkSelector fork = ForkSelector::Data;
    std::u16string name;
    bool checkNameLock = true;
    std::optional<EditClassicProperties> classic;
    std::optional<EditBasicMetadata> basic;
    std::optional<ChangeCompression> compression;
    std::vector<WireEndpoint> sources;
    WireEndpoint target;
};

bool SupportsOperation(Kind kind);
DWORD FrameLimit(DWORD message);
Message ValidateFrame(const DWORD (&header)[3]);
void EncodeEntry(Data &data, const Entry &entry);
Entry DecodeEntry(Data &data);
void EncodeClassic(Data&, const hfsdirent&);
hfsdirent DecodeClassic(Data&);
void EncodeOutcome(Data &data, const Storage::Result &result);
void DecodeOutcome(Data &data, Storage::Result &result);
void EncodeProgress(Data &data, const Storage::Progress &progress);
Storage::Progress DecodeProgress(Data &data);
void EncodeOrigin(Data &data, const WorkerOrigin &origin);
WorkerOrigin DecodeOrigin(Data&);
void EncodeEndpoint(Data &data, const Endpoint &endpoint);
WireEndpoint ParseEndpoint(Data &data, bool allowMounted = true);
Data EncodeRequest(const FileOperations::Request &request, HANDLE parent, unsigned fault,
                   const std::vector<Endpoint> &sources, const Endpoint &target);
WireRequest DecodeRequest(Data &data);
} // namespace CopyWorkerProtocol
