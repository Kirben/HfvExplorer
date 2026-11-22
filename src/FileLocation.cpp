// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "FileLocation.h"
#include "VolumeCoordinator.h"
#include "ftypes.h"
#include <algorithm>
namespace FileOperations {
namespace {
void Check(bool ok, const char *text) {
    if (!ok)
        throw HfsPlus::Error(text);
}
bool Absolute(const std::wstring &p) {
    return p.size() >= 3 && ((iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) ||
                             (p[0] == L'\\' && p[1] == L'\\'));
}
bool Device(const std::wstring &p) {
    return p.compare(0, 4, L"\\\\.\\") == 0 || _wcsnicmp(p.c_str(), L"\\\\?\\GLOBALROOT", 14) == 0 ||
           p.compare(0, 4, L"\\??\\") == 0;
}
std::string FullImage(const std::string &image) {
    Check(!image.empty() && image.find('\0') == image.npos, "Choose a classic image path");
    auto wide = VolumeCoord::Wide(image);
    Check(!Device(wide), "Use an image or an explicitly selected physical-media location");
    DWORD n = GetFullPathNameA(image.c_str(), 0, nullptr, nullptr);
    Check(n > 0, "Cannot resolve classic image path");
    std::string out(n, 0);
    DWORD used = GetFullPathNameA(image.c_str(), n, &out[0], nullptr);
    Check(used && used < n, "Cannot resolve classic image path");
    out.resize(strlen(out.c_str()));
    return out;
}
bool Pseudo(const std::string &p) {
    return p.size() == strlen("A:\\NO~1_2~3.DSK") && p[1] == ':' && p[2] == '\\' &&
           (!_stricmp(p.c_str() + 3, "NO~1_2~3.DSK") || !_stricmp(p.c_str() + 3, "NP~1_2~3.DSK"));
}
bool ConverterImage(const std::string &p) {
    auto n = p.size();
    return n >= 4 && p[n - 4] == '.' &&
           ((toupper((unsigned char)p[n - 3]) == 'H' && toupper((unsigned char)p[n - 2]) == 'F') ||
            !_stricmp(p.c_str() + n - 3, "DSK"));
}
void ClassicPath(const std::string &path) {
    Check(!path.empty() && path.find(':') != path.npos && path.find('\0') == path.npos,
          "Use an absolute classic catalog path");
}
} // namespace
Location Location::Windows(std::wstring path) {
    Check(Absolute(path) && path.find(L'\0') == path.npos && !Device(path),
          "Use an absolute Windows file or folder path");
    return Location(WindowsAddress{std::move(path)});
}
Location Location::Mounted(std::shared_ptr<MacBrowser::Volume> volume, uint32_t id) {
    Check(bool(volume) && id >= 2, "Use a mounted volume and a valid catalog entry");
    return Location(MountedAddress{std::move(volume), id});
}
Location Location::Image(std::string image, std::string path) {
    ::FileOperations::ClassicPath(path);
    Check(!Pseudo(image), "Use an explicit physical-media address");
    return Location(ClassicImageAddress{FullImage(image), std::move(path)});
}
Location Location::Physical(int drive, ClassicMedia media, std::string path) {
    Check(drive >= 0 && drive < 26 && (media == ClassicMedia::Floppy || media == ClassicMedia::Cd),
          "Invalid selected physical-media drive");
    ::FileOperations::ClassicPath(path);
    std::string device = "A:\\N";
    device += media == ClassicMedia::Floppy ? 'O' : 'P';
    device += "~1_2~3.DSK";
    device[0] = char('A' + drive);
    return Location(ClassicMediaAddress{drive, media, std::move(path), std::move(device), {}});
}
Location Location::Classic(std::string image, std::string path) {
    if (image.size() == strlen("A:\\NO~1_2~3.DSK") && image[1] == ':' && image[2] == '\\' &&
        (!_stricmp(image.c_str() + 3, "NO~1_2~3.DSK") || !_stricmp(image.c_str() + 3, "NP~1_2~3.DSK")))
        return Physical(toupper((unsigned char)image[0]) - 'A',
                        toupper((unsigned char)image[4]) == 'O' ? ClassicMedia::Floppy : ClassicMedia::Cd,
                        std::move(path));
    return Image(std::move(image), std::move(path));
}
Location Location::FromLegacy(const std::string &volume, const std::string &path) {
    if (ConverterImage(volume))
        return Classic(volume, path);
    auto wide = VolumeCoord::Wide(path);
    if (Absolute(wide))
        return Windows(std::move(wide));
    return Classic(volume, path);
}
const std::shared_ptr<MacBrowser::Volume> &Location::Volume() const {
    static const std::shared_ptr<MacBrowser::Volume> none;
    if (auto p = std::get_if<MountedAddress>(&address_))
        return p->volume;
    return none;
}
uint32_t Location::Id() const {
    Check(IsMounted(), "This location is not a mounted catalog entry");
    return std::get<MountedAddress>(address_).id;
}
std::optional<DWORD> Location::WindowsMediaSerial() const {
    const auto windows = std::get_if<WindowsAddress>(&address_);
    return windows ? windows->mediaSerial : std::nullopt;
}

void Location::BindWindowsMediaSerial(std::optional<DWORD> serial) {
    std::get<WindowsAddress>(address_).mediaSerial = serial;
}

const std::wstring &Location::WindowsPath() const {
    Check(IsWindows(), "This location is not a Windows path");
    return std::get<WindowsAddress>(address_).path;
}
const std::string &Location::ClassicImage() const {
    if (auto p = std::get_if<ClassicImageAddress>(&address_))
        return p->image;
    Check(IsPhysical(), "This location is not a classic image or physical medium");
    return std::get<ClassicMediaAddress>(address_).device;
}
const std::string &Location::ClassicPath() const {
    if (auto p = std::get_if<ClassicImageAddress>(&address_))
        return p->path;
    Check(IsPhysical(), "This location is not a classic catalog path");
    return std::get<ClassicMediaAddress>(address_).path;
}
int Location::Drive() const {
    Check(IsPhysical(), "This location is not physical media");
    return std::get<ClassicMediaAddress>(address_).drive;
}
ClassicMedia Location::Media() const {
    Check(IsPhysical(), "This location is not physical media");
    return std::get<ClassicMediaAddress>(address_).media;
}
bool Location::MediaGenerationKnown() const {
    return IsPhysical() && std::get<ClassicMediaAddress>(address_).generation.has_value();
}
DWORD Location::MediaGeneration() const {
    Check(MediaGenerationKnown(), "This physical medium has no captured generation");
    return *std::get<ClassicMediaAddress>(address_).generation;
}
void Location::BindMediaGeneration(DWORD value) {
    Check(IsPhysical(), "Only physical media can carry a change counter");
    std::get<ClassicMediaAddress>(address_).generation = value;
}
void Location::ClearMediaGeneration() {
    Check(IsPhysical(), "Only physical media can carry a change counter");
    std::get<ClassicMediaAddress>(address_).generation.reset();
}
std::wstring Location::HostPath() const {
    if (IsWindows())
        return WindowsPath();
    if (IsMounted())
        return Volume()->HostPath(Id());
    Check(!IsNone(), "No file location was supplied");
    return {};
}
std::string Location::ResourceKey() const {
    auto path = HostPath();
    if (!path.empty())
        return VolumeCoord::PathKey(path);
    if (IsMounted())
        return Volume()->ResourceKey();
    return expectedResource.empty()?VolumeCoord::ImageKey(ClassicImage()):expectedResource;
}
std::string Location::StorageKey() const {
    if (IsWindows())
        return VolumeCoord::HostKey(WindowsPath());
    if (IsMounted())
        return Volume()->ResourceKey();
    Check(IsClassic(), "No storage location was supplied");
    return expectedResource.empty()?VolumeCoord::ImageKey(ClassicImage()):expectedResource;
}
int Location::LegacyType() const {
    switch (kind()) {
    case Kind::Windows:
        return LIST_FAT;
    case Kind::Mounted:
        return LIST_MAC;
    case Kind::ClassicImage:
    case Kind::ClassicMedia:
        return LIST_HFS;
    default:
        return 0;
    }
}
const std::string &Location::LegacyVolume() const {
    static const std::string windows;
    if (IsWindows())
        return windows;
    Check(IsClassic(), "Legacy conversion needs a Windows or classic catalog path");
    Check(ConverterImage(ClassicImage()), "The legacy converter requires a .dsk or .hf? image name; use "
                                          "ordinary copying for other image names");
    return ClassicImage();
}
std::string Location::LegacyPath() const {
    if (IsClassic())
        return ClassicPath();
    Check(IsWindows(), "Legacy conversion needs a Windows or classic catalog path");
    BOOL lost = FALSE;
    int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, WindowsPath().c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    Check(n > 0, "Invalid Windows conversion path");
    std::string path(n, 0);
    Check(WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, WindowsPath().c_str(), -1, &path[0], n, nullptr,
                              &lost) &&
              !lost,
          "This path cannot be represented by the legacy converter");
    path.pop_back();
    return path;
}

} // namespace FileOperations
