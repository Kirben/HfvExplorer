// SPDX-License-Identifier: GPL-2.0-or-later
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <objbase.h>
#pragma comment(lib,"ole32.lib")
#include "HfsPlusDisk.h"
#include "ImageHandleTransfer.h"
namespace HfsPlus {
#ifdef HFV_TESTING
std::function<void(bool,uint64_t,size_t)> workerIoFault;
#endif
namespace {
class Image final : public NewImage {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    bool writable_ = false;
    bool discard_ = false;
    std::wstring path_;
    std::wstring journalPath_;std::shared_ptr<Source> journalDevice_;std::string journalUuid_;
    void Io(uint64_t offset, void *bytes, size_t n, bool write) {
#ifdef HFV_TESTING
        if(workerIoFault)workerIoFault(write,offset,n);
#endif
        Disk::Check(Disk::Range(offset, n, size_), "Image I/O range overflow");
        LARGE_INTEGER at;
        at.QuadPart = offset;
        if (!SetFilePointerEx(file_, at, nullptr, FILE_BEGIN))
            throw Error("Cannot seek HFS+ image");
        auto p = static_cast<uint8_t *>(bytes);
        while (n) {
            DWORD requested = DWORD((std::min)(n, size_t(1024 * 1024))), done = 0;
            BOOL ok = write ? WriteFile(file_, p, requested, &done, nullptr)
                            : ReadFile(file_, p, requested, &done, nullptr);
            if (!ok || done != requested)
                throw Error(write ? "Cannot write HFS+ image" : "Cannot read HFS+ image");
            p += done;
            n -= done;
        }
    }

