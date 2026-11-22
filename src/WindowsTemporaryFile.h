// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <cwchar>

// Create the file and obtain its first handle in the same operation.
inline HANDLE CreateWindowsTemporaryFile(const wchar_t* prefix,wchar_t (&path)[MAX_PATH],
                                         DWORD access,DWORD attributes)
{
    static volatile LONG serial=0;
    wchar_t directory[MAX_PATH];
    const DWORD length=GetTempPathW(MAX_PATH,directory);
    if(!length||length>=MAX_PATH||length+wcslen(prefix)+28>=MAX_PATH)return INVALID_HANDLE_VALUE;
    for(unsigned attempt=0;attempt<100;++attempt){
        if(swprintf_s(path,MAX_PATH,L"%ls%ls%08lX%08lX%08lX.tmp",directory,prefix,
                      GetCurrentProcessId(),GetTickCount(),DWORD(InterlockedIncrement(&serial)))<0)
            return INVALID_HANDLE_VALUE;
        const auto file=CreateFileW(path,access,0,nullptr,CREATE_NEW,attributes,nullptr);
        if(file!=INVALID_HANDLE_VALUE)return file;
        const auto error=GetLastError();
        if(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS)return INVALID_HANDLE_VALUE;
    }
    return INVALID_HANDLE_VALUE;
}
