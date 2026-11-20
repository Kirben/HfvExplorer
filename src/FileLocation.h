// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include <optional>
#include <variant>
namespace FileOperations {
struct WindowsAddress {
    std::wstring path;
    std::optional<DWORD> mediaSerial;
};
struct MountedAddress {
    std::shared_ptr<MacBrowser::Volume> volume;
    uint32_t id;
};
struct ClassicImageAddress {
    std::string image, path;
};
enum class ClassicMedia { Floppy, Cd };
struct ClassicMediaAddress {
    int drive;
    ClassicMedia media;
    std::string path, device;
    std::optional<DWORD> generation;
};
// Addresses are constructed atomically; there are no writable type tags,
// document indexes, or inactive address fields. Selection guards are metadata.
class Location {
  public:
    enum class Kind { None, Windows, Mounted, ClassicImage, ClassicMedia };
    using Address = std::variant<std::monostate, WindowsAddress, MountedAddress, ClassicImageAddress,
                                 ClassicMediaAddress>;
    Location() = default;
    static Location Windows(std::wstring);
    static Location Mounted(std::shared_ptr<MacBrowser::Volume>, uint32_t id = 2);
    static Location Image(std::string image, std::string path);
    static Location Physical(int drive, ClassicMedia, std::string path);
    // Adapter for legacy browser paths. Pseudo-device names are recognized by
    // syntax, without probing hardware; all other image paths are normalized.
    static Location Classic(std::string image, std::string path);
    static Location FromLegacy(const std::string &volume, const std::string &path);
    Kind kind() const {
        return address_.valueless_by_exception() ? Kind::None : static_cast<Kind>(address_.index());
    }
    bool IsNone() const { return kind() == Kind::None; }
    bool IsWindows() const { return kind() == Kind::Windows; }
    bool IsMounted() const { return kind() == Kind::Mounted; }
    bool IsPhysical() const { return kind() == Kind::ClassicMedia; }
    bool IsClassic() const { return kind() == Kind::ClassicImage || IsPhysical(); }
    const Address &address() const { return address_; }
    const std::shared_ptr<MacBrowser::Volume> &Volume() const;
    uint32_t Id() const;
    const std::wstring &WindowsPath() const;
    std::optional<DWORD> WindowsMediaSerial() const;
    void BindWindowsMediaSerial(std::optional<DWORD>);
    const std::string &ClassicImage() const;
    const std::string &ClassicPath() const;
    int Drive() const;
    ClassicMedia Media() const;
    bool MediaGenerationKnown() const;
    DWORD MediaGeneration() const;
    void BindMediaGeneration(DWORD);
    void ClearMediaGeneration();
    // Rebinding replaces the entire address, preserving captured selection
    // guards; worker rehydration explicitly refreshes IDs/parent guards.
    void SetWindows(std::wstring path) { address_ = Windows(std::move(path)).address_; }
    void SetMounted(std::shared_ptr<MacBrowser::Volume> volume, uint32_t id = 2) {
        address_ = Mounted(std::move(volume), id).address_;
    }
    void SetImage(std::string image, std::string path) {
        address_ = Image(std::move(image), std::move(path)).address_;
    }
    void SetClassic(std::string image, std::string path) {
        address_ = Classic(std::move(image), std::move(path)).address_;
    }
    void SetId(uint32_t id) { SetMounted(Volume(), id); }
    std::string ResourceKey() const;
    std::string StorageKey() const;
    std::wstring HostPath() const;
    // Only the old browser/converter UI needs these constants.
    int LegacyType() const;
    const std::string &LegacyVolume() const;
    std::string LegacyPath() const;
    bool cut = false, directory = false;
    bool requiresWindowsIdentity = false;
    uint32_t expectedCatalogId = 0, expectedParent = 0;
    MacBrowser::EntryIdentity expectedIdentity;
    std::string expectedResource;
    std::u16string expectedName;

  private:
    explicit Location(Address value) : address_(std::move(value)) {}
    Address address_;
};
} // namespace FileOperations
