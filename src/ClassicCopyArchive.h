// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"

namespace ClassicCopyArchive {
using Digest = std::array<uint8_t,32>;
struct Metadata {
    MacBrowser::Entry entry;
    std::optional<HfsPlus::Bytes> symbolicLinkTarget;
    std::string linkGroup;
    uint32_t compressionType=0;
    Metadata()=default;
    explicit Metadata(const MacBrowser::Entry& source):entry(source){}
};

uint64_t EncodedSize(const Metadata&,Storage::Job&);
HfsPlus::AttributeData Encode(const Metadata&,const Digest&,Storage::Job&,const MacBrowser::Entry& projected);
std::u16string CompanionName(uint32_t catalogId);
std::u16string ExpectationName(uint32_t catalogId);
bool ReservedName(std::u16string_view);
bool IsExpectationRecord(const MacBrowser::Entry&);
struct Pair {
    MacBrowser::Entry companion;
    Metadata metadata;
    std::optional<MacBrowser::Entry> expectation;
};
std::optional<Pair> RequireVerifiedPair(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
void RemoveCompanion(MacBrowser::Volume&,const Pair&);
bool ValidateCut(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
void RemoveArchive(MacBrowser::Volume&,uint32_t,Storage::Job&,bool batchActive,bool deletionProgress=false);
void Save(MacBrowser::Volume&,uint32_t id,const Metadata&,Storage::Job&);
std::optional<Metadata> Load(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&,const char** copyWarning=nullptr);
std::optional<Metadata> Decode(const HfsPlus::AttributeData&,const Digest&,Storage::Job&);
}
