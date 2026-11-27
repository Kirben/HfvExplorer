// SPDX-License-Identifier: GPL-2.0-or-later
// Volume field definitions adapted from hfsutils, Copyright (C) 1996, 1997 Robert Leslie.
#pragma once
#include "ClassicHfsExtents.h"
#include "ClassicHfsRecordBytes.h"

typedef struct {
  int16_t	drSigWord;	/* volume signature (0x4244 for HFS) */
  long	drCrDate;	/* date and time of volume creation */
  long	drLsMod;	/* date and time of last modification */
  int16_t	drAtrb;		/* volume attributes */
  uint16_t	drNmFls;	/* number of files in root directory */
  uint16_t	drVBMSt;	/* first block of volume bit map (always 3) */
  uint16_t	drAllocPtr;	/* start of next allocation search */
  uint16_t	drNmAlBlks;	/* number of allocation blocks in volume */
  unsigned long	drAlBlkSiz;	/* size (in bytes) of allocation blocks */
  unsigned long	drClpSiz;	/* default clump size */
  uint16_t	drAlBlSt;	/* first allocation block in volume */
  long	drNxtCNID;	/* next unused catalog node ID (dir/file ID) */
  uint16_t	drFreeBks;	/* number of unused allocation blocks */
  char		drVN[28];	/* volume name (1-27 chars) */
  long	drVolBkUp;	/* date and time of last backup */
  int16_t	drVSeqNum;	/* volume backup sequence number */
  unsigned long	drWrCnt;	/* volume write count */
  unsigned long	drXTClpSiz;	/* clump size for extents overflow file */
  unsigned long	drCTClpSiz;	/* clump size for catalog file */
  uint16_t	drNmRtDirs;	/* number of directories in root directory */
  unsigned long	drFilCnt;	/* number of files in volume */
  unsigned long	drDirCnt;	/* number of directories in volume */
  long	drFndrInfo[8];	/* information used by the Finder */
  uint16_t	drVCSize;	/* size (in blocks) of volume cache */
  uint16_t	drVBMCSize;	/* size (in blocks) of volume bitmap cache */
  uint16_t	drCtlCSize;	/* size (in blocks) of common volume cache */
  unsigned long	drXTFlSize;	/* size (in bytes) of extents overflow file */
  ClassicHfsExtent	drXTExtRec[ClassicHfsExtentCount];	/* first extent record for extents file */
  unsigned long	drCTFlSize;	/* size (in bytes) of catalog file */
  ClassicHfsExtent	drCTExtRec[ClassicHfsExtentCount];	/* first extent record for catalog file */
} ClassicHfsMasterDirectory;

static inline void ClassicHfsDecodeMasterDirectory(unsigned char bytes[512], ClassicHfsMasterDirectory *mdb)
{
    mdb->drSigWord = ClassicHfsRead16(bytes + 0);
    mdb->drCrDate = ClassicHfsRead32(bytes + 2);
    mdb->drLsMod = ClassicHfsRead32(bytes + 6);
    mdb->drAtrb = ClassicHfsRead16(bytes + 10);
    mdb->drNmFls = ClassicHfsRead16(bytes + 12);
    mdb->drVBMSt = ClassicHfsRead16(bytes + 14);
    mdb->drAllocPtr = ClassicHfsRead16(bytes + 16);
    mdb->drNmAlBlks = ClassicHfsRead16(bytes + 18);
    mdb->drAlBlkSiz = ClassicHfsRead32(bytes + 20);
    mdb->drClpSiz = ClassicHfsRead32(bytes + 24);
    mdb->drAlBlSt = ClassicHfsRead16(bytes + 28);
    mdb->drNxtCNID = ClassicHfsRead32(bytes + 30);
    mdb->drFreeBks = ClassicHfsRead16(bytes + 34);
    unsigned char *name = bytes + 36;
    ClassicHfsTakeName(&name, mdb->drVN, sizeof(mdb->drVN));
    mdb->drVolBkUp = ClassicHfsRead32(bytes + 64);
    mdb->drVSeqNum = ClassicHfsRead16(bytes + 68);
    mdb->drWrCnt = ClassicHfsRead32(bytes + 70);
    mdb->drXTClpSiz = ClassicHfsRead32(bytes + 74);
    mdb->drCTClpSiz = ClassicHfsRead32(bytes + 78);
    mdb->drNmRtDirs = ClassicHfsRead16(bytes + 82);
    mdb->drFilCnt = ClassicHfsRead32(bytes + 84);
    mdb->drDirCnt = ClassicHfsRead32(bytes + 88);
    for (unsigned i = 0; i < 8; ++i)
        mdb->drFndrInfo[i] = ClassicHfsRead32(bytes + 92 + 4 * i);
    mdb->drVCSize = ClassicHfsRead16(bytes + 124);
    mdb->drVBMCSize = ClassicHfsRead16(bytes + 126);
    mdb->drCtlCSize = ClassicHfsRead16(bytes + 128);
    mdb->drXTFlSize = ClassicHfsRead32(bytes + 130);
    ClassicHfsDecodeExtents(bytes + 134, mdb->drXTExtRec);
    mdb->drCTFlSize = ClassicHfsRead32(bytes + 146);
    ClassicHfsDecodeExtents(bytes + 150, mdb->drCTExtRec);
}

