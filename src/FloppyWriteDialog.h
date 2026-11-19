// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "StorageWorker.h"
#include "CommandStorage.h"
#include <vector>
#include <optional>
#include <variant>
class BrowserSession;
class FloppyWriteApi;
std::function<Storage::Result(Storage::Job&)> MakeFloppyWriteJob(int drive,std::vector<BYTE> image,FloppyWriteApi* api=nullptr,CommandStorage::Reservation held={},std::optional<DWORD> generation={});
struct NewFloppyImage {std::string label;};
struct ExistingFloppyImage {std::string path;};
using FloppyImageInput=std::variant<NewFloppyImage,ExistingFloppyImage>;
struct PreparedFloppyImage {
 std::vector<BYTE> bytes;
 DWORD generation=0;
};
std::function<Storage::Result(Storage::Job&)> MakeFloppyPreparationJob(int drive,FloppyImageInput,
                                               std::shared_ptr<PreparedFloppyImage>);
bool StartFloppyWrite(BrowserSession&,int drive,FloppyImageInput);
