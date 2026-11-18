// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "FileOperations.h"
#include "CopyProcess.h"
#include "BrowserTransfers.h"
#include <cstdio>
#include <type_traits>
namespace {
using namespace FileOperations;
void Check(bool ok, const char *text) {
    if (!ok)
        throw HfsPlus::Error(text);
}
template <class F> void Reject(F action, const char *text) {
    try {
        action();
    } catch (const std::exception &) {
        return;
    }
    throw HfsPlus::Error(text);
}
struct Memory final : MacBrowser::Volume {
    unsigned calls = 0, renamedParent = 0;
    std::u16string Name() const override { return u"Typed fixture"; }
    const char *Kind() const override { return "test"; }
    bool Writable() const override { return true; }
    bool CaseSensitive() const override { return false; }
    uint64_t FreeBytes() const override { return 0; }
    std::string ResourceKey() const override { return "typed-location:memory"; }
    std::vector<MacBrowser::Entry> List(uint32_t) override {
        ++calls;
        return {};
    }
    MacBrowser::Entry Stat(uint32_t id) override {
        ++calls;
        MacBrowser::Entry e;
        e.id = id;
        e.parent = 17;
        e.name = u"Before";
        return e;
    }
    void Read(uint32_t, unsigned, uint64_t, void *, size_t) override {
        ++calls;
        throw HfsPlus::Error("Unexpected fixture read");
    }
    uint32_t MakeDirectory(uint32_t, const std::u16string &) override {
        ++calls;
        throw HfsPlus::Error("Unexpected fixture write");
    }
    uint32_t Put(uint32_t, const MacBrowser::Entry &, MacBrowser::Reader, bool) override {
        ++calls;
        throw HfsPlus::Error("Unexpected fixture write");
    }
    void Rename(uint32_t, uint32_t parent, const std::u16string &) override {
        ++calls;
        renamedParent = parent;
    }
    void Remove(uint32_t) override {
        ++calls;
        throw HfsPlus::Error("Unexpected fixture delete");
    }
};
template <class T, class = void> struct HasType : std::false_type {};
template <class T> struct HasType<T, std::void_t<decltype(std::declval<T>().type)>> : std::true_type {};
template <class T, class = void> struct HasIndex : std::false_type {};
template <class T> struct HasIndex<T, std::void_t<decltype(std::declval<T>().index)>> : std::true_type {};
static_assert(!HasType<Location>::value && !HasIndex<Location>::value,
              "Storage locations must not carry mutable UI routing");
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<Location>().address())>>,
              "Address payloads must be immutable to callers");
