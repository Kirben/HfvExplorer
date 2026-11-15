// SPDX-License-Identifier: GPL-2.0-or-later
#include "ReadOnlyCd.h"
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stddef.h>

namespace {
const DWORD kSector = 2048, kTransfer = 64 * 1024;
WORD be16(const BYTE* p) { return (WORD)((p[0] << 8) | p[1]); }
DWORD be32(const BYTE* p) { return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3]; }
struct ScsiRequest { SCSI_PASS_THROUGH_DIRECT request; ULONG padding; BYTE sense[32]; };
bool sendReadCommand(HANDLE h, ScsiRequest& packet, void* buffer, DWORD bytes) {
    packet.request.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    packet.request.DataIn = SCSI_IOCTL_DATA_IN;
    packet.request.DataBuffer = buffer;
    packet.request.DataTransferLength = bytes;
    packet.request.TimeOutValue = 5;
    packet.request.SenseInfoLength = sizeof(packet.sense);
    packet.request.SenseInfoOffset = offsetof(ScsiRequest, sense);
    DWORD returned = 0;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &packet, sizeof(packet),
                         &packet, sizeof(packet), &returned, NULL)) return false;
    if (packet.request.ScsiStatus || packet.request.DataTransferLength != bytes) {
        SetLastError(ERROR_IO_DEVICE); return false;
    }
    return true;
}
}

