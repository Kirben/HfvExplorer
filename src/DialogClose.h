// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
namespace DialogClose {
enum class Kind {Dialog,Copy,CopyMode,Browser,BackgroundBrowser,LastBrowser,Quit,QuitWithWork};
void SetKind(HWND,Kind);
bool Confirm(HWND,Kind);
#ifdef HFV_TESTING
struct TestState{int answer=-1;unsigned prompts=0;Kind kind=Kind::Dialog;HWND owner=nullptr;};
extern TestState test;
#endif
}
