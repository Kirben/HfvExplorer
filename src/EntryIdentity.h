// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace MacBrowser {
struct ObjectIdentity {
    std::string key;
    std::optional<uint64_t> version;
    auto Fields()const{return std::tie(key,version);}
    bool operator==(const ObjectIdentity& other)const{return Fields()==other.Fields();}
    bool operator!=(const ObjectIdentity& other)const{return !(*this==other);}
    bool operator<(const ObjectIdentity& other)const{return Fields()<other.Fields();}
};
struct MediaIdentity {
    std::string device;
    uint32_t generation;
    auto Fields()const{return std::tie(device,generation);}
    bool operator==(const MediaIdentity& other)const{return Fields()==other.Fields();}
    bool operator!=(const MediaIdentity& other)const{return !(*this==other);}
    bool operator<(const MediaIdentity& other)const{return Fields()<other.Fields();}
};
struct EntryIdentity {
    ObjectIdentity primary, companion;
    std::optional<MediaIdentity> media;
    std::optional<uint64_t> session;

    EntryIdentity()=default;
    explicit EntryIdentity(std::string key,std::optional<uint64_t> version={})
        :primary{std::move(key),version}{}
    bool empty()const{return primary.key.empty();}
    void clear(){*this=EntryIdentity{};}
    bool SameObject(const EntryIdentity& other)const{
        return !empty()&&primary.key==other.primary.key&&media==other.media&&session==other.session;
    }
    auto Fields()const{return std::tie(primary,companion,media,session);}
    bool operator==(const EntryIdentity& other)const{return Fields()==other.Fields();}
    bool operator!=(const EntryIdentity& other)const{return !(*this==other);}
    bool operator<(const EntryIdentity& other)const{return Fields()<other.Fields();}
};
}
