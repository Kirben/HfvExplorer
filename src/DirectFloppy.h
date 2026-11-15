// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FloppyWriter.h"
// Scoped to the serialized copy worker. Browsing remains read-only.
class DirectFloppy {
 FloppyWriteApi* api_;HANDLE handle_=INVALID_HANDLE_VALUE;int drive_=-1;
 BYTE* scratch_=nullptr;BYTE* pending_=nullptr;DWORD generation_=0,error_=0,pendingOffset_=0,pendingCount_=0;
 cachetype cache_={};bool locked_=false,writes_=false;
 bool fail(DWORD error);bool verify();bool flushWrites();
public:
 explicit DirectFloppy(FloppyWriteApi* api=nullptr);
 DirectFloppy(const DirectFloppy&)=delete;DirectFloppy& operator=(const DirectFloppy&)=delete;
 ~DirectFloppy();
 bool Open(int drive);bool Read(ULONGLONG offset,DWORD count,void* buffer);bool Write(ULONGLONG offset,DWORD count,const void* buffer);bool Finish();
 DWORD Error() const{return error_;}bool WritesStarted() const{return writes_;}
 static DirectFloppy* Current(int drive);
};
