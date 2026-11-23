// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "HfsPlus.h"
#include <optional>

namespace Storage {class Job;}
namespace Apfs {
class VolumeCipher;
struct Container {
    uint64_t offset, blockCount, transaction, objectMap;
    uint32_t blockSize;
    std::array<uint8_t,16> uuid;
    std::vector<uint64_t> volumes;
    uint64_t checkpointBlock=0;
    uint64_t keybagBlock=0,keybagBlocks=0;
    uint64_t writeVolume=0;
    std::shared_ptr<const VolumeCipher> writeCipher;
    std::map<uint64_t,std::shared_ptr<const VolumeCipher>> siblingCiphers;
};

// Locate an APFS container and validate its latest complete contiguous checkpoint.
std::optional<Container> Probe(HfsPlus::Source&);
uint64_t ObjectChecksum(const HfsPlus::Bytes&);

struct CheckpointPublicationUncertain : HfsPlus::Error {
    using HfsPlus::Error::Error;
};
// Republish the current state at a new transaction ID. The caller must hold
// exclusive writable access to an ordinary, raw image for the whole operation.
uint64_t CommitCheckpoint(HfsPlus::Source&,const Container& expected,Storage::Job&);
std::vector<std::u16string> ListSnapshotNames(HfsPlus::Source&,const Container&,std::u16string* volumeName=nullptr);
uint64_t CreateSnapshot(HfsPlus::Source&,const Container&,const std::u16string& name,Storage::Job&);
uint64_t DeleteSnapshot(HfsPlus::Source&,const Container&,const std::u16string& name,Storage::Job&);
uint64_t RenameVolume(HfsPlus::Source&,const Container&,const std::u16string& name,Storage::Job&);
struct CloneUnavailable : HfsPlus::Error {
    using HfsPlus::Error::Error;
};
// Folder and file creation and rename support up to two catalog tree levels in native images.
// The same exclusive image ownership is required as for CommitCheckpoint.
using FinderBytes=std::array<uint8_t,32>;
inline constexpr uint32_t UserImmutable=0x00000002,SystemImmutable=0x00020000;
inline constexpr uint32_t UserAppendOnly=0x00000004,SystemAppendOnly=0x00040000;
inline constexpr uint32_t NoDump=0x00000001,Opaque=0x00000008,Archived=0x00010000;
inline constexpr uint32_t BsdHintMask=NoDump|Opaque|Archived;
inline constexpr uint32_t AppendOnlyMask=UserAppendOnly|SystemAppendOnly;
inline constexpr uint32_t WriteProtectionMask=UserImmutable|SystemImmutable|AppendOnlyMask;
inline uint32_t CopiedProtectionFlags(const std::optional<PosixMetadata>& posix)
{
    if(!posix||!posix->protection)return UserImmutable;
    return ((posix->protection&PosixMetadata::UserImmutable)?UserImmutable:0)|
           ((posix->protection&PosixMetadata::SystemImmutable)?SystemImmutable:0)|
           ((posix->protection&PosixMetadata::UserAppendOnly)?UserAppendOnly:0)|
           ((posix->protection&PosixMetadata::SystemAppendOnly)?SystemAppendOnly:0);
}
struct FileDates {
    uint64_t created,modified; // Seconds since 1904, as used by the shared browser.
    HfsPlus::AdditionalFileDates additionalDates;
    std::optional<std::array<uint64_t,4>> exact;
};
uint64_t CreateCatalogDirectory(HfsPlus::Source&,const Container& expected,uint64_t parent,const std::u16string& name,
                                Storage::Job&,std::optional<FileDates> dates={},const FinderBytes* finder=nullptr,const HfsPlus::Attributes* attributes=nullptr,bool external=false,std::optional<PosixMetadata> posix={});
uint64_t SetDirectoryDates(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                           FileDates dates,Storage::Job&);

struct TimestampEdits {
    std::optional<uint64_t> created,modified;
    HfsPlus::AdditionalFileDates additionalDates;
    std::optional<std::array<uint64_t,4>> exact;
};
// Edits fixed inode fields on supported independently formatted volumes.
uint64_t SetExternalCatalogProperties(HfsPlus::Source&,const Container&,uint64_t parent,
                                  const std::u16string& name,TimestampEdits,Storage::Job&,
                                  std::optional<bool> locked={},const FinderBytes* finder=nullptr,uint32_t lockFlags=UserImmutable,
                                  const PosixMetadata* folderPosix=nullptr,const HfsPlus::Attributes* folderAttributes=nullptr);
uint64_t RenameExternalCatalogEntry(HfsPlus::Source&,const Container&,uint64_t parent,
                                   const std::u16string& name,const std::u16string& newName,Storage::Job&,uint64_t destinationParent=0);
uint64_t SetEntryProperties(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                            const FinderBytes&,TimestampEdits,bool locked,Storage::Job&,uint32_t lockFlags=UserImmutable);
inline constexpr size_t EmbeddedAttributeLimit=3804;
uint64_t SetResourceFork(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                                 HfsPlus::Source&,uint64_t modified,Storage::Job&,bool external=false);
uint64_t SetCatalogAttributes(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                               const HfsPlus::Attributes&,Storage::Job&,bool external=false);
uint64_t CreateCatalogSymlink(HfsPlus::Source&,const Container&,uint64_t parent,const std::u16string&,
                              const HfsPlus::Bytes& target,Storage::Job&,std::optional<FileDates> dates,
                              const FinderBytes&,bool locked,const HfsPlus::Attributes&,bool external=false,
                              uint64_t replacedEntry=0,std::optional<PosixMetadata> posix={});
uint64_t ReplaceCatalogSymlink(HfsPlus::Source&,const Container&,uint64_t parent,uint64_t inode,
                               const HfsPlus::Bytes& target,Storage::Job&,std::optional<FileDates> dates,
                               const FinderBytes&,bool locked,const HfsPlus::Attributes&,bool external=false,
                               std::optional<PosixMetadata> posix={});
uint64_t CreateCatalogHardLink(HfsPlus::Source&,const Container&,uint64_t parent,uint64_t inode,
                               const std::u16string& name,Storage::Job&,uint64_t replaced=0,bool external=false,bool copiedLockedInode=false);
uint64_t CreateEmptyFile(HfsPlus::Source&,const Container& expected,uint64_t parent,const std::u16string& name,Storage::Job&);
uint64_t CreateCatalogClone(HfsPlus::Source&,const Container&,uint64_t parent,const std::u16string& name,
                            uint64_t inode,uint64_t size,std::optional<FileDates>,const FinderBytes&,Storage::Job&);

// Stream a data fork from a separate source into a newly created file.
uint64_t CreateCatalogFile(HfsPlus::Source&,const Container& expected,uint64_t parent,const std::u16string& name,
                    HfsPlus::Source& data,Storage::Job&,std::optional<FileDates> dates={},const FinderBytes* finder=nullptr,bool locked=false,const HfsPlus::Attributes* attributes=nullptr,HfsPlus::Source* resource=nullptr,const HfsPlus::Bytes* compression=nullptr,bool external=false,uint64_t replacedEntry=0,std::optional<PosixMetadata> posix={},uint64_t replacedAlias=0);
// Rename within the same directory, using on-disk inode IDs and the expected checkpoint.
uint64_t RenameEntry(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                     const std::u16string& name,Storage::Job&,uint64_t alias=0);
// Delete a regular file or an empty folder. File blocks become reusable after publication.
uint64_t DeleteEntry(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                     Storage::Job&,uint64_t alias=0,bool external=false,bool recursive=false);
// Replace a regular file's data fork in one checkpoint, preserving its inode.
uint64_t ReplaceFileData(HfsPlus::Source&,const Container& expected,uint64_t parent,uint64_t inode,
                         HfsPlus::Source& data,Storage::Job&,std::optional<FileDates> dates={},const FinderBytes* finder=nullptr,bool locked=false,const HfsPlus::Attributes* attributes=nullptr,HfsPlus::Source* resource=nullptr,const HfsPlus::Bytes* compression=nullptr,bool external=false,uint64_t alias=0,bool preserveDates=false,std::optional<PosixMetadata> posix={});
}
