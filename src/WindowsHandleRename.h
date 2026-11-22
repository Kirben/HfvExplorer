// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

// Rename the held file without replacing an occupant at the new name.
inline bool RenameWindowsHandle(HANDLE handle,const std::wstring& path)
{
    if(path.empty()||path.size()>32767){
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    const auto nameBytes=path.size()*sizeof(wchar_t);
    const auto bytes=offsetof(FILE_RENAME_INFO,FileName)+nameBytes+sizeof(wchar_t);
    std::vector<BYTE> buffer((std::max)(bytes,sizeof(FILE_RENAME_INFO)),0);
    auto rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    rename->FileNameLength=DWORD(nameBytes);
    memcpy(rename->FileName,path.data(),nameBytes);
    return SetFileInformationByHandle(handle,FileRenameInfo,rename,DWORD(buffer.size()))!=FALSE;
}
