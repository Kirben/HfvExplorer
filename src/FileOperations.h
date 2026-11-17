// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include "CopyProgress.h"
#include "ftypes.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
}
namespace FileOperations {
// Owning backend references or absolute paths; no window, document, row or tree handles.
struct Location {
 int type=0,index=-1;uint32_t id=2;bool cut=false,directory=false;
 std::shared_ptr<MacBrowser::Volume> volume;
 std::wstring windowsPath;
 std::string classicImage,classicPath;
 uint32_t expectedCatalogId=0,expectedParent=0;
 // Bound to the selected media before asynchronous conversion questions.
 bool mediaGenerationKnown=false;DWORD mediaGeneration=0;
 std::string expectedIdentity,expectedResource;
 std::u16string expectedName;
};
enum class Kind {CreateHardLink,Copy,LegacyCopy,Rename,Delete,ExportFork,ReplaceResource,CreateFolder,Inspect,SetProperties,SetAttributes,ReadAttributeFile,ExportAttribute,SetBasicMetadata,SetCompression};
enum class MetadataPatch {All,Label,Position};
struct Request {
 Kind kind=Kind::Copy;
 std::vector<Location> sources;
 Location target;
 std::u16string name;
 MacBrowser::Entry metadata,originalMetadata;
 HfsPlus::Attributes attributes,originalAttributes;
 MetadataPatch patch=MetadataPatch::All;
 hfsdirent originalProperties{},properties{};
 int label=0;short x=0,y=0;
 bool compressed=false;
 bool checkNameLock=true;
 bool background=false;
 bool askMode=false;
 unsigned fork=0;
 // Retain the original MacBinary/BinHex/text/AppleDouble conversion route.
 std::string sourceVolume,sourcePath,targetVolume,targetPath;
 bool directory=false,move=false;
 LegacyCopyOptions conversion;
};
struct Change {
 int type=0;
 std::weak_ptr<MacBrowser::Volume> volume;
 std::wstring windowsPath;
 std::string classicImage,classicPath;
};
struct Completion {
 bool complete=false;
 unsigned completed=0;
 HfsPlus::Attributes attributes;bool supportsAttributes=false,basicMetadata=false,supportsCompression=false;
 MacBrowser::Entry entry;hfsdirent properties{};bool classicProperties=false,writable=false;std::string description;
 // Conservatively includes resources that may have changed on a partial failure.
 std::vector<Change> affected;
};
class Operation {
 struct State;
 std::shared_ptr<State> state_;
 std::shared_ptr<Storage::Job> job_;
 Operation(std::shared_ptr<State>,std::shared_ptr<Storage::Job>);
 friend Operation Submit(Request);
 friend std::vector<Operation> Pending();
 friend struct Registry;
public:
 const std::shared_ptr<Storage::Job>& Job()const{return job_;}
 Completion Snapshot()const;
};
// The queue/worker owns the request until execution finishes, independently of
// the submitting browser or progress dialog. UI refresh is a separate consumer.
Operation Submit(Request);
std::vector<Operation> Pending();
void CancelPending();

}
