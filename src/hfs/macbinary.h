// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string.h>
#include "crc.h"

enum { MacBinaryI=128, MacBinaryII=129, MacBinaryIII=130 };

static inline int macbinary_version(unsigned char* header)
{
    unsigned i;
    if(header[0]||header[74]||header[82]||!header[1]||header[1]>63||
       memchr(header+2,0,header[1])||memchr(header+2,':',header[1]))
        return 0;
    for(i=99;i<128&&!header[i];++i){}
    if(i==128)return MacBinaryI;
    if(crc_macb(header,124,0)!=((header[124]<<8)|header[125]))
        return 0;
    if(header[122]==MacBinaryIII&&header[123]<=MacBinaryIII&&!memcmp(header+102,"mBIN",4))
        return MacBinaryIII;
    if(header[122]==MacBinaryII&&header[123]<=MacBinaryII&&memcmp(header+102,"mBIN",4))
        return MacBinaryII;
    return 0;
}

static inline void macbinary_finish_header(unsigned char* header,int version)
{
    unsigned short crc;
    if(version==MacBinaryI)return;
    if(version==MacBinaryIII)memcpy(header+102,"mBIN",4);
    header[122]=(unsigned char)version;
    header[123]=MacBinaryII;
    crc=crc_macb(header,124,0);
    header[124]=(unsigned char)(crc>>8);
    header[125]=(unsigned char)crc;
}
