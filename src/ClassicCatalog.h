// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include <optional>

namespace MacBrowser {
// Finder presentation needs the original classic catalog fields.
struct ClassicCatalogEntry {
    using PackedRecord = std::array<uint8_t,102>;
    std::string name;
    PackedRecord data{};
    std::optional<PackedRecord> folderIcon;
};
std::vector<ClassicCatalogEntry> ReadClassicCatalog(Volume&, Storage::Job*);
struct ClassicMountInfo {
    std::string label;
    uint64_t freeBytes=0;
    std::optional<DWORD> generation;
    std::string resourceKey;
};
ClassicMountInfo ReadClassicMountInfo(Volume&);
void SelectClassicCatalogFolder(Volume&,const std::string& path);
struct ClassicAncestor { uint32_t id, parent; std::string name; };
std::vector<ClassicAncestor> ReadClassicAncestors(Volume&,uint32_t catalogId);
std::vector<ClassicAncestor> ResolveClassicFolders(Volume&, const std::string& relative, bool showHidden);
std::string FindClassicApplication(Volume&, const char* type, const char* creator, bool showHidden, Storage::Job&);
void ReadClassicArtwork(Volume&, uint32_t catalogId, uint64_t offset, void*, size_t);
}
