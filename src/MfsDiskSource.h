// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include "MfsImage.h"

class MfsDiskSource final : public MfsSource {
    std::shared_ptr<HfsPlus::Source> disk_;
    bool allowWrite_;
public:
    explicit MfsDiskSource(std::shared_ptr<HfsPlus::Source> disk,bool allowWrite=false)
        :disk_(std::move(disk)),allowWrite_(allowWrite){}
    uint64_t Length()const override{return disk_->Size();}
    bool Writable()const override{return allowWrite_&&disk_->Writable();}
    void DisableWrites()override{allowWrite_=false;}
    bool ReadAt(uint64_t at,void* bytes,size_t count)override{
        disk_->Read(at,bytes,count);
        return true;
    }
    bool WriteAt(uint64_t at,const void* bytes,size_t count)override{
        if(!Writable())return false;
        try{disk_->Write(at,bytes,count);return true;}
        catch(...){return false;}
    }
    bool Flush()override{
        try{disk_->Flush();return true;}
        catch(...){return false;}
    }
    bool ProcessLocal()const override{return disk_->ProcessLocal();}
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return disk_->WorkerFiles();}
};
