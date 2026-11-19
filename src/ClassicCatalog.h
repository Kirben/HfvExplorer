// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include <optional>

namespace MacBrowser {
// Classic Finder's original icon and bundle readers need catalog extents.
// Other consumers use the normalized Entry returned by Volume::List.
struct ClassicCatalogEntry {
    // The legacy resource loader uses a packed catalog record with its
    // own declaration. Keep the engine's record type private.
    using PackedRecord = std::array<uint8_t,102>;
    std::string name;
    PackedRecord data{};
    std::optional<PackedRecord> folderIcon;
};
std::vector<ClassicCatalogEntry> ReadClassicCatalog(Volume&, Storage::Job*);
struct ClassicAncestor { uint32_t id, parent; std::string name; };
std::vector<ClassicAncestor> ReadClassicAncestors(Volume&,uint32_t catalogId);
}
