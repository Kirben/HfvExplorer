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
enum class ConversionTarget { NamedPath, Folder };
struct LegacyConversion {
 Location source, destination;
 bool directory=false, move=false, askMode=false;
 LegacyCopyOptions options;
 ConversionTarget target=ConversionTarget::NamedPath;
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
struct CreateFolder { Location parent; std::u16string name; };
enum class ForkSelector { Data, Resource };
struct ExportFileFork { Location source, output; ForkSelector fork = ForkSelector::Data; };
struct ImportResourceFork { Location input, file; };
struct CreateHardLink { Location source, parent; std::u16string name; };
class Request {
 using Operation = std::variant<OrdinaryCopy, LegacyConversion, RenameEntry, DeleteEntries,
                                MetadataOperation, CreateFolder, ExportFileFork,
                                ImportResourceFork, CreateHardLink>;
 Operation operation_;
public:
 Request() = default;
 template<class T, std::enable_if_t<std::is_constructible_v<Operation, T>, int> = 0>
 Request(T operation) : operation_(std::move(operation)) {}
 template<class T> T* Get() { return std::get_if<T>(&operation_); }
 template<class T> const T* Get() const { return std::get_if<T>(&operation_); }
 const MetadataOperation* Metadata() const { return Get<MetadataOperation>(); }
 const RenameEntry* Rename() const { return Get<RenameEntry>(); }
 const DeleteEntries* Delete() const { return Get<DeleteEntries>(); }
 LegacyConversion* Conversion() { return Get<LegacyConversion>(); }
 const LegacyConversion* Conversion() const { return Get<LegacyConversion>(); }
 LegacyConversion& Legacy() { return std::get<LegacyConversion>(operation_); }
 const LegacyConversion& Legacy() const { return std::get<LegacyConversion>(operation_); }
 OrdinaryCopy& Copy() { return std::get<OrdinaryCopy>(operation_); }
 const OrdinaryCopy& Copy() const { return std::get<OrdinaryCopy>(operation_); }
 Kind OperationKind() const {
  if (Get<OrdinaryCopy>()) return Kind::Copy;
  if (Conversion()) return Kind::LegacyCopy;
  if (Rename()) return Kind::Rename;
  if (Delete()) return Kind::Delete;
  if (auto* metadata = Metadata()) return std::visit([](const auto& value) { return value.kind; }, *metadata);
  if (Get<CreateFolder>()) return Kind::CreateFolder;
  if (Get<ExportFileFork>()) return Kind::ExportFork;
  if (Get<ImportResourceFork>()) return Kind::ReplaceResource;
  return Kind::CreateHardLink;
 }
 bool background=false;
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
 HfsPlus::Attributes attributes;
 MacBrowser::Capabilities capabilities;
 MacBrowser::Entry entry;hfsdirent properties{};bool classicProperties=false;std::string description,details;
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
struct TransferSettings {
 LegacyCopyOptions conversion;
 bool askImport=false,askExport=false;
};
// Preserve source order and group adjacent ordinary copies for shared progress.
std::vector<Request> PlanTransfers(OrdinaryCopy, const TransferSettings&);

// The queue/worker owns the request until execution finishes, independently of
// the submitting browser or progress dialog. UI refresh is a separate consumer.
Operation Submit(Request);
Storage::Result ExecuteInWorker(const Request&,Storage::Job&,Completion&);
#ifdef HFV_TESTING
struct PlanAccessForTest {
 bool temporaryWorker, sameMount, read, write;
 size_t workerSources, streamedSources;
};
void PausePublicationForTest(HANDLE entered,HANDLE release);
PlanAccessForTest InspectPlanForTest(const Request&,const std::string& key,bool inWorker=false);
#endif
std::vector<Operation> Pending();
void CancelPending();
std::string CaptureLaunchVersion(const Location&, Storage::Job&);
Storage::Result ApplyLaunchEdit(const Location&, const std::string&, const std::wstring&, Storage::Job&);

}