static inline void ClassicHfsEncodeMasterDirectory(unsigned char bytes[512], const ClassicHfsMasterDirectory *mdb)
{
    ClassicHfsWrite16(bytes + 0, mdb->drSigWord);
    ClassicHfsWrite32(bytes + 2, mdb->drCrDate);
    ClassicHfsWrite32(bytes + 6, mdb->drLsMod);
    ClassicHfsWrite16(bytes + 10, mdb->drAtrb);
    ClassicHfsWrite16(bytes + 12, mdb->drNmFls);
    ClassicHfsWrite16(bytes + 14, mdb->drVBMSt);
    ClassicHfsWrite16(bytes + 16, mdb->drAllocPtr);
    ClassicHfsWrite16(bytes + 18, mdb->drNmAlBlks);
    ClassicHfsWrite32(bytes + 20, mdb->drAlBlkSiz);
    ClassicHfsWrite32(bytes + 24, mdb->drClpSiz);
    ClassicHfsWrite16(bytes + 28, mdb->drAlBlSt);
    ClassicHfsWrite32(bytes + 30, mdb->drNxtCNID);
    ClassicHfsWrite16(bytes + 34, mdb->drFreeBks);
    unsigned char *name = bytes + 36;
    ClassicHfsAppendName(&name, mdb->drVN, sizeof(mdb->drVN));
    ClassicHfsWrite32(bytes + 64, mdb->drVolBkUp);
    ClassicHfsWrite16(bytes + 68, mdb->drVSeqNum);
    ClassicHfsWrite32(bytes + 70, mdb->drWrCnt);
    ClassicHfsWrite32(bytes + 74, mdb->drXTClpSiz);
    ClassicHfsWrite32(bytes + 78, mdb->drCTClpSiz);
    ClassicHfsWrite16(bytes + 82, mdb->drNmRtDirs);
    ClassicHfsWrite32(bytes + 84, mdb->drFilCnt);
    ClassicHfsWrite32(bytes + 88, mdb->drDirCnt);
    for (unsigned i = 0; i < 8; ++i)
        ClassicHfsWrite32(bytes + 92 + 4 * i, mdb->drFndrInfo[i]);
    ClassicHfsWrite16(bytes + 124, mdb->drVCSize);
    ClassicHfsWrite16(bytes + 126, mdb->drVBMCSize);
    ClassicHfsWrite16(bytes + 128, mdb->drCtlCSize);
    ClassicHfsWrite32(bytes + 130, mdb->drXTFlSize);
    ClassicHfsEncodeExtents(bytes + 134, mdb->drXTExtRec);
    ClassicHfsWrite32(bytes + 146, mdb->drCTFlSize);
    ClassicHfsEncodeExtents(bytes + 150, mdb->drCTExtRec);
}
