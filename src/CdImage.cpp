// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "CdImage.h"
#include "ReadOnlyCd.h"
#include "CdImageToc.h"
#include <vector>
namespace {
class NativeCdImageSource : public CdImageSource {
 ReadOnlyCd cd_;DWORD error_=0,identity_=0;bool ready_=false;unsigned long long size_=0;
public:
 bool Open(int drive,std::string& reason) override {
  ready_=false;size_=0;
  if(!cd_.OpenDrive(drive)){error_=cd_.Error();reason="Could not open the data CD";return false;}
  DWORD before=0,after=0;
  if(!Verify(before)){reason="Could not establish CD media identity";return false;}
  CDROM_READ_TOC_EX query={};query.Format=CDROM_READ_TOC_EX_FORMAT_TOC;query.Msf=0;
  CDROM_TOC toc={};DWORD returned=0;
  if(!DeviceIoControl(cd_.Handle(),IOCTL_CDROM_READ_TOC_EX,&query,sizeof(query),&toc,sizeof(toc),&returned,nullptr)){
   error_=GetLastError();reason="Could not read the CD table of contents";return false;
  }
  if(!CdImageSupportedToc(toc,returned)){
   error_=ERROR_NOT_SUPPORTED;reason="Only single-track data CDs starting at sector zero are supported. Audio, mixed-mode and multisession CDs need a different image format.";return false;
  }
  if(!Verify(after)){reason="CD media check failed while reading the table of contents";return false;}
  if(before!=after){error_=ERROR_MEDIA_CHANGED;reason="CD media changed while opening";return false;}
  size_=CdImageTocBytes(toc);identity_=before;ready_=true;
  return true;
 }
 unsigned long long Size() const override{return size_;}
 bool Verify(DWORD& generation) override {
  DWORD returned=0;generation=0;
  if(!DeviceIoControl(cd_.Handle(),IOCTL_STORAGE_CHECK_VERIFY,nullptr,0,&generation,sizeof(generation),&returned,nullptr)){error_=GetLastError();return false;}
  if(returned<sizeof(generation)){error_=ERROR_NOT_SUPPORTED;return false;}
  if(ready_&&generation!=identity_){error_=ERROR_MEDIA_CHANGED;return false;}return true;
 }
 bool Read(unsigned long long offset,DWORD count,void* buffer,DWORD& got) override {
  got=0;if(!cd_.ReadBytes(offset,count,buffer)){error_=cd_.Error();return false;}got=count;return true;
 }
 DWORD Error() const override{return error_;}
};
struct Output {
 char temporary[MAX_PATH]={};HANDLE handle=INVALID_HANDLE_VALUE;
 ~Output(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);if(*temporary)DeleteFileA(temporary);}
};
Storage::Result failure(DWORD error,const std::string& reason,unsigned long long offset=0) {
 if(!error)error=ERROR_IO_DEVICE;
 return {1,error,reason+" (Windows error "+std::to_string(error)+", byte offset "+std::to_string(offset)+")."};
}
}
std::function<Storage::Result(Storage::Job&)> MakeCdImageJob(int drive,std::string output,std::shared_ptr<CdImageSource> injected) {
 return [=](Storage::Job& job)->Storage::Result {
  if(drive<0||drive>25)return failure(ERROR_INVALID_PARAMETER,"Invalid source drive");
  char destination[MAX_PATH];DWORD length=GetFullPathNameA(output.c_str(),MAX_PATH,destination,nullptr);
  if(!length||length>=MAX_PATH)return failure(ERROR_FILENAME_EXCED_RANGE,"The output path is too long or invalid");
  DWORD attributes=GetFileAttributesA(destination);bool replace=attributes!=INVALID_FILE_ATTRIBUTES;
  if(replace){
   if(attributes&FILE_ATTRIBUTE_DIRECTORY)return failure(ERROR_ACCESS_DENIED,"The output path is a folder");
   auto choice=job.replace(destination);
   if(choice==Storage::ConflictChoice::Skip)return {};
   if(choice==Storage::ConflictChoice::Cancel)throw Storage::Cancelled();
  }
  if(job.cancelled())throw Storage::Cancelled();
  auto source=injected?injected:std::make_shared<NativeCdImageSource>();
  Storage::Progress p;p.path=destination;p.phase="Opening data CD";p.byteTransfer=true;job.progress(p);
  std::string reason;
  if(!source->Open(drive,reason))return failure(source->Error(),reason);
  auto size=source->Size();
  if(!size||size%2048||size>0x7fffffffffffffffULL)return failure(ERROR_INVALID_DATA,"The drive reported an invalid data-image length");
  DWORD generation=0,current=0;
  if(!source->Verify(generation))return failure(source->Error(),"Could not establish CD media identity");
  char directory[MAX_PATH];strcpy_s(directory,destination);char* slash=strrchr(directory,'\\');
  if(!slash)return failure(ERROR_INVALID_NAME,"The output path has no folder");slash[1]=0;
  Output file;
  if(!GetTempFileNameA(directory,"hcd",0,file.temporary))return failure(GetLastError(),"Could not create the temporary image");
  file.handle=CreateFileA(file.temporary,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(file.handle==INVALID_HANDLE_VALUE)return failure(GetLastError(),"Could not open the temporary image");
  std::vector<BYTE> buffer(64*1024);p.scanning=false;p.totalBytes=size;p.phase="Reading CD image";job.progress(p);
  for(unsigned long long offset=0;offset<size;){
   if(job.cancelled())throw Storage::Cancelled();
   if(!source->Verify(current))return failure(source->Error(),"CD media check failed",offset);
   if(current!=generation)return failure(ERROR_MEDIA_CHANGED,"CD media changed; the image was discarded",offset);
   DWORD count=DWORD(min(size-offset,(unsigned long long)buffer.size())),got=0,wrote=0;
   if(!source->Read(offset,count,buffer.data(),got))return failure(source->Error(),"CD sector read failed; missing sectors were not synthesized",offset);
   if(got!=count)return failure(ERROR_READ_FAULT,"Short CD sector read; the image was discarded",offset);
   if(!source->Verify(current))return failure(source->Error(),"CD media check failed after reading",offset);
   if(current!=generation)return failure(ERROR_MEDIA_CHANGED,"CD media changed while reading; the image was discarded",offset);
   if(!WriteFile(file.handle,buffer.data(),count,&wrote,nullptr))return failure(GetLastError(),"Writing the image file failed",offset);
   if(wrote!=count)return failure(ERROR_WRITE_FAULT,"Short write to the image file",offset);
   offset+=count;p.bytes=p.processed=offset;job.progress(p);
  }
  if(job.cancelled())throw Storage::Cancelled();
  if(!source->Verify(current))return failure(source->Error(),"Final CD media check failed",size);
  if(current!=generation)return failure(ERROR_MEDIA_CHANGED,"CD media changed at completion; the image was discarded",size);
  p.phase="Saving CD image";job.progress(p);
  if(!FlushFileBuffers(file.handle))return failure(GetLastError(),"Flushing the image file failed",size);
  HANDLE handle=file.handle;file.handle=INVALID_HANDLE_VALUE;
  if(!CloseHandle(handle))return failure(GetLastError(),"Closing the image file failed",size);
  if(job.cancelled())throw Storage::Cancelled();
  if(!MoveFileExA(file.temporary,destination,MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0)))return failure(GetLastError(),"Could not publish the completed image",size);
  file.temporary[0]=0;p.phase="CD image saved";job.progress(p);return {};
 };
}
