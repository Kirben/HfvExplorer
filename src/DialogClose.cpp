// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "DialogClose.h"
namespace DialogClose {
#ifdef HFV_TESTING
TestState test;
#endif
namespace {
bool confirming=false;
const wchar_t* kindProperty=L"HFVExplorer.DialogCloseKind";
const wchar_t* previousProperty=L"HFVExplorer.DialogClosePrevious";
const wchar_t* unicodeProperty=L"HFVExplorer.DialogCloseUnicode";
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM w,LPARAM l){
 auto previous=reinterpret_cast<WNDPROC>(GetPropW(window,previousProperty));bool unicode=GetPropW(window,unicodeProperty)!=nullptr;
 if(message==WM_SYSCOMMAND&&(w&0xfff0)==SC_CLOSE){
  auto value=reinterpret_cast<UINT_PTR>(GetPropW(window,kindProperty));
  auto kind=value?static_cast<Kind>(value-1):Kind::Dialog;
  // Guard the user's system-close command, not explicit buttons or teardown.
  if(kind!=Kind::Dialog&&(!Confirm(window,kind)||!IsWindow(window)))return 0;
 }
 LRESULT result=previous?(unicode?CallWindowProcW(previous,window,message,w,l):CallWindowProcA(previous,window,message,w,l)):
  (unicode?DefWindowProcW(window,message,w,l):DefWindowProcA(window,message,w,l));
 // The window is being destroyed: do not overwrite a later subclass in the
 // chain. Keep our properties alive while previous procedures handle teardown.
 if(message==WM_NCDESTROY){RemovePropW(window,kindProperty);RemovePropW(window,previousProperty);RemovePropW(window,unicodeProperty);}
 return result;
}
void Attach(HWND window){
 if(GetPropW(window,previousProperty))return;bool unicode=IsWindowUnicode(window)!=FALSE;
 auto previous=reinterpret_cast<WNDPROC>(unicode?GetWindowLongPtrW(window,GWLP_WNDPROC):GetWindowLongPtrA(window,GWLP_WNDPROC));
 if(!previous||!SetPropW(window,previousProperty,reinterpret_cast<HANDLE>(previous)))return;
 if(unicode&&!SetPropW(window,unicodeProperty,reinterpret_cast<HANDLE>(UINT_PTR(1)))){RemovePropW(window,previousProperty);return;}
 SetLastError(0);auto replaced=unicode?SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Procedure)):SetWindowLongPtrA(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Procedure));
 if(!replaced&&GetLastError()){RemovePropW(window,previousProperty);RemovePropW(window,unicodeProperty);}
}
}
void SetKind(HWND window,Kind kind){if(kind!=Kind::Dialog)Attach(window);SetPropW(window,kindProperty,reinterpret_cast<HANDLE>(UINT_PTR(kind)+1));}
bool Confirm(HWND window,Kind kind){
 if(confirming)return false; // A confirmation must never recursively confirm itself.
#ifdef HFV_TESTING
 ++test.prompts;test.kind=kind;test.owner=window;if(test.answer!=-1)return test.answer==IDYES;
#endif
 const wchar_t* text=kind==Kind::Copy?L"Cancel this operation? The window will remain open until it can stop and clean up safely.":
  kind==Kind::CopyMode?L"Cancel this copy operation before choosing a copy mode?":
  kind==Kind::Browser?L"Close this browser window?":
  kind==Kind::BackgroundBrowser?L"Close this browser window? Background file operations will continue in the remaining windows.":
  kind==Kind::LastBrowser?L"Close the last browser and cancel all outstanding file operations? HFVExplorer will wait for safe cleanup before exiting.":L"Close this dialog?";
 confirming=true;int answer=MessageBoxW(window,text,L"Confirm close",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2);confirming=false;return answer==IDYES;
}
}
