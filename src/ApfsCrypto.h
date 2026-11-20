// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ApfsContainer.h"

namespace Dmg {class Password;}
namespace Apfs {
class VolumeCipher {
public:
    virtual ~VolumeCipher()=default;
    // APFS software encryption uses independent 512-byte XTS data units.
    virtual void Decrypt(uint64_t firstSector,uint8_t* data,size_t size)const=0;
};
std::shared_ptr<const VolumeCipher> UnlockSoftwareVolume(
    HfsPlus::Source&,const Container&,const std::array<uint8_t,16>& uuid,const Dmg::Password&);
}
