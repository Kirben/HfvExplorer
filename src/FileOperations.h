// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "MacBrowserModel.h"
#include "FileLocation.h"
#include "CopyProgress.h"
#include "ftypes.h"
extern "C" {
#include "hfs/libhfs/hfs.h"
}
namespace FileOperations {
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
 HfsPlus::Volume::CompressionMode compressionMethod=HfsPlus::Volume::CompressionMode::Zlib;
 bool checkNameLock=true;
 bool background=false;
 bool askMode=false;
 unsigned fork=0;
 // Retain the original MacBinary/BinHex/text/AppleDouble conversion route.
 Location legacySource,legacyTarget;
 void SetLegacySourcePath(std::string path){legacySource=Location::FromLegacy(legacySource.IsClassic()?legacySource.ClassicImage():std::string(),path);}
 void SetLegacyTargetPath(std::string path){legacyTarget=Location::FromLegacy(legacyTarget.IsClassic()?legacyTarget.ClassicImage():std::string(),path);}
 bool directory=false,move=false;
 LegacyCopyOptions conversion;
};
struct Change {std::string resourceKey;};
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
Storage::Result ExecuteCopyWorker(const Request&,Storage::Job&,Completion&);
std::vector<Operation> Pending();
void CancelPending();

}
