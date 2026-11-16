// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HfsPlusCd.h"
#include "HfsPlusDisk.h"
#include "CdImage.h"
namespace HfsPlus {
namespace {
class Cd final : public Source {
    std::shared_ptr<MediaImageSource> media_;
    uint64_t size_ = 0;
    DWORD generation_ = 0;
    bool valid_ = true;
    std::string failure_;
    void Fail(const char *reason, DWORD error) {
        valid_ = false;
        failure_ = std::string(reason) + " (Windows error " +
                   std::to_string(error ? error : ERROR_IO_DEVICE) + "). Close and reopen the CD browser.";
        throw Error(failure_);
    }

  public:
    Cd(int drive, std::shared_ptr<MediaImageSource> media) : media_(std::move(media)) {
        Disk::Check(drive >= 0 && drive < 26, "Invalid CD drive");
        if (!media_)
            media_ = MakeNativeCdImageSource();
        std::string reason;
        if (!media_->Open(drive, reason))
            Fail(reason.c_str(), media_->Error());
        size_ = media_->Size();
        Disk::Check(size_ && size_ % 2048 == 0 && size_ <= INT64_MAX, "Invalid CD data-track length");
        if (!media_->Verify(generation_))
            Fail("Cannot establish CD media identity", media_->Error());
    }
    uint64_t Size() const override { return size_; }
    bool Writable() const override { return false; }
    void CheckMedia() override {
        if (!valid_)
            throw Error(failure_);
        DWORD current = 0;
        if (!media_->Verify(current))
            Fail("CD media check failed", media_->Error());
        if (current != generation_)
            Fail("CD media changed", ERROR_MEDIA_CHANGED);
    }
    void Read(uint64_t offset, void *buffer, size_t length) override {
        Disk::Check(Disk::Range(offset, length, size_) && (buffer || !length), "Read outside CD data track");
        CheckMedia();
        auto p = static_cast<uint8_t *>(buffer);
        while (length) {
            DWORD count = DWORD((std::min)(length, size_t(65536))), got = 0;
            if (!media_->Read(offset, count, p, got))
                Fail("CD sector read failed", media_->Error());
            if (got != count)
                Fail("Short CD sector read", ERROR_READ_FAULT);
            CheckMedia();
            offset += count;
            length -= count;
            p += count;
        }
    }
    void Write(uint64_t, const void *, size_t) override { throw Error("HFS+ / HFSX CDs are read-only"); }
    void Flush() override {} // no write or flush request is issued to the device
};
} // namespace
std::unique_ptr<Source> OpenCd(int drive, std::shared_ptr<MediaImageSource> media) {
    return std::unique_ptr<Source>(new Cd(drive, std::move(media)));
}
} // namespace HfsPlus
