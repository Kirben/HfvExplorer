// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "HfsPlusCd.h"
#include "HfsPlusDisk.h"
#include "CdImage.h"
#include "CdMacProbe.h"
namespace HfsPlus {
int CdDriveFromRoot(const char *path) {
    if (!path || !path[0] || path[1] != ':' ||
        (path[2] && !((path[2] == '\\' || path[2] == '/') && !path[3]))) return -1;
    char c = path[0]; if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return c >= 'A' && c <= 'Z' ? c - 'A' : -1;
}

namespace {
class Cd final : public Source {
    std::shared_ptr<MediaImageSource> media_;
    uint64_t size_ = 0,base_ = 0;
    int drive_=-1;
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
    Cd(int drive, std::shared_ptr<MediaImageSource> media, uint16_t* macSignature) : media_(std::move(media)),drive_(drive) {
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
        auto tracks=media_->DataTracks();Disk::Check(!tracks.empty(),"The disc has no data tracks");
        auto latest=tracks.back();bool matched=false;
        if(macSignature)*macSignature=0;
        for(auto it=tracks.rbegin();it!=tracks.rend();++it){
            if(!media_->SelectTrack(it->number,reason))Fail(reason.c_str(),media_->Error());size_=media_->Size();base_=0;
            Disk::Check(size_&&size_%2048==0&&size_<=INT64_MAX,"Invalid CD data-track length");
            CdMedia::MacVolume volume;if(CdMedia::ProbeMac(size_,it->start*2048,[&](ULONGLONG at,DWORD n,void* p){Read(at,p,n);return true;},volume)){
                // An absolute APM is a property of the disc, not of the HFS+
                // filesystem. Expose its bounded raw volume to the engine.
                if(macSignature)*macSignature=volume.signature;
                if(volume.discAddressed||(macSignature&&volume.signature==0x4244)){
                    base_=volume.offset;
                    size_=volume.bytes;
                }
                matched=true;
                break;
            }
        }
        if(!matched){if(!media_->SelectTrack(latest.number,reason))Fail(reason.c_str(),media_->Error());size_=media_->Size();base_=0;}
    }
    ~Cd() override { media_->Close(); }
    int MediaDrive()const override{return drive_;}
    bool MediaGeneration(uint32_t& value)const override{value=generation_;return true;}
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
            if (!media_->Read(base_+offset, count, p, got))
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
std::unique_ptr<Source> OpenCd(int drive, std::shared_ptr<MediaImageSource> media, uint16_t* macSignature) {
    return std::make_unique<Cd>(drive, std::move(media), macSignature);
}
} // namespace HfsPlus
