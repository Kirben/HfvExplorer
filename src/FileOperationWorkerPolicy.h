// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "FileOperations.h"

namespace FileOperations {
enum class WorkerSources { None, One, Selection };
struct WorkerOperationPolicy {
    WorkerSources sources;
    unsigned destinations;
    bool needsName = false;
    bool reportsAffectedStorage = false;
    const char* sourceError = "Worker job requires exactly one source";
    const char* destinationError = "Unexpected worker destination";
    unsigned streamedAttributeSets = 0;

    static constexpr unsigned Address(Location::Kind kind){return 1u<<unsigned(kind);}
    bool AcceptsSourceCount(uint64_t count)const{
        return sources==WorkerSources::Selection?count>0:count==unsigned(sources==WorkerSources::One);
    }
    bool AcceptsDestination(Location::Kind kind)const{return (destinations&Address(kind))!=0;}
};

inline std::optional<WorkerOperationPolicy> WorkerPolicy(Kind kind)
{
    using Address=Location::Kind;
    using Count=WorkerSources;
    constexpr auto none=WorkerOperationPolicy::Address(Address::None);
    constexpr auto windows=WorkerOperationPolicy::Address(Address::Windows);
    constexpr auto mounted=WorkerOperationPolicy::Address(Address::Mounted);
    constexpr auto destination=windows|mounted|WorkerOperationPolicy::Address(Address::ClassicImage)|
        WorkerOperationPolicy::Address(Address::ClassicMedia);

    switch(kind){
    case Kind::Copy: return WorkerOperationPolicy{Count::Selection,destination,false,false,"Copy requires a source","Missing copy destination"};
    case Kind::LegacyCopy: return WorkerOperationPolicy{Count::One,destination,false,false,"Worker job requires exactly one source","Missing conversion destination"};
    case Kind::ExportFork: return WorkerOperationPolicy{Count::One,windows,false,false,"Fork jobs require exactly one source","Fork export requires a Windows output file"};
    case Kind::ReplaceResource: return WorkerOperationPolicy{Count::One,windows|mounted,false,false,"Fork jobs require exactly one source","Resource import requires a mounted or Windows destination file"};
    case Kind::Rename: return WorkerOperationPolicy{Count::One,none|mounted,true,true,"Worker job requires exactly one source","Invalid rename parent"};
    case Kind::Delete: return WorkerOperationPolicy{Count::Selection,none,false,true,"Delete requires a selection"};
    case Kind::CreateFolder: return WorkerOperationPolicy{Count::None,destination,true,true,"Folder creation has no source","Missing worker destination"};
    case Kind::CreateHardLink: return WorkerOperationPolicy{Count::One,destination,true,true,"Worker job requires exactly one source","Missing worker destination"};
    case Kind::SetProperties:
    case Kind::SetBasicMetadata:
        return WorkerOperationPolicy{Count::One,none,false,true};
    case Kind::SetAttributes:
        return WorkerOperationPolicy{Count::One,none,false,true,"Worker job requires exactly one source",
                                     "Unexpected worker destination",2};
    case Kind::SetCompression:
        return WorkerOperationPolicy{Count::One,none,false,true,"Worker job requires exactly one source",
                                     "Unexpected worker destination",1};
    default:
        return {};
    }
}
}
