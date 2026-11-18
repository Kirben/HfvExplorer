// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <stdint.h>
namespace HfsPlus {
// Copyable immutable snapshots. Large values are streamed to an owned, anonymous
// temporary file. Copies share that snapshot; modifying small data detaches it.
class AttributeData {
 struct Storage;std::shared_ptr<Storage> storage_;
 std::vector<uint8_t>& Small();const std::vector<uint8_t>& Small()const;
public:
 using Reader=std::function<void(uint64_t,void*,size_t)>;
 AttributeData();AttributeData(const std::vector<uint8_t>&);AttributeData(std::vector<uint8_t>&&);
 AttributeData(std::initializer_list<uint8_t> bytes):AttributeData(std::vector<uint8_t>(bytes)){}
 static AttributeData Snapshot(uint64_t,Reader,bool fileBacked=false);
 uint64_t size()const;bool empty()const{return !size();}bool FileBacked()const;
 void Read(uint64_t,void*,size_t)const;
 AttributeData Patch(uint64_t,const std::vector<uint8_t>&)const;
 bool operator==(const AttributeData&)const;bool operator!=(const AttributeData& b)const{return !(*this==b);}
 // Small structured metadata only. Streaming callers use Read instead.
 operator std::vector<uint8_t>()const{return Small();}
 const uint8_t* data()const{return Small().data();}uint8_t* data(){return Small().data();}
 auto begin()const->std::vector<uint8_t>::const_iterator{return Small().begin();}
 auto end()const->std::vector<uint8_t>::const_iterator{return Small().end();}
 auto begin()->std::vector<uint8_t>::iterator{return Small().begin();}
 auto end()->std::vector<uint8_t>::iterator{return Small().end();}
 uint8_t operator[](size_t n)const{return Small().at(n);}uint8_t& operator[](size_t n){return Small().at(n);}
 uint8_t back()const{return Small().back();}void push_back(uint8_t n){Small().push_back(n);}void pop_back(){Small().pop_back();}
};
}
