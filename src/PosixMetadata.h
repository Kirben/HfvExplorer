// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

struct PosixMetadata {
    static constexpr uint16_t ModeMask = 07777;
    static constexpr uint8_t UserImmutable = 1, SystemImmutable = 2,
                             UserAppendOnly = 4, SystemAppendOnly = 8;
    static constexpr uint8_t ProtectionMask = UserImmutable|SystemImmutable|UserAppendOnly|SystemAppendOnly;
    uint32_t uid, gid;
    uint16_t mode; // Permission and special bits, with the file type stored separately.

    uint8_t protection = 0;

    bool operator==(const PosixMetadata& other) const {
        return uid == other.uid && gid == other.gid && mode == other.mode && protection == other.protection;
    }
};
