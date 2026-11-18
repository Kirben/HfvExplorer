// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FloppyWriter.h"
#include "FloppyWriteLease.h"
#include <vector>
// Scoped to the serialized copy worker. Browsing remains read-only.
class DirectFloppy {
 FloppyWriteLease lease_;
 FloppyWriteApi* api_;HANDLE handle_=INVALID_HANDLE_VALUE;int drive_=-1;
 BYTE* scratch_=nullptr;BYTE* pending_=nullptr;DWORD generation_=0,error_=0,pendingOffset_=0,pendingCount_=0;
 cachetype cache_={};bool finished_=false;
 bool locked_=false,writes_=false,verifyWrites_=false;
 std::vector<BYTE> expected_,dirty_;
 bool fail(DWORD error);bool verify();bool flushWrites();
public:
 explicit DirectFloppy(FloppyWriteApi* api=nullptr);
 DirectFloppy(const DirectFloppy&)=delete;DirectFloppy& operator=(const DirectFloppy&)=delete;
 ~DirectFloppy();
 bool Open(int drive,bool verifyWrites=false);bool Read(ULONGLONG offset,DWORD count,void* buffer);bool Write(ULONGLONG offset,DWORD count,const void* buffer);bool Finish();
 DWORD Error() const{return error_;}DWORD Generation()const{return generation_;}bool WritesStarted() const{return writes_;}
 static DirectFloppy* Current(int drive);
};