ReadOnlyCd::ReadOnlyCd() : handle_(INVALID_HANDLE_VALUE), scsiHandle_(INVALID_HANDLE_VALUE),
    scratch_(NULL), mediaSize_(0), volumeOffset_(0), volumeSize_(0), error_(0),
    fixture_(false), passThroughUsed_(false) {
    declaredVolumeSize_ = 0; lengthSize_ = tocSize_ = scsiSize_ = 0; scsiCapacityError_ = 0; devicePath_[0] = 0;
}
ReadOnlyCd::~ReadOnlyCd() { Close(); }
void ReadOnlyCd::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    if (scsiHandle_ != INVALID_HANDLE_VALUE) CloseHandle(scsiHandle_);
    if (scratch_) VirtualFree(scratch_, 0, MEM_RELEASE);
    handle_ = scsiHandle_ = INVALID_HANDLE_VALUE; scratch_ = NULL;
    mediaSize_ = volumeOffset_ = 0; volumeSize_ = 0; error_ = 0;
    declaredVolumeSize_ = 0;
    lengthSize_ = tocSize_ = scsiSize_ = 0; scsiCapacityError_ = 0;
    fixture_ = passThroughUsed_ = false; devicePath_[0] = 0;
}
bool ReadOnlyCd::EnsureScsiHandle() {
    if (fixture_) return false;
    if (scsiHandle_ != INVALID_HANDLE_VALUE) return true;
    // Windows requires both access rights for the pass-through IOCTL. The only
    // commands issued on this handle are READ CAPACITY(10) and READ(10).
    scsiHandle_ = CreateFileA(devicePath_, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    return scsiHandle_ != INVALID_HANDLE_VALUE;
}
bool ReadOnlyCd::ScsiCapacity() {
    if (!EnsureScsiHandle()) { scsiCapacityError_ = GetLastError(); return false; }
    ScsiRequest p = {}; p.request.CdbLength = 10; p.request.Cdb[0] = 0x25;
    if (!sendReadCommand(scsiHandle_, p, scratch_, 8)) { scsiCapacityError_ = GetLastError(); return false; }
    DWORD last = be32(scratch_), block = be32(scratch_ + 4);
    if (block != kSector || last == MAXDWORD) { scsiCapacityError_ = ERROR_NOT_SUPPORTED; SetLastError(ERROR_NOT_SUPPORTED); return false; }
    scsiSize_ = ((ULONGLONG)last + 1) * block; scsiCapacityError_ = 0;
    SelectCapacity();
    return true;
}
void ReadOnlyCd::SelectCapacity() {
    // The volume/disk-length IOCTL may describe only the mounted data extent.
    // Prefer whole-medium capacity from the CD TOC or READ CAPACITY instead.
    mediaSize_ = scsiSize_ ? scsiSize_ : (tocSize_ ? tocSize_ : lengthSize_);
}
bool ReadOnlyCd::ScsiRead(DWORD sector, WORD sectors, void* buffer) {
    if (!EnsureScsiHandle()) return false;
    ScsiRequest p = {}; p.request.CdbLength = 10; p.request.Cdb[0] = 0x28;
    p.request.Cdb[2] = (BYTE)(sector >> 24); p.request.Cdb[3] = (BYTE)(sector >> 16);
    p.request.Cdb[4] = (BYTE)(sector >> 8); p.request.Cdb[5] = (BYTE)sector;
    p.request.Cdb[7] = (BYTE)(sectors >> 8); p.request.Cdb[8] = (BYTE)sectors;
    if (!sendReadCommand(scsiHandle_, p, buffer, (DWORD)sectors * kSector)) return false;
    passThroughUsed_ = true; return true;
}
bool ReadOnlyCd::OpenDrive(int drive) {
    Close();
    if (drive < 0 || drive > 25) { error_ = ERROR_INVALID_PARAMETER; return false; }
    char root[] = "A:\\"; root[0] += (char)drive;
    if (GetDriveTypeA(root) != DRIVE_CDROM) { error_ = ERROR_BAD_DEVICE; return false; }
    sprintf(devicePath_, "\\\\.\\%c:", 'A' + drive);
    handle_ = CreateFileA(devicePath_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (handle_ == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return false; }
    scratch_ = (BYTE*)VirtualAlloc(NULL, kTransfer, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!scratch_) { error_ = ERROR_NOT_ENOUGH_MEMORY; return false; }
    DWORD returned = 0; GET_LENGTH_INFORMATION length = {};
    if (DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &length, sizeof(length), &returned, NULL)
        && length.Length.QuadPart > 0) lengthSize_ = (ULONGLONG)length.Length.QuadPart;
    {
        CDROM_READ_TOC_EX query = {}; query.Format = CDROM_READ_TOC_EX_FORMAT_TOC; query.Msf = 0;
        CDROM_TOC toc = {};
        if (DeviceIoControl(handle_, IOCTL_CDROM_READ_TOC_EX, &query, sizeof(query),
                            &toc, sizeof(toc), &returned, NULL) && returned >= 12) {
            unsigned tracks = (returned - FIELD_OFFSET(CDROM_TOC, TrackData)) / sizeof(TRACK_DATA);
            for (unsigned n = 0; n < tracks && n < ARRAYSIZE(toc.TrackData); ++n)
                if (toc.TrackData[n].TrackNumber == 0xAA)
                    tocSize_ = (ULONGLONG)be32(toc.TrackData[n].Address) * kSector;
        }
    }
    SelectCapacity();
    if (!mediaSize_ && !ScsiCapacity()) { error_ = GetLastError(); return false; }
    error_ = ERROR_SUCCESS; return true;
}
#ifdef HFV_CD_TESTING
bool ReadOnlyCd::OpenFixture(const char* path, ULONGLONG reportedLength) {
    Close(); fixture_ = true;
    handle_ = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (handle_ == INVALID_HANDLE_VALUE) { error_ = GetLastError(); return false; }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle_, &size)) { error_ = GetLastError(); return false; }
    lengthSize_ = reportedLength ? reportedLength : size.QuadPart;
    tocSize_ = size.QuadPart;
    SelectCapacity();
    scratch_ = (BYTE*)VirtualAlloc(NULL, kTransfer, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!scratch_) { error_ = ERROR_NOT_ENOUGH_MEMORY; return false; }
    return true;
}
#endif
bool ReadOnlyCd::ReadSectors(ULONGLONG offset, DWORD count, void* buffer) {
    LARGE_INTEGER pos; pos.QuadPart = offset;
    DWORD got = 0;
    if (SetFilePointerEx(handle_, pos, NULL, FILE_BEGIN) && ReadFile(handle_, buffer, count, &got, NULL) && got == count)
        return true;
    DWORD fileError = GetLastError();
    if (!fixture_ && (offset / kSector) <= MAXDWORD && ScsiRead((DWORD)(offset / kSector), (WORD)(count / kSector), buffer))
        return true;
    error_ = fixture_ ? (got < count ? ERROR_HANDLE_EOF : fileError) : GetLastError();
    return false;
}
bool ReadOnlyCd::ReadBytes(ULONGLONG offset, DWORD count, void* buffer) {
    if (handle_ == INVALID_HANDLE_VALUE || !scratch_ || (!buffer && count)) { error_ = ERROR_INVALID_HANDLE; return false; }
    if (offset > mediaSize_ || count > mediaSize_ - offset) { error_ = ERROR_HANDLE_EOF; return false; }
    BYTE* out = (BYTE*)buffer;
    while (count) {
        ULONGLONG aligned = offset & ~(ULONGLONG)(kSector - 1);
        DWORD skip = (DWORD)(offset - aligned), chunk = min(count, kTransfer - skip);
        DWORD read = (skip + chunk + kSector - 1) & ~(kSector - 1);
        if (read > mediaSize_ - aligned) { error_ = ERROR_HANDLE_EOF; return false; }
        if (!ReadSectors(aligned, read, scratch_)) return false;
        memcpy(out, scratch_ + skip, chunk); out += chunk; offset += chunk; count -= chunk;
    }
    error_ = ERROR_SUCCESS; return true;
}
bool ReadOnlyCd::CheckVolume(ULONGLONG offset, ULONGLONG length, bool allowShortened) {
    BYTE mdb[512];
    if (length < 1536 || length > LONG_MAX || offset > mediaSize_ || mediaSize_ - offset < 1536 ||
        (!allowShortened && length > mediaSize_ - offset) ||
        !ReadBytes(offset + 1024, sizeof(mdb), mdb)) return false;
    ULONGLONG available = min(length, mediaSize_ - offset);
    DWORD allocation = be32(mdb + 20); WORD blocks = be16(mdb + 18), first = be16(mdb + 28);
    if (be16(mdb) != 0x4244 || !blocks || allocation < 512 || allocation % 512 ||
        mdb[36] == 0 || mdb[36] > 27 || (ULONGLONG)first * 512 + (ULONGLONG)blocks * allocation > length)
        return false;
    if (available < length) {
        // Only the read-only CD adapter accepts a shortened APM partition.
        // Require its bitmap and initial catalog/extents trees to be present;
        // never manufacture missing bytes or expand the transport's bounds.
        ULONGLONG bitmapEnd = (ULONGLONG)be16(mdb + 14) * 512 + ((blocks + 4095) / 4096) * 512;
        if (bitmapEnd > available) return false;
        const unsigned records[] = {134,150};
        for (unsigned r = 0; r < ARRAYSIZE(records); ++r) {
            for (unsigned n = 0; n < 3; ++n) {
                const BYTE* extent = mdb + records[r] + n * 4;
                DWORD start = be16(extent), count = be16(extent + 2);
                if (count && (start + count > blocks ||
                    (ULONGLONG)first * 512 + (ULONGLONG)(start + count) * allocation > available)) return false;
            }
        }
    }
    volumeOffset_ = offset; volumeSize_ = (DWORD)available; declaredVolumeSize_ = (DWORD)length; return true;
}
bool ReadOnlyCd::FindHfs() {
    volumeOffset_ = 0; volumeSize_ = declaredVolumeSize_ = 0;
    BYTE block[512];
    if (CheckVolume(0, mediaSize_)) return true;
    if (!ReadBytes(0, sizeof(block), block)) return false;
    DWORD mapBlock = be16(block) == 0x4552 ? be16(block + 2) : 512;
    if (!fixture_ && be16(block) == 0x4552 && (mapBlock == 512 || mapBlock == 2048) &&
        (ULONGLONG)be32(block + 4) * mapBlock > mediaSize_ && !scsiSize_)
        ScsiCapacity(); // Cross-check a short Windows extent; never trust disk metadata as capacity.
    if (mapBlock == 512 || mapBlock == 2048) {
        if (ReadBytes(mapBlock, sizeof(block), block) && be16(block) == 0x504D) {
            DWORD entries = be32(block + 4);
            if (entries > 4096 || (ULONGLONG)(entries + 1) * mapBlock > mediaSize_) {
                error_ = ERROR_BAD_FORMAT; return false;
            }
            for (DWORD n = 1; n <= entries; ++n) {
                if (!ReadBytes((ULONGLONG)n * mapBlock, sizeof(block), block) || be16(block) != 0x504D) break;
                if (!memcmp(block + 48, "Apple_HFS", 9) && block[57] == 0 &&
                    CheckVolume((ULONGLONG)be32(block + 8) * mapBlock, (ULONGLONG)be32(block + 12) * mapBlock, true))
                    return true;
            }
            error_ = ERROR_UNRECOGNIZED_VOLUME; return false;
        }
    }
    // Some historical hybrid CDs omit a usable map. Match the legacy search,
    // but validate MDB allocation geometry before accepting a candidate.
    ULONGLONG end = min(mediaSize_, (ULONGLONG)1024 * 1024);
    BYTE scan[32768];
    for (ULONGLONG base = 0; base < end; base += sizeof(scan)) {
        DWORD bytes = (DWORD)min((ULONGLONG)sizeof(scan), end - base);
        if (!ReadBytes(base, bytes, scan)) return false;
        for (DWORD n = 0; n + 512 <= bytes; n += 512) {
            ULONGLONG mdb = base + n;
            if (mdb >= 1024 && be16(scan + n) == 0x4244 &&
                CheckVolume(mdb - 1024, mediaSize_ - (mdb - 1024))) return true;
        }
    }
    error_ = ERROR_UNRECOGNIZED_VOLUME; return false;
}
bool ReadOnlyCd::ReadVolume(ULONGLONG offset, DWORD count, void* buffer) {
    if (!volumeSize_ || offset > volumeSize_ || count > volumeSize_ - offset) { error_ = ERROR_HANDLE_EOF; return false; }
    return ReadBytes(volumeOffset_ + offset, count, buffer);
}
