// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
class CWnd;
void ShowFloppyWriteDialog(CWnd* parent,void (*releaseDrive)(int,void*),void* context);