void Wait(const Operation &op) {
    Check(WaitForSingleObject(op.Job()->event(), 10000) == WAIT_OBJECT_0, "Typed request timed out");
}
} // namespace
int RunTypedLocationsTest(const char *directory) {
    if (!CreateDirectoryA(directory, nullptr))
        return 2;
    std::string root = directory;
    FILE *report = fopen((root + "\\typed-locations-report.txt").c_str(), "w");
    if (!report)
        return 2;
    int result = 0;
    try {
        auto path = MacBrowser::Wide(u"C:\\Typed\\\u4e2d\u6587.txt");
        auto windows = Location::Windows(path);
        Check(windows.IsWindows() && windows.WindowsPath() == path && !windows.Volume(),
              "Windows address mismatch");
        Reject([&] { windows.ClassicImage(); }, "Windows location exposed a classic image");
        Reject([&] { windows.Id(); }, "Windows location exposed a catalog ID");
        Reject([&] { windows.BindMediaGeneration(7); }, "Windows path accepted media identity");
        for (auto invalid : {L"", L"relative.txt", L"C:relative.txt", L"\\\\.\\PhysicalDrive0",
                             L"\\\\?\\GLOBALROOT\\Device\\Harddisk0"})
            Reject([&] { Location::Windows(invalid); }, "Invalid Windows address accepted");
        Reject([&] { windows.SetWindows(L"relative.txt"); }, "Invalid rebind accepted");
        Check(windows.WindowsPath() == path, "Failed rebind destroyed the old location");
        auto image = Location::Classic(root + "\\image.dsk", "Disk:Folder:File");
        Check(image.kind() == Location::Kind::ClassicImage && image.ClassicPath() == "Disk:Folder:File",
              "Classic image address");
        Reject([&] { Location::Classic(root + "\\image.dsk", "relative"); },
               "Relative classic path accepted");
        Reject([] { Location::Image("Z:\\NO~1_2~3.DSK", ":"); },
               "Physical pseudo-path accepted as a regular image");
        auto unmapped = Location::Image(root + "\\ordinary.img", ":");
        Check(unmapped.kind() == Location::Kind::ClassicImage, "Ordinary image address rejected");
        Reject([&] { unmapped.LegacyVolume(); }, "Legacy adapter accepted a contradictory routing hint");
        Check(Location::Windows(L"C:\\folder.dsk\\file").LegacyVolume().empty(),
              "Windows folder name changed converter routing");
        auto floppy = Location::Physical(0, ClassicMedia::Floppy, "Disk:");
        Check(floppy.IsPhysical() && floppy.ClassicImage() == "A:\\NO~1_2~3.DSK" &&
                  !floppy.MediaGenerationKnown(),
              "Floppy identity");
        floppy.BindMediaGeneration(0);
        Check(floppy.MediaGenerationKnown() && floppy.MediaGeneration() == 0, "Zero media generation lost");
        auto clone = floppy;
        floppy.ClearMediaGeneration();
        Check(!floppy.MediaGenerationKnown() && clone.MediaGenerationKnown(),
              "Media snapshot changed through a copy");
        auto cd = Location::Classic("d:\\NP~1_2~3.DSK", "CD:File");
        Check(cd.IsPhysical() && cd.Drive() == 3 && cd.Media() == ClassicMedia::Cd, "CD pseudo-path adapter");
        Reject([] { Location::Physical(26, ClassicMedia::Floppy, ":"); }, "Out-of-range drive accepted");
        Reject([] { Location::Mounted({}, 2); }, "Null catalog backend accepted");
        auto backend = std::make_shared<Memory>();
        Reject([&] { Location::Mounted(backend, 0); }, "Invalid catalog identifier accepted");
        auto mounted = Location::Mounted(backend, 99);
        mounted.cut = mounted.directory = true;
        mounted.expectedIdentity = "selected";
        mounted.SetMounted(backend, 101);
        Check(mounted.Id() == 101 && mounted.cut && mounted.directory &&
                  mounted.expectedIdentity == "selected" && !backend->calls,
              "Rebinding changed guards or performed I/O");
        {
            auto owned = std::make_shared<Memory>();
            std::weak_ptr<Memory> weak = owned;
            auto first = Location::Mounted(owned, 2);
            auto second = first;
            owned.reset();
            first = {};
            Check(!weak.expired(), "Captured backend lifetime ended early");
            second = {};
            Check(weak.expired(), "Discarded locations retained backend");
        }
        for (auto location : {windows, image, clone, cd}) {
            location.cut = true;
            location.directory = true;
            location.expectedIdentity = "identity";
            location.expectedName = u"Name";
            location.expectedCatalogId = 19;
            location.expectedParent = 17;
            auto bytes = CopyProcess::EncodeLocationForTest(location);
            auto restored = CopyProcess::DecodeLocationForTest(bytes);
            Check(restored.kind() == location.kind() && restored.cut && restored.directory &&
                      restored.expectedIdentity == location.expectedIdentity &&
                      restored.expectedName == location.expectedName && restored.expectedCatalogId == 19 &&
                      restored.expectedParent == 17,
                  "Typed wire round trip lost selection guards");
            if (location.IsWindows())
                Check(restored.WindowsPath() == location.WindowsPath(), "Wire Windows path");
            else {
                Check(restored.ClassicImage() == location.ClassicImage() &&
                          restored.ClassicPath() == location.ClassicPath(),
                      "Wire classic address");
                Check(restored.MediaGenerationKnown() == location.MediaGenerationKnown(),
                      "Wire media binding");
            }
            for (size_t n = 0; n < bytes.size(); ++n) {
                auto truncated = std::vector<uint8_t>(bytes.begin(), bytes.begin() + n);
                Reject([&] { CopyProcess::DecodeLocationForTest(truncated); },
                       "Truncated typed packet accepted");
            }
            auto invalid = bytes;
            invalid[0] = 255;
            Reject([&] { CopyProcess::DecodeLocationForTest(invalid); }, "Unknown typed packet tag accepted");
            invalid = bytes;
            invalid.push_back(0);
            Reject([&] { CopyProcess::DecodeLocationForTest(invalid); },
                   "Trailing typed packet data accepted");
        }
        fprintf(report, "PASS: typed worker wire round trips, selection guards, truncated packets, unknown "
                        "tags and trailing-data rejection without opening media\n");
        BrowserTransfers::Publish({{Location::Mounted(backend, 99)}, true}, false);
        Reject([] { BrowserTransfers::Publish({{Location{}}, true}, false); },
               "Clipboard accepted a missing address");
        Check(BrowserTransfers::Snapshot().items.size() == 1 &&
                  BrowserTransfers::Snapshot().items.front().IsMounted(),
              "Invalid publication changed the clipboard");
        Check(BrowserTransfers::Choose(BrowserTransfers::Snapshot(), Location{}, true, 0) ==
                  BrowserTransfers::Effect::None,
              "Drop accepted a missing destination");
        BrowserTransfers::Clear();
        fprintf(report, "PASS: exclusive typed addresses, immutable payloads, Unicode paths, invalid "
                        "combinations, media snapshots, lifetime ownership and no browser indexes\n");
        Request copy = OrdinaryCopy{{windows}, Location::Mounted(backend)};
        Check(copy.OperationKind() == Kind::Copy && !copy.Conversion() &&
                  copy.Sources().size() == 1 && copy.Target().IsMounted(),
              "Ordinary copy was not captured with its own inputs");
        Reject([&] { copy.SetKind(Kind::LegacyCopy); },
               "Legacy conversion accepted a bare operation tag");
        Request conversion = LegacyConversion{
            Location::Image(root + "\\converter.dsk", "Fixture:File"), windows,
            false, false, true, {}};
        Check(conversion.OperationKind() == Kind::LegacyCopy && conversion.Conversion() &&
                  conversion.Legacy().askMode && conversion.Legacy().destination.IsWindows(),
              "Legacy conversion did not retain its typed payload");
        auto captured = conversion;
        conversion.Legacy().options.output = 'r';
        Check(captured.Legacy().options.output == 'a',
              "Editing conversion options changed a captured request");
        Request missing;
        missing.Sources() = {Location{}};
        missing.Target() = Location::Mounted(backend);
        auto before = backend->calls;
        auto bad = Submit(missing);
        Wait(bad);
        Check(bad.Job()->result().code && backend->calls == before, "Missing source reached a backend");
        missing.Sources() = {Location::Mounted(backend, 99)};
        missing.Target() = {};
        bad = Submit(missing);
        Wait(bad);
        Check(bad.Job()->result().code && backend->calls == before, "Missing destination reached a backend");
        auto unknown = missing;
        unknown.SetKind(static_cast<Kind>(999));
        bad = Submit(unknown);
        Wait(bad);
        Check(bad.Job()->result().code && backend->calls == before,
              "Unknown operation reached a mutation backend");
        auto emptyDelete = Submit(DeleteEntries{});
        Wait(emptyDelete);
        Check(emptyDelete.Job()->result().code && backend->calls == before,
              "Empty delete reached a backend");
        auto blankRename = Submit(RenameEntry{Location::Mounted(backend, 99), {}});
        Wait(blankRename);
        Check(blankRename.Job()->result().code && backend->calls == before,
              "Blank rename reached a backend");
        Request rename = FileOperations::RenameEntry{Location::Mounted(backend, 99), u"After"};
        auto changed = Submit(rename);
        Wait(changed);
        Check(!changed.Job()->result().code && backend->renamedParent == 17,
              "Rename with no explicit target moved the file to a root");
        Check(!changed.Snapshot().affected.empty() &&
                  changed.Snapshot().affected.front().resourceKey == backend->ResourceKey(),
              "Typed change notification");
        fprintf(report, "PASS: incomplete requests fail before backend access; rename retains its original "
                        "parent; notifications carry storage identities\n");
    } catch (const std::exception &e) {
        fprintf(report, "FAIL: %s\n", e.what());
        result = 1;
    }
    fclose(report);
    return result;
}
#endif
