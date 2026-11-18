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

struct OrdinaryCopy {
 std::vector<Location> sources;
 Location destination;
};
struct LegacyConversion {
 Location source, destination;
 bool directory=false, move=false, askMode=false;
 LegacyCopyOptions options;
};
struct RenameEntry {
 Location source;
 std::u16string name;
 Location parent;
 bool checkNameLock = true;
};
struct DeleteEntries {
 std::vector<Location> selection;
};
struct InspectProperties { static constexpr Kind kind = Kind::Inspect; Location item; };
struct FinderLabel { int value; };
struct FinderPosition { short x, y; };
struct ClassicPropertyEdit { hfsdirent original, desired; };
struct EditClassicProperties {
 static constexpr Kind kind = Kind::SetProperties;
 Location item;
 std::variant<ClassicPropertyEdit, FinderLabel, FinderPosition> change;
};
struct EditBasicMetadata {
 static constexpr Kind kind = Kind::SetBasicMetadata;
 Location item;
 MacBrowser::Entry original, desired;
};
struct EditAttributes {
 static constexpr Kind kind = Kind::SetAttributes;
 Location item;
 HfsPlus::Attributes original, desired;
};
struct ChangeCompression {
 static constexpr Kind kind = Kind::SetCompression;
 Location item;
 MacBrowser::Entry original;
 HfsPlus::Attributes originalAttributes;
 HfsPlus::Volume::CompressionMode method;
};
struct ImportAttributeValue {
 static constexpr Kind kind = Kind::ReadAttributeFile;
 Location item;
 std::u16string name;
};
struct ExportAttributeValue {
 static constexpr Kind kind = Kind::ExportAttribute;
 Location item; // Windows output file
 HfsPlus::AttributeData value;
};
using MetadataOperation = std::variant<InspectProperties, EditClassicProperties, EditBasicMetadata,
                                       EditAttributes, ChangeCompression, ImportAttributeValue, ExportAttributeValue>;
class Request {
 std::variant<OrdinaryCopy, LegacyConversion, RenameEntry, DeleteEntries, MetadataOperation, Kind> operation_;
 // Other operations retain their existing selection and destination fields.
 // For conversion these are the optional preflight selection and folder.
 std::vector<Location> otherSources_;
 Location otherTarget_;
public:
 Request() = default;
 Request(OrdinaryCopy copy) : operation_(std::move(copy)) {}
 Request(LegacyConversion conversion) : operation_(std::move(conversion)) {}
 Request(RenameEntry rename) : operation_(std::move(rename)) {}
 Request(DeleteEntries deletion) : operation_(std::move(deletion)) {}
 template<class T, std::enable_if_t<std::is_constructible_v<MetadataOperation, T>, int> = 0>
 Request(T metadata) : operation_(MetadataOperation(std::move(metadata))) {}
 const MetadataOperation* Metadata() const { return std::get_if<MetadataOperation>(&operation_); }
 const RenameEntry* Rename() const { return std::get_if<RenameEntry>(&operation_); }
 const DeleteEntries* Delete() const { return std::get_if<DeleteEntries>(&operation_); }
 Kind OperationKind() const {
  if (std::holds_alternative<OrdinaryCopy>(operation_)) return Kind::Copy;
  if (std::holds_alternative<LegacyConversion>(operation_)) return Kind::LegacyCopy;
  if (Rename()) return Kind::Rename;
  if (Delete()) return Kind::Delete;
  if (auto* metadata = Metadata()) return std::visit([](const auto& value) { return value.kind; }, *metadata);
  return std::get<Kind>(operation_);
 }
 void SetKind(Kind kind) {
  if (kind == Kind::LegacyCopy || kind == Kind::Rename || kind == Kind::Delete ||
      kind == Kind::Inspect || kind == Kind::SetProperties || kind == Kind::SetBasicMetadata ||
      kind == Kind::SetAttributes || kind == Kind::SetCompression ||
      kind == Kind::ReadAttributeFile || kind == Kind::ExportAttribute)
   throw HfsPlus::Error("This operation requires its own request payload");
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
