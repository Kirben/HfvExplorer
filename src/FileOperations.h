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
struct OrdinaryCopy {
 std::vector<Location> sources;
 Location destination;
};
struct LegacyConversion {
 Location source, destination;
 bool directory=false, move=false, askMode=false;
 LegacyCopyOptions options;
};
class Request {
 std::variant<OrdinaryCopy, LegacyConversion, Kind> operation_;
 // Other operations retain their existing selection and destination fields.
 // For conversion these are the optional preflight selection and folder.
 std::vector<Location> otherSources_;
 Location otherTarget_;
public:
 Request() = default;
 Request(OrdinaryCopy copy) : operation_(std::move(copy)) {}
 Request(LegacyConversion conversion) : operation_(std::move(conversion)) {}
 Kind OperationKind() const {
  if (std::holds_alternative<OrdinaryCopy>(operation_)) return Kind::Copy;
  if (std::holds_alternative<LegacyConversion>(operation_)) return Kind::LegacyCopy;
  return std::get<Kind>(operation_);
 }
 void SetKind(Kind kind) {
  if (kind == Kind::LegacyCopy) throw HfsPlus::Error("Legacy conversion requires its own request payload");
  if (kind == OperationKind()) return;
  if (kind == Kind::Copy) {
   operation_ = OrdinaryCopy{std::move(otherSources_), std::move(otherTarget_)};
  } else {
   if (auto* copy = std::get_if<OrdinaryCopy>(&operation_)) {
    otherSources_ = std::move(copy->sources);
    otherTarget_ = std::move(copy->destination);
   }
   operation_ = kind;
  }
 }
 LegacyConversion* Conversion() { return std::get_if<LegacyConversion>(&operation_); }
 const LegacyConversion* Conversion() const { return std::get_if<LegacyConversion>(&operation_); }
 LegacyConversion& Legacy() { return std::get<LegacyConversion>(operation_); }
 const LegacyConversion& Legacy() const { return std::get<LegacyConversion>(operation_); }
 std::vector<Location>& Sources() {
  auto* copy = std::get_if<OrdinaryCopy>(&operation_);
  return copy ? copy->sources : otherSources_;
 }
 const std::vector<Location>& Sources() const {
  const auto* copy = std::get_if<OrdinaryCopy>(&operation_);
  return copy ? copy->sources : otherSources_;
 }
 Location& Target() {
  auto* copy = std::get_if<OrdinaryCopy>(&operation_);
  return copy ? copy->destination : otherTarget_;
 }
 const Location& Target() const {
  const auto* copy = std::get_if<OrdinaryCopy>(&operation_);
  return copy ? copy->destination : otherTarget_;
 }
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
 unsigned fork=0;
 void SetLegacySourcePath(std::string path) {
  auto& source = Legacy().source;
  source = Location::FromLegacy(source.IsClassic() ? source.ClassicImage() : std::string(), path);
 }
 void SetLegacyTargetPath(std::string path) {
  auto& destination = Legacy().destination;
  destination = Location::FromLegacy(destination.IsClassic() ? destination.ClassicImage() : std::string(), path);
 }
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
