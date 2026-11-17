// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <string>
#include <set>
namespace VolumeCoord { namespace Detail {
struct PathIdentity {
 std::set<std::string> names,ancestors,hosts,devices;
 std::string object,namespaceName,fingerprint;
 bool directory=false,unknownLinks=false;
};
std::string Fingerprint(const PathIdentity&);
PathIdentity DescribePath(const std::wstring&);
bool PathContains(const PathIdentity&,const PathIdentity&);
bool PathOverlap(const PathIdentity&,const PathIdentity&);
}}
