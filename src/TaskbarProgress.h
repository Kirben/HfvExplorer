// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace TaskbarProgress {
struct Value{bool waiting=false;uint64_t done=0,total=0;};
struct Summary{bool active=false,waiting=false;uint64_t done=0,total=0;};
// Equal job weighting also handles mixtures of byte and file-count progress.
inline Summary Combine(const std::vector<Value>& values){
 Summary result;result.active=!values.empty();
 for(auto& value:values){if(value.waiting||!value.total)result.waiting=true;else{auto done=value.done>value.total?value.total:value.done;result.done+=uint64_t((long double)done*1000000/value.total);}result.total+=1000000;}
 return result;
}
}
