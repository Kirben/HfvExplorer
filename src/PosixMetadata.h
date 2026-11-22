// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

enum class SpecialFileType : uint8_t { None=0, Fifo=1, CharacterDevice=2, BlockDevice=6, Socket=12, Whiteout=14 };
constexpr bool IsDevice(SpecialFileType type)
{
    return type==SpecialFileType::CharacterDevice||type==SpecialFileType::BlockDevice;
}
constexpr SpecialFileType SpecialType(unsigned type)
{
    switch(type){
    case 1:return SpecialFileType::Fifo;
    case 2:return SpecialFileType::CharacterDevice;
    case 6:return SpecialFileType::BlockDevice;
    case 12:return SpecialFileType::Socket;
    case 14:return SpecialFileType::Whiteout;
    default:return SpecialFileType::None;
    }
}

struct PosixMetadata {
    static constexpr uint16_t ModeMask = 07777;
    static constexpr uint8_t UserImmutable = 1, SystemImmutable = 2,
                             UserAppendOnly = 4, SystemAppendOnly = 8;
    static constexpr uint8_t ProtectionMask = UserImmutable|SystemImmutable|UserAppendOnly|SystemAppendOnly;
    uint32_t uid, gid;
    uint16_t mode; // Permission and special bits, with the file type stored separately.

    uint8_t protection = 0;
    static constexpr uint8_t NoDump=1, Opaque=2, Archived=4, HintMask=NoDump|Opaque|Archived;
    uint8_t flags = 0;

    SpecialFileType special = SpecialFileType::None;
    uint32_t device = 0;
    bool IsSpecial() const { return special!=SpecialFileType::None; }

    bool operator==(const PosixMetadata& other) const {
        return uid == other.uid && gid == other.gid && mode == other.mode && protection == other.protection && flags == other.flags && special == other.special && device == other.device;
    }
};
