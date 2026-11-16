// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <stdexcept>
namespace BrowserPresentation {
class Batch {
    HWND window_;LONG_PTR sort_;
public:
    explicit Batch(HWND window,bool list=false):window_(window),sort_(list?(GetWindowLongPtr(window,GWL_STYLE)&(LVS_SORTASCENDING|LVS_SORTDESCENDING)):0){
        SendMessage(window_,WM_SETREDRAW,FALSE,0);
        if(sort_)SetWindowLongPtr(window_,GWL_STYLE,GetWindowLongPtr(window_,GWL_STYLE)&~sort_);
    }
    ~Batch(){if(IsWindow(window_)){if(sort_)SetWindowLongPtr(window_,GWL_STYLE,GetWindowLongPtr(window_,GWL_STYLE)|sort_);
        SendMessage(window_,WM_SETREDRAW,TRUE,0);RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);}}
};
inline std::wstring WindowsPath(const std::wstring& path) {
    if(path.compare(0,4,L"\\\\?\\")==0)return path;
    wchar_t full[32768];DWORD n=GetFullPathNameW(path.c_str(),ARRAYSIZE(full),full,nullptr);
    if(!n||n>=ARRAYSIZE(full))throw std::runtime_error("Windows path is too long");
    std::wstring out(full,n);
    return out.compare(0,2,L"\\\\")==0?L"\\\\?\\UNC\\"+out.substr(2):L"\\\\?\\"+out;
}
inline void Cell(HWND list,int row,int column,const std::wstring& text) {
    LVITEMW value{};value.iSubItem=column;value.pszText=const_cast<wchar_t*>(text.c_str());
    SendMessageW(list,LVM_SETITEMTEXTW,row,reinterpret_cast<LPARAM>(&value));
}
inline std::wstring Text(HWND window) {
    int n=GetWindowTextLengthW(window);std::wstring text(size_t(n)+1,0);
    GetWindowTextW(window,&text[0],n+1);text.resize(n);return text;
}
inline void UnicodeControl(HWND parent,int id,const wchar_t* kind) {
    HWND old=GetDlgItem(parent,id);if(IsWindowUnicode(old))return;RECT rect;GetWindowRect(old,&rect);
    MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&rect),2);auto text=Text(old);
    DWORD style=DWORD(GetWindowLongPtr(old,GWL_STYLE)),ex=DWORD(GetWindowLongPtr(old,GWL_EXSTYLE));
    HWND previous=GetWindow(old,GW_HWNDPREV);
    HFONT font=reinterpret_cast<HFONT>(SendMessage(old,WM_GETFONT,0,0));DestroyWindow(old);
    HWND replacement=CreateWindowExW(ex,kind,text.c_str(),style,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,
        parent,reinterpret_cast<HMENU>(INT_PTR(id)),AfxGetInstanceHandle(),nullptr);
    if(!replacement)throw std::runtime_error("Cannot create Unicode browser control");
    SendMessage(replacement,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    SetWindowPos(replacement,previous,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
inline void UnicodeEdit(HWND parent,int id){UnicodeControl(parent,id,L"EDIT");}
inline std::string Utf8(const std::u16string& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,reinterpret_cast<const wchar_t*>(s.data()),int(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,0);if(n)WideCharToMultiByte(CP_UTF8,0,reinterpret_cast<const wchar_t*>(s.data()),int(s.size()),&out[0],n,nullptr,nullptr);return out;
}
inline std::wstring FromUtf8(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);std::wstring out(n,0);
    if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),&out[0],n);return out;
}
}
