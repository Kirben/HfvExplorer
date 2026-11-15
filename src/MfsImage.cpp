// SPDX-License-Identifier: GPL-2.0-or-later
// Independent implementation of the MFS disk structures documented by Apple.
#include "MfsImage.h"
#include <algorithm>
#include <set>
#include <limits>

static unsigned be16(const BYTE* p) { return unsigned(p[0])*256+p[1]; }
static uint32_t be32(const BYTE* p) { return uint32_t(be16(p))*65536+be16(p+2); }
static void put16(BYTE* p,unsigned v) {p[0]=BYTE(v>>8);p[1]=BYTE(v);}
static void put32(BYTE* p,uint32_t v) {put16(p,v>>16);put16(p+2,v);}

MfsImageSource::MfsImageSource():file_(INVALID_HANDLE_VALUE),length_(0) {}
MfsImageSource::~MfsImageSource() {if(file_!=INVALID_HANDLE_VALUE) CloseHandle(file_);}
bool MfsImageSource::Open(const wchar_t* path,std::string& error) {
    if(file_!=INVALID_HANDLE_VALUE) {CloseHandle(file_);file_=INVALID_HANDLE_VALUE;}
    length_=0;
    // No physical-device namespace, write access, sharing for writers or driver.
    if(!path || wcsncmp(path,L"\\\\.\\",4)==0 || wcsncmp(path,L"\\\\?\\",4)==0) {error="Choose a regular disk image file.";return false;}
    file_=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file_==INVALID_HANDLE_VALUE) {error="Could not open the image for read-only access.";return false;}
    LARGE_INTEGER size;
    if(GetFileType(file_)!=FILE_TYPE_DISK || !GetFileSizeEx(file_,&size) || size.QuadPart<0) {
        CloseHandle(file_);file_=INVALID_HANDLE_VALUE;error="Could not determine image length.";return false;
    }
    length_=size.QuadPart;return true;
}
bool MfsImageSource::ReadAt(uint64_t offset,void* bytes,size_t length) {
    if(file_==INVALID_HANDLE_VALUE || offset>length_ || length>length_-offset || length>MAXDWORD) return false;
    LARGE_INTEGER position;position.QuadPart=offset;DWORD got;
    return SetFilePointerEx(file_,position,NULL,FILE_BEGIN) && ReadFile(file_,bytes,DWORD(length),&got,NULL) && got==length;
}
bool MfsImage::Recognizes(MfsSource& source) {
    BYTE signature[2],header[84];
    if(source.ReadAt(1024,signature,2) && be16(signature)==0xd2d7) return true;
    return source.ReadAt(0,header,84) && header[0]<=63 && be16(header+82)==0x0100 &&
        source.ReadAt(84+1024,signature,2) && be16(signature)==0xd2d7;
}
bool MfsImage::Fail(const char* error) {error_=error;return false;}
bool MfsImage::Read(uint64_t offset,void* bytes,size_t length) {
    if(offset>length_ || length>length_-offset || !source_->ReadAt(base_+offset,bytes,length)) return Fail("Image is truncated or unreadable.");
    return true;
}
bool MfsImage::Chain(MfsFork& fork,uint16_t first,std::vector<bool>& claimed) {
    if(fork.physicalLength%allocationSize_ || fork.length>fork.physicalLength ||
       fork.physicalLength/allocationSize_>map_.size()) return Fail("Invalid fork length.");
    unsigned blocks=fork.physicalLength/allocationSize_;
    if(!blocks) return first==0 || Fail("Empty fork has an allocation chain.");
    for(unsigned i=0;i<blocks;++i) {
        if(first<2 || first>=map_.size()+2 || first==0xfff || claimed[first-2] || map_[first-2]==0xfff)
            return Fail("Invalid, cyclic or cross-linked allocation chain.");
        claimed[first-2]=true;fork.blocks.push_back(first);
        first=map_[first-2];
    }
    return first==1 || Fail("Allocation chain does not match the fork length.");
}
bool MfsImage::Open(MfsSource& source) {
    source_=nullptr;entries_.clear();map_.clear();name_.clear();error_.clear();
    allocationSize_=0;freeBlocks_=0;base_=0;length_=source.Length();
    BYTE mdb[64],header[84];
    bool raw=source.ReadAt(1024,mdb,sizeof(mdb)) && be16(mdb)==0xd2d7;
    if(!raw) {
        if(!source.ReadAt(0,header,sizeof(header)) || header[0]>63 || be16(header+82)!=0x0100 ||
           !source.ReadAt(84+1024,mdb,sizeof(mdb)) || be16(mdb)!=0xd2d7) return Fail("Not a supported MFS disk image.");
        uint64_t data=be32(header+64),tags=be32(header+68);
        if(data%512 || data<1536 || data>source.Length()-84 || tags>source.Length()-84-data)
            return Fail("Invalid Disk Copy 4.2 container lengths.");
        base_=84;length_=data;
    }
    source_=&source;
    unsigned count=be16(mdb+18),directory=be16(mdb+14),directoryBlocks=be16(mdb+16);
    unsigned fileCount=be16(mdb+12),nameLength=mdb[36];
    allocationSize_=be32(mdb+20);allocationStart_=uint64_t(be16(mdb+28))*512;
    freeBlocks_=be16(mdb+34);
    size_t mapBytes=(count*12+7)/8;
    uint64_t directoryStart=uint64_t(directory)*512,directoryEnd=directoryStart+uint64_t(directoryBlocks)*512;
    if(!count || count>4093 || !allocationSize_ || allocationSize_%512 || freeBlocks_>count ||
       !nameLength || nameLength>27 || !directoryBlocks || directoryStart<1024+64+mapBytes ||
       directoryEnd>length_ || allocationStart_<1024+64+mapBytes ||
       allocationStart_>length_ || uint64_t(count)*allocationSize_>length_-allocationStart_)
        return Fail("Invalid MFS volume layout.");
    name_.assign(reinterpret_cast<char*>(mdb+37),nameLength);
    if(name_.find('\0')!=std::string::npos || name_.find(':')!=std::string::npos) return Fail("Invalid volume name.");
    std::vector<BYTE> packed(mapBytes);if(!Read(1088,packed.data(),packed.size())) return false;
    map_.resize(count);
    for(unsigned i=0;i<count;++i) {
        unsigned offset=i*3/2;
        map_[i]=uint16_t(i%2?((packed[offset]&15)<<8)|packed[offset+1]:(packed[offset]<<4)|(packed[offset+1]>>4));
        // Directory sectors can reside in the allocation area, but must then
        // be marked with the directory-reserved value, never exposed as data.
        uint64_t start=allocationStart_+uint64_t(i)*allocationSize_,end=start+allocationSize_;
        if(start<directoryEnd && end>directoryStart && map_[i]!=0xfff) return Fail("Directory overlaps an unreserved allocation block.");
    }
    std::vector<bool> claimed(count,false);std::set<uint32_t> ids;
    for(unsigned sector=0;sector<directoryBlocks;++sector) {
        BYTE bytes[512];if(!Read(directoryStart+uint64_t(sector)*512,bytes,sizeof(bytes))) return false;
        for(unsigned offset=0;offset<512;) {
            if(bytes[offset]==0) break; // End of this sector, not the entire directory.
            if(512-offset<52) return Fail("Truncated directory entry.");
            BYTE* p=bytes+offset;unsigned n=p[50],size=(51+n+1)&~1;
            if(!n || size>512-offset) return Fail("Directory name crosses a sector boundary.");
            if(!(p[0]&0x80) || (p[0]&0x3e)) return Fail("Invalid directory flags.");
            MfsEntry entry;entry.flags=p[0];entry.version=p[1];entry.id=be32(p+18);
            entry.name.assign(reinterpret_cast<char*>(p+51),n);memcpy(entry.finderInfo,p+2,16);
            entry.created=be32(p+42);entry.modified=be32(p+46);
            if(!entry.id || !ids.insert(entry.id).second || entry.name.find('\0')!=std::string::npos ||
               entry.name.find(':')!=std::string::npos) return Fail("Invalid file ID or name.");
            for(unsigned fork=0;fork<2;++fork) {
                unsigned f=22+10*fork;
                entry.forks[fork].length=be32(p+f+2);entry.forks[fork].physicalLength=be32(p+f+6);
                if(!Chain(entry.forks[fork],uint16_t(be16(p+f)),claimed)) {entries_.clear();return false;}
            }
            entries_.push_back(entry);offset+=size;
        }
    }
    if(entries_.size()!=fileCount) {entries_.clear();return Fail("Directory file count does not match the volume header.");}
    return true;
}
bool MfsImage::ReadFork(size_t entry,unsigned fork,uint64_t offset,void* bytes,size_t requested,size_t& read) {
    read=0;
    if(!source_ || !error_.empty() || entry>=entries_.size() || fork>1) return Fail("Invalid MFS fork selection.");
    const MfsFork& f=entries_[entry].forks[fork];if(offset>=f.length) return true;
    size_t length=static_cast<size_t>((std::min)(uint64_t(requested),uint64_t(f.length)-offset));
    while(read<length) {
        size_t block=static_cast<size_t>(offset/allocationSize_),inside=static_cast<size_t>(offset%allocationSize_);
        size_t chunk=(std::min)(length-read,size_t(allocationSize_)-inside);
        if(block>=f.blocks.size() || !Read(allocationStart_+uint64_t(f.blocks[block]-2)*allocationSize_+inside,static_cast<BYTE*>(bytes)+read,chunk)) return false;
        offset+=chunk;read+=chunk;
    }
    return true;
}
std::wstring MfsDisplayName(const std::string& name) {
    if(name.empty()) return {};
    int size=MultiByteToWideChar(10000,0,name.data(),int(name.size()),NULL,0);
    if(size<=0) return L"(name conversion failed)";
    std::wstring result(size,L'\0');MultiByteToWideChar(10000,0,name.data(),int(name.size()),&result[0],size);return result;
}

