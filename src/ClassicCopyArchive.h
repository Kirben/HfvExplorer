// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"

namespace ClassicCopyArchive {
using Digest = std::array<uint8_t,32>;

// Bind the saved metadata to the published classic HFS object and its logical forks.
Digest Fingerprint(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
HfsPlus::AttributeData Encode(const MacBrowser::Entry&,const Digest&,Storage::Job&);
std::u16string CompanionName(uint32_t catalogId);
bool ReservedName(std::u16string_view);
void RefuseCut(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
void Save(MacBrowser::Volume&,uint32_t id,const MacBrowser::Entry&,Storage::Job&);
std::optional<MacBrowser::Entry> Load(MacBrowser::Volume&,const MacBrowser::Entry&,Storage::Job&);
std::optional<MacBrowser::Entry> Decode(const HfsPlus::AttributeData&,const Digest&,Storage::Job&);
}
