// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlusDisk.h"

namespace DiskImage {
class Slice final : public HfsPlus::Source {
    std::shared_ptr<Source> disk_;
    const uint64_t start_,length_;
    const bool allowWrite_;
public:
    Slice(std::shared_ptr<Source> disk,uint64_t start,uint64_t length,bool allowWrite=false)
        :disk_(std::move(disk)),start_(start),length_(length),allowWrite_(allowWrite){
        HfsPlus::Disk::Check(disk_&&HfsPlus::Disk::Range(start_,length_,disk_->Size()),
                            "Partition lies outside the disk");
    }
    uint64_t Size()const override{return length_;}
    bool Writable()const override{return allowWrite_&&disk_->Writable();}
    bool ProcessLocal()const override{return disk_->ProcessLocal();}
    void CheckMedia()override{disk_->CheckMedia();}
    int MediaDrive()const override{return disk_->MediaDrive();}
    bool MediaGeneration(uint32_t& generation)const override{return disk_->MediaGeneration(generation);}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return disk_->WorkerFiles();}
    void Read(uint64_t at,void* bytes,size_t count)override{
        HfsPlus::Disk::Check(HfsPlus::Disk::Range(at,count,length_),"Read outside the disk partition");
        disk_->Read(start_+at,bytes,count);
    }
    void Write(uint64_t at,const void* bytes,size_t count)override{
        HfsPlus::Disk::Check(Writable(),"The disk partition is read-only");
        HfsPlus::Disk::Check(HfsPlus::Disk::Range(at,count,length_),"Write outside the disk partition");
        disk_->Write(start_+at,bytes,count);
    }
    void Flush()override{if(Writable())disk_->Flush();}
};
}
