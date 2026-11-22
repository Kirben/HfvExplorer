// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MfsImage.h"

namespace MfsRecovery {
bool Pending(const std::wstring& path);
bool Pending(HANDLE image);
void Recover(HANDLE image,bool writable);
std::unique_ptr<MfsSource::CatalogUndo> Stage(HANDLE image,const std::vector<BYTE>& original);
}
