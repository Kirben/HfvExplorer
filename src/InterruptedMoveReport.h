// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ShlObj.h>
#include <string>
#ifdef HFV_TESTING
#include <functional>
#endif

namespace FileOperations {
#ifdef HFV_TESTING
enum class MoveReportFault { None, ShortWrite, Flush };
extern thread_local std::function<MoveReportFault(const std::string&)> moveReportFault;
#endif
inline std::wstring InterruptedMoveFolder()
{
#ifdef HFV_TESTING
    wchar_t scratch[MAX_PATH]{};
    const auto length=GetEnvironmentVariableW(L"HFV_TEST_MOVE_REPORT_DIR",scratch,MAX_PATH);
    if(length&&length<MAX_PATH)return scratch;
    const auto temp=GetTempPathW(MAX_PATH,scratch);
    if(temp&&temp<MAX_PATH)return std::wstring(scratch)+L"HFVExplorer\\Interrupted Moves";
#endif
    wchar_t local[MAX_PATH]{};
    if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,local)))return {};
    return std::wstring(local)+L"\\HFVExplorer\\Interrupted Moves";
}
}
