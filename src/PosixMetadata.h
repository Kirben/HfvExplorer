// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

struct PosixMetadata {
    static constexpr uint16_t ModeMask = 07777;
    uint32_t uid, gid;
    uint16_t mode; // Permission and special bits, with the file type stored separately.

    bool operator==(const PosixMetadata& other) const {
        return uid == other.uid && gid == other.gid && mode == other.mode;
    }
};