bool MfsExportFile(MfsImage& image,size_t index,const wchar_t* path,bool appleSingle,std::string& error) {
    if(index>=image.Entries().size() || !image.Error().empty()) {error="Invalid file selection.";return false;}
    const MfsEntry& entry=image.Entries()[index];
    HANDLE out=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(out==INVALID_HANDLE_VALUE) {error="Could not create the export. Choose a new filename; existing files are not overwritten.";return false;}
    bool ok=true;
    auto write=[&](const void* data,size_t length) {DWORD written;if(length && (!WriteFile(out,data,DWORD(length),&written,NULL)||written!=length)) ok=false;};
    if(appleSingle) {
        // AppleSingle v2: data, resource, original MacRoman name, Finder info,
        // and file dates. The extended Finder information is absent in MFS.
        BYTE header[26+5*12]={};put32(header,0x00051600);put32(header+4,0x00020000);put16(header+24,5);
        uint32_t ids[]={1,2,3,9,8},lengths[]={entry.forks[0].length,entry.forks[1].length,uint32_t(entry.name.size()),32,16};
        uint64_t offset=sizeof(header);
        for(unsigned i=0;i<5;++i) {
            if(offset+lengths[i]>0xffffffffULL) ok=false;
            put32(header+26+12*i,ids[i]);put32(header+30+12*i,uint32_t(offset));put32(header+34+12*i,lengths[i]);offset+=lengths[i];
        }
        if(ok) write(header,sizeof(header));
    }
    std::vector<BYTE> buffer(64*1024);
    for(unsigned fork=0;fork<(appleSingle?2u:1u) && ok;++fork) {
        uint64_t offset=0;
        while(offset<entry.forks[fork].length && ok) {
            size_t read=0;
            if(!image.ReadFork(index,fork,offset,buffer.data(),buffer.size(),read) || !read) {ok=false;break;}
            write(buffer.data(),read);offset+=read;
        }
    }
    if(appleSingle && ok) {
        write(entry.name.data(),entry.name.size());BYTE finder[32]={},dates[16];memcpy(finder,entry.finderInfo,16);write(finder,sizeof(finder));
        // AppleSingle dates use seconds relative to 2000-01-01; MFS uses 1904.
        // Out-of-range/unknown dates use Apple's reserved unknown value.
        auto date=[](uint32_t v)->uint32_t {int64_t seconds=int64_t(v)-3029529600LL;return v && seconds>INT32_MIN && seconds<=INT32_MAX?uint32_t(seconds):0x80000000u;};
        put32(dates,date(entry.created));put32(dates+4,date(entry.modified));put32(dates+8,0x80000000u);put32(dates+12,0x80000000u);write(dates,sizeof(dates));
    }
    if(ok && !FlushFileBuffers(out)) ok=false;
    CloseHandle(out);
    if(!ok) {DeleteFileW(path);error="Export failed; the incomplete output was removed.";return false;}
    return true;
}
