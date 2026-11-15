// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
inline bool CdImageSupportedToc(const CDROM_TOC& toc,DWORD returned) {
 const BYTE* first=toc.TrackData[0].Address;
 return returned>=FIELD_OFFSET(CDROM_TOC,TrackData)+2*sizeof(TRACK_DATA)&&toc.FirstTrack==1&&toc.LastTrack==1&&
  toc.TrackData[0].TrackNumber==1&&(toc.TrackData[0].Control&4)&&!first[0]&&!first[1]&&!first[2]&&!first[3]&&toc.TrackData[1].TrackNumber==0xaa;
}
inline unsigned long long CdImageTocBytes(const CDROM_TOC& toc) {
 const BYTE* p=toc.TrackData[1].Address;
 return ((static_cast<unsigned long long>(p[0])<<24)|(static_cast<unsigned long long>(p[1])<<16)|(static_cast<unsigned long long>(p[2])<<8)|p[3])*2048;
}
