// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"

namespace TransferConversion {
bool Supported(char mode);
bool ContainerMode(char mode);
bool RecognizesContainer(char mode,MacBrowser::Volume&,const MacBrowser::Entry&);
MacBrowser::Reader Adapt(char mode, bool importing, MacBrowser::Volume& source,
                         MacBrowser::Entry& entry, Storage::Job& job);
}