  public:
    Image(const wchar_t *path, bool write, uint64_t createSize,const wchar_t* journalPath=nullptr,bool discard=false)
        :discard_(discard),path_(path?path:L""),journalPath_(journalPath?journalPath:L"") {
        Disk::Check(path && *path && wcsncmp(path, L"\\\\.\\", 4) != 0 &&
                        wcsncmp(path, L"\\\\?\\GLOBALROOT", 14) != 0,
                    "Physical device paths are not HFS+ images");
        if(!createSize)file_=ImageHandleTransfer::Open(path,write);
        if(file_==INVALID_HANDLE_VALUE)file_ = CreateFileW(path, GENERIC_READ | (write ? GENERIC_WRITE : 0) | (discard ? DELETE : 0), FILE_SHARE_READ, nullptr,
                            createSize ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Disk::Check(file_ != INVALID_HANDLE_VALUE, "Cannot open HFS+ image (it may be in use)");
        try {
            Disk::Check(GetFileType(file_) == FILE_TYPE_DISK, "HFS+ requires a disk image file");
            if (createSize) {
                DWORD returned;
                DeviceIoControl(file_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
                LARGE_INTEGER at;
                at.QuadPart = createSize;
                Disk::Check(SetFilePointerEx(file_, at, nullptr, FILE_BEGIN) && SetEndOfFile(file_),
                            "Cannot size HFS+ image");
            }
            BY_HANDLE_FILE_INFORMATION info = {};
            Disk::Check(GetFileInformationByHandle(file_, &info) && info.nNumberOfLinks &&
                            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                        "HFS+ requires an ordinary image file");
            LARGE_INTEGER length;
            Disk::Check(GetFileSizeEx(file_, &length) && length.QuadPart >= 0, "Invalid HFS+ image length");
            size_ = length.QuadPart;
            writable_ = write;
        } catch (...) {
            Discard();
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            throw;
        }
    }
    ~Image() {
        if (file_ != INVALID_HANDLE_VALUE) {
            Discard();
            CloseHandle(file_);
        }
    }
    void Keep() noexcept override {discard_=false;}
    bool Discard() noexcept override {
        if(!discard_)return true;
        FILE_DISPOSITION_INFO remove{TRUE};
        if(!SetFileInformationByHandle(file_,FileDispositionInfo,&remove,sizeof(remove)))return false;
        discard_=false;
        return true;
    }
    std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{std::vector<std::pair<std::wstring,uintptr_t>> out{{path_,uintptr_t(file_)}};if(journalDevice_){auto extra=journalDevice_->WorkerFiles();out.insert(out.end(),extra.begin(),extra.end());}return out;}
    std::shared_ptr<Source> OpenJournal(const Bytes&)override;
    uint64_t Size() const override { return size_; }
    bool Writable() const override { return writable_; }
    void Read(uint64_t o, void *p, size_t n) override { Io(o, p, n, false); }
    void Write(uint64_t o, const void *p, size_t n) override {
        Disk::Check(writable_, "HFS+ image is read-only");
        Io(o, const_cast<void *>(p), n, true);
    }
    void Flush() override {
        if (writable_ && !FlushFileBuffers(file_))
            throw Error("Cannot flush HFS+ image");
    }
};

// External journal GPT layout follows UEFI chapter 5; both table copies and
// CRCs are checked before exposing the UUID-matched partition to the journal.
uint32_t L32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint64_t L64(const uint8_t* p){return L32(p)|uint64_t(L32(p+4))<<32;}
uint32_t Crc(const Bytes& bytes){uint32_t crc=~0u;for(auto b:bytes){crc^=b;for(unsigned n=0;n<8;++n)crc=(crc>>1)^(0xedb88320u&uint32_t(-int(crc&1)));}return ~crc;}
std::array<uint8_t,16> Uuid(const std::string& text){Disk::Check(text.size()==36,"External journal UUID is missing or invalid");std::array<uint8_t,16> bytes{};unsigned at=0;int half=-1;for(unsigned i=0;i<text.size();++i){if(i==8||i==13||i==18||i==23){Disk::Check(text[i]=='-',"Invalid journal UUID");continue;}char c=text[i];int n=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;Disk::Check(n>=0,"Invalid journal UUID");if(half<0)half=n;else{bytes[at++]=uint8_t(half*16+n);half=-1;}}Disk::Check(std::any_of(bytes.begin(),bytes.end(),[](uint8_t n){return n!=0;}),"External journal UUID cannot be zero");return bytes;}
std::array<uint8_t,16> GptUuid(const uint8_t* p){std::array<uint8_t,16> out{};for(unsigned i=0;i<16;++i)out[i]=p[i];std::reverse(out.begin(),out.begin()+4);std::reverse(out.begin()+4,out.begin()+6);std::reverse(out.begin()+6,out.begin()+8);return out;}
class JournalPartition final:public Source{
 std::unique_ptr<Source> file_;uint64_t base_,length_;
public:JournalPartition(std::unique_ptr<Source> file,uint64_t base,uint64_t length):file_(std::move(file)),base_(base),length_(length){}
 std::vector<std::pair<std::wstring,uintptr_t>> WorkerFiles()const override{return file_->WorkerFiles();}
 uint64_t Size()const override{return length_;}bool Writable()const override{return file_->Writable();}
 void Read(uint64_t at,void* p,size_t n)override{Disk::Check(Disk::Range(at,n,length_),"External journal partition read range");file_->Read(base_+at,p,n);}
 void Write(uint64_t at,const void* p,size_t n)override{Disk::Check(Disk::Range(at,n,length_),"External journal partition write range");file_->Write(base_+at,p,n);}void Flush()override{file_->Flush();}
};
std::shared_ptr<Source> BindJournal(std::unique_ptr<Source> file,const std::string& wanted){
 auto uuid=Uuid(wanted),type=Uuid("4A6F7572-6E61-11AA-AA11-00306543ECAC");
 for(uint64_t sector:{512ULL,4096ULL}){
  if(file->Size()<sector*6||file->Size()%sector)continue;Bytes probe(8);file->Read(sector,probe.data(),8);if(memcmp(probe.data(),"EFI PART",8))continue;
  uint64_t sectors=file->Size()/sector;
  auto header=[&](uint64_t lba){Bytes h(size_t(sector),0);file->Read(lba*sector,h.data(),h.size());Disk::Check(!memcmp(h.data(),"EFI PART",8)&&L32(h.data()+8)==0x10000&&L32(h.data()+12)>=92&&L32(h.data()+12)<=sector&&L32(h.data()+20)==0&&L64(h.data()+24)==lba,"Invalid external-journal GPT header");h.resize(L32(h.data()+12));auto crc=L32(h.data()+16);memset(h.data()+16,0,4);Disk::Check(Crc(h)==crc,"External-journal GPT header checksum mismatch");return h;};
  auto primary=header(1),backup=header(sectors-1);Disk::Check(L64(primary.data()+32)==sectors-1&&L64(backup.data()+32)==1&&!memcmp(primary.data()+40,backup.data()+40,32)&&!memcmp(primary.data()+80,backup.data()+80,12),"External-journal GPT copies disagree");
  uint64_t first=L64(primary.data()+40),last=L64(primary.data()+48);auto count=L32(primary.data()+80),entrySize=L32(primary.data()+84);Disk::Check(first>1&&last>=first&&last<sectors-1&&count&&count<=4096&&entrySize>=128&&entrySize<=1024&&(entrySize&(entrySize-1))==0,"Invalid external-journal GPT geometry");
  auto table=[&](const Bytes& h){auto at=L64(h.data()+72),bytes=uint64_t(count)*entrySize;Disk::Check(at>1&&at<sectors-1&&Disk::Range(at*sector,bytes,file->Size())&&(L64(h.data()+24)==1?(at+(bytes+sector-1)/sector<=first):(at>last&&at+(bytes+sector-1)/sector<=sectors-1)),"External-journal GPT table overlaps usable data");Bytes out(size_t(bytes),0);file->Read(at*sector,out.data(),out.size());Disk::Check(Crc(out)==L32(h.data()+88),"External-journal GPT table checksum mismatch");return out;};
  auto entries=table(primary);Disk::Check(entries==table(backup),"External-journal GPT partition tables disagree");std::vector<std::pair<uint64_t,uint64_t>> ranges;uint64_t base=0,length=0;unsigned matches=0;
  for(unsigned i=0;i<count;++i){auto p=entries.data()+size_t(i)*entrySize;if(std::all_of(p,p+16,[](uint8_t n){return !n;}))continue;auto begin=L64(p+32),end=L64(p+40);Disk::Check(begin>=first&&end>=begin&&end<=last,"GPT partition lies outside usable area");ranges.emplace_back(begin,end);if(GptUuid(p+16)==uuid){Disk::Check(GptUuid(p)==type,"Matching UUID is not an external-journal partition");Disk::Check(!(L64(p+48)&(1ULL<<60))||!file->Writable(),"External-journal GPT partition is read-only");base=begin*sector;length=(end-begin+1)*sector;++matches;}}
  std::sort(ranges.begin(),ranges.end());for(size_t i=1;i<ranges.size();++i)Disk::Check(ranges[i-1].second<ranges[i].first,"Overlapping external-journal GPT partitions");Disk::Check(matches==1,"External journal UUID does not uniquely match this disk image");return std::make_shared<JournalPartition>(std::move(file),base,length);
 }
 throw Error("Select a GPT disk image containing the matching external-journal partition");
}
std::shared_ptr<Source> Image::OpenJournal(const Bytes& info){
 Disk::Check(info.size()>=89&&info[88]==0,"Missing external journal UUID");std::string uuid(reinterpret_cast<const char*>(info.data()+52),36);(void)Uuid(uuid);
 if(journalDevice_){Disk::Check(Uuid(uuid)==Uuid(journalUuid_),"External journal identity changed during recovery");return journalDevice_;}
 if(journalPath_.empty())throw ExternalJournalRequired("Select external journal disk image for UUID "+uuid);
 journalDevice_=BindJournal(HfsPlus::OpenImage(journalPath_.c_str(),writable_),uuid);journalUuid_=uuid;return journalDevice_;
}
} // namespace
std::unique_ptr<Source> OpenImage(const wchar_t *path, bool write,const wchar_t* externalJournal) {
    return std::unique_ptr<Source>(new Image(path, write, 0,externalJournal));
}
std::unique_ptr<Source> CreateImage(const wchar_t *path, uint64_t size,const wchar_t* externalJournal) {
    Disk::Check(size >= 8 * 1024 * 1024 && size % 4096 == 0 && size <= INT64_MAX,
                "HFS+ image size must be at least 8 MiB and a multiple of 4096");
    return std::unique_ptr<Source>(new Image(path, true, size,externalJournal));
}

std::unique_ptr<NewImage> CreateFormatOutput(const wchar_t* path,uint64_t size,const wchar_t* journal)
{
 Disk::Check(size&&size<=INT64_MAX,"Invalid format output length");
 return std::make_unique<Image>(path,true,size,journal,true);
}

std::string InitializeExternalJournalImage(Source& file)
{
 const auto size=file.Size();
 Disk::Check(size>=8*1024*1024&&size%4096==0,"External journal disk image must be at least 8 MiB and 4096-byte aligned");
 GUID partition{},disk{};Disk::Check(SUCCEEDED(CoCreateGuid(&partition))&&SUCCEEDED(CoCreateGuid(&disk)),"Cannot generate journal identity");
 char text[37];sprintf_s(text,"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",partition.Data1,partition.Data2,partition.Data3,partition.Data4[0],partition.Data4[1],partition.Data4[2],partition.Data4[3],partition.Data4[4],partition.Data4[5],partition.Data4[6],partition.Data4[7]);
 auto little=[](uint8_t* p,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i)p[i]=uint8_t(n>>(8*i));};uint64_t sectors=size/512;Bytes table(128*128,0);auto type=Uuid("4A6F7572-6E61-11AA-AA11-00306543ECAC");std::reverse(type.begin(),type.begin()+4);std::reverse(type.begin()+4,type.begin()+6);std::reverse(type.begin()+6,type.begin()+8);memcpy(table.data(),type.data(),16);memcpy(table.data()+16,&partition,16);little(table.data()+32,2048,8);little(table.data()+40,sectors-34,8);const char* label="HFVExplorer external journal";for(unsigned i=0;label[i];++i)table[56+i*2]=uint8_t(label[i]);
 Bytes mbr(512,0);mbr[448]=2;mbr[450]=0xee;mbr[451]=mbr[452]=mbr[453]=0xff;little(mbr.data()+454,1,4);little(mbr.data()+458,(std::min)(sectors-1,uint64_t(UINT32_MAX)),4);mbr[510]=0x55;mbr[511]=0xaa;file.Write(0,mbr.data(),mbr.size());file.Write(1024,table.data(),table.size());file.Write((sectors-33)*512,table.data(),table.size());
 for(bool backup:{false,true}){Bytes h(512,0);memcpy(h.data(),"EFI PART",8);little(h.data()+8,0x10000,4);little(h.data()+12,92,4);little(h.data()+24,backup?sectors-1:1,8);little(h.data()+32,backup?1:sectors-1,8);little(h.data()+40,34,8);little(h.data()+48,sectors-34,8);memcpy(h.data()+56,&disk,16);little(h.data()+72,backup?sectors-33:2,8);little(h.data()+80,128,4);little(h.data()+84,128,4);little(h.data()+88,Crc(table),4);Bytes check(h.begin(),h.begin()+92);little(h.data()+16,Crc(check),4);file.Write((backup?sectors-1:1)*512,h.data(),h.size());}
 file.Flush();return text;
}
std::string CreateExternalJournalImage(const wchar_t* path,uint64_t size)
{
 Disk::Check(size>=8*1024*1024&&size%4096==0,"External journal disk image must be at least 8 MiB and 4096-byte aligned");
 auto output=CreateFormatOutput(path,size);
 auto uuid=InitializeExternalJournalImage(*output);
 output->Keep();
 return uuid;
}
} // namespace HfsPlus
