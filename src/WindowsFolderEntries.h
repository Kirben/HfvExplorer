// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <string>

class WindowsFolderEntries {
 HANDLE handle_=INVALID_HANDLE_VALUE;
 WIN32_FIND_DATAW entry_{};
 DWORD error_=ERROR_SUCCESS;

public:
 explicit WindowsFolderEntries(const std::wstring& mask) {
  handle_=FindFirstFileW(mask.c_str(),&entry_);
  if(handle_==INVALID_HANDLE_VALUE)error_=GetLastError();
 }
 ~WindowsFolderEntries(){if(handle_!=INVALID_HANDLE_VALUE)FindClose(handle_);}
 WindowsFolderEntries(const WindowsFolderEntries&)=delete;
 WindowsFolderEntries& operator=(const WindowsFolderEntries&)=delete;

 explicit operator bool()const{return handle_!=INVALID_HANDLE_VALUE;}
 DWORD Error()const{return error_;}
 const WIN32_FIND_DATAW& Entry()const{return entry_;}
 bool Next(){
  if(FindNextFileW(handle_,&entry_))return true;
  error_=GetLastError();
  return false;
 }
};
