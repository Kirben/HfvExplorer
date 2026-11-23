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
bool ReservedName(std::u16string_view);
struct Pair {
    MacBrowser::Entry companion;
    Metadata metadata;
};
std::optional<Pair> RequireVerifiedPair(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
void RemoveCompanion(MacBrowser::Volume&,const MacBrowser::Entry&);
void RefuseCut(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
void Save(MacBrowser::Volume&,uint32_t id,const Metadata&,Storage::Job&);
std::optional<Metadata> Load(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
std::optional<Metadata> Decode(const HfsPlus::AttributeData&,const Digest&,Storage::Job&);
}
