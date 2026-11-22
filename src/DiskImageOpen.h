// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"

namespace Dmg { class Password; class PrivateKey; }
namespace Storage { class Job; }

namespace DiskImage {
#ifdef HFV_TESTING
extern thread_local std::function<void(const std::wstring&)> beforeReopenTest;
#endif
enum class Container { Raw, Udif, EncryptedDisk, SparseImage, SparseBundle, Ndif };

struct OpenOptions {
    bool writable = false;
    std::wstring externalJournal;
    const Dmg::Password* password = nullptr;
    const Dmg::PrivateKey* privateKey = nullptr;
    Storage::Job* job = nullptr;
    std::string expectedImage;
};

struct Opened {
    std::unique_ptr<HfsPlus::Source> source;
    Container container = Container::Raw;
};

// An unrecognized directory returns no source. Recognized invalid containers
// and unavailable credentials propagate their original diagnostic.
Opened Open(const std::wstring& path, const OpenOptions& options = {});

std::unique_ptr<HfsPlus::Source> Reopen(const std::wstring& path, Container expected,
                                     const OpenOptions& options);
}
