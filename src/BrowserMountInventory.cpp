// SPDX-License-Identifier: GPL-2.0-or-later
#include "stdafx.h"
#include "BrowserMountInventory.h"
#include "VolumeCoordinator.h"
#include "ftypes.h"
#include <algorithm>

void BrowserMountInventory::ClassicMount::ReadMount(const MacBrowser::ClassicMountInfo& info)
{
 m_volume_name=info.label.c_str();
 freeBytes=info.freeBytes;
 m_mediaGenerationKnown=info.generation.has_value();
 m_mediaGeneration=info.generation.value_or(0);
 update_time_stamp();
 m_opened=true;
}

int BrowserMountInventory::ClassicMount::open()
{
 m_opened=false;
 if(m_file_name.IsEmpty())return false;
 try{
  auto volume=MacBrowser::OpenClassic(m_file_name.GetString(),":",false);
  ReadMount(MacBrowser::ReadClassicMountInfo(*volume));
 }catch(const HfsPlus::Error&){return false;}
 return true;
}

void BrowserMountInventory::ClassicMount::update_time_stamp()
{
 WIN32_FILE_ATTRIBUTE_DATA status{};
 if(!m_is_floppy&&!m_is_cd&&GetFileAttributesExA(m_file_name,GetFileExInfoStandard,&status))
  modified_=status.ftLastWriteTime;
}

bool BrowserMountInventory::ClassicMount::update_if_needed(int ask)
{
 if(m_is_cd)return false;
 if(m_is_floppy){
  if(!m_floppy_update_needed)return false;
  m_floppy_update_needed=false;
  return open()!=0;
 }
 WIN32_FILE_ATTRIBUTE_DATA status{};
 if(!GetFileAttributesExA(m_file_name,GetFileExInfoStandard,&status)||
    CompareFileTime(&status.ftLastWriteTime,&modified_)==0)return false;
 if(ask&&AfxMessageBox("Contents of volume \""+m_volume_name+"\" have changed. Reload now?",MB_YESNO)==IDNO&&
    AfxMessageBox("Not reloading will cause data integrity problems. Reload?",MB_YESNO)==IDNO)return false;
 return open()!=0;
}

int BrowserMountInventory::AddClassic(ClassicMount volume)
{
 int index=0;
 for(;index<classicCount;++index){
  if(!classic[index]||classic[index]->m_free)break;
 }
 if(index==MAX_HFS_VOLUMES)return -1;
 volume.m_free=false;
 classic[index]=std::make_unique<ClassicMount>(std::move(volume));
 classicIncarnation_[index]=++nextIncarnation_;
 if(index==classicCount)++classicCount;
 return index;
}

void BrowserMountInventory::RemoveClassic(int index)
{
 *classic.at(index)=ClassicMount{};
 classicIncarnation_[index]=++nextIncarnation_;
}

void BrowserMountInventory::UpdateClassic(int index,const MacBrowser::ClassicMountInfo& info)
{
 auto& slot=*classic.at(index);
 const auto previous=slot.m_mediaGenerationKnown?std::optional<DWORD>(slot.m_mediaGeneration):std::nullopt;
 if(previous!=info.generation)classicIncarnation_[index]=++nextIncarnation_;
 slot.ReadMount(info);
}

bool BrowserMountInventory::PublishClassic(const Entry& expected,ClassicMount volume)
{
 if(expected.address.kind!=Kind::ClassicVolume||!IsCurrent(expected))return false;
 const auto index=expected.address.index;
 auto& old=*classic.at(index);
 if(old.m_file_name.CompareNoCase(volume.m_file_name)!=0)return false;
 if(old.m_mediaGenerationKnown!=volume.m_mediaGenerationKnown||
    (volume.m_mediaGenerationKnown&&old.m_mediaGeneration!=volume.m_mediaGeneration))
    classicIncarnation_[index]=++nextIncarnation_;
 old=std::move(volume);
 return true;
}

int BrowserMountInventory::AddWindows(char letter,UINT type,CString path)
{
 if(windowsCount>=MAX_FAT_VOLUMES)return -1;
 int index=windowsCount++;
 auto& drive=windows[index];
 drive.m_letter=letter;
 if(path.IsEmpty())path.Format("%c:\\\\",letter);
 drive.m_volume_name=std::move(path);
 SetWindowsType(index,type);
 return index;
}

int BrowserMountInventory::FindWindowsSlot(char letter) const
{
 for(int index=0;index<windowsCount;++index)
  if(toupper(static_cast<unsigned char>(windows[index].m_letter))==toupper(static_cast<unsigned char>(letter)))
   return index;
 return -1;
}

int BrowserMountInventory::FindClassicSlot(const CString& path) const
{
 for(int index=0;index<classicCount;++index)
  if(const auto& slot=ClassicAt(index);!slot.m_free&&slot.m_file_name.CompareNoCase(path)==0)return index;
 return -1;
}

void BrowserMountInventory::CloseClassic(int index){classic.at(index)->close();}
void BrowserMountInventory::CloseClassicVolumes()
{
 for(int index=0;index<classicCount;++index)CloseClassic(index);
}
void BrowserMountInventory::MarkClassicDirty(int index){classic.at(index)->m_floppy_update_needed=true;}
void BrowserMountInventory::MarkClassicClean(int index){classic.at(index)->update_time_stamp();}
bool BrowserMountInventory::RefreshClassic(int index,int ask)
{
 auto& slot=*classic.at(index);
 const auto before=slot.m_mediaGenerationKnown?std::optional<DWORD>(slot.m_mediaGeneration):std::nullopt;
 if(!slot.update_if_needed(ask))return false;
 const auto after=slot.m_mediaGenerationKnown?std::optional<DWORD>(slot.m_mediaGeneration):std::nullopt;
 if(before!=after)classicIncarnation_[index]=++nextIncarnation_;
 return true;
}

#ifdef HFV_TESTING
bool BrowserMountInventory::OpenClassicForTest(int index){return classic.at(index)->open()!=0;}
void BrowserMountInventory::SetClassicSourceForTest(int index,const CString& path,bool cd)
{
 auto& slot=*classic.at(index);
 slot.close();
 slot.m_file_name=path;
 slot.m_is_cd=cd;
 classicIncarnation_[index]=++nextIncarnation_;
}
#endif

void BrowserMountInventory::SetWindowsType(int index,UINT type)
{
 auto& drive=windows[index];
 if(drive.m_type!=type||!windowsIncarnation_[index]){
  drive.m_type=type;
  windowsSerial_[index].reset();
  windowsIncarnation_[index]=++nextIncarnation_;
 }
}

int BrowserMountInventory::AddMounted(Mount mount)
{
 mount.incarnation=++nextIncarnation_;
 mounted.push_back(std::move(mount));
 return int(mounted.size()-1);
}

void BrowserMountInventory::ReplaceMounted(int index,Mount mount)
{
 mount.incarnation=++nextIncarnation_;
 mounted.at(index)=std::move(mount);
}

void BrowserMountInventory::SetMountedSourceDrive(int index,int drive)
{
 mounted.at(index).sourceDrive=drive;
}

std::shared_ptr<MacBrowser::Volume> BrowserMountInventory::RemoveMounted(int index)
{
 auto& mount=mounted.at(index);
 mount.incarnation=++nextIncarnation_;
 return std::move(mount.volume);
}

BrowserMountInventory::Address BrowserMountInventory::Bind(Address address) const
{
 const auto index=address.index;
 address.incarnation=0;
 if(index<0)return address;
 switch(address.kind){
 case Kind::ClassicVolume:
  if(index<MAX_HFS_VOLUMES)address.incarnation=classicIncarnation_[index];
  break;
 case Kind::WindowsDrive:
  if(index<MAX_FAT_VOLUMES)address.incarnation=windowsIncarnation_[index];
  break;
 case Kind::MountedVolume:
  if(size_t(index)<mounted.size())address.incarnation=mounted[index].incarnation;
  return address;
 }
 return address;
}

bool BrowserMountInventory::IsBound(Address address) const
{
 const auto bound=Bind(address);
 return bound.incarnation&&(!address.incarnation||address==bound);
}

std::optional<BrowserMountInventory::Entry> BrowserMountInventory::Lookup(Address address) const
{
 if(!IsBound(address))return {};
 Entry entry{Bind(address)};
 const auto index=address.index;
 switch(address.kind){
 case Kind::WindowsDrive:{
  if(index>=windowsCount)return {};
  const auto& volume=windows[index];
  if(volume.m_type==DRIVE_NO_ROOT_DIR)return {};
  entry.locator=VolumeCoord::Wide(volume.m_volume_name.GetString());
  entry.drive=toupper(static_cast<unsigned char>(volume.m_letter))-'A';
  entry.generation=windowsSerial_[index];
  break;
 }
 case Kind::ClassicVolume:{
  if(index>=classicCount)return {};
  const auto* volume=classic[index].get();
  if(!volume||volume->m_free||volume->m_file_name.IsEmpty())return {};
  const auto* file=volume->m_file_name.GetString();
  entry.locator=VolumeCoord::Wide(file);
  if(volume->m_is_floppy||volume->m_is_cd)entry.drive=toupper(static_cast<unsigned char>(file[0]))-'A';
  if(volume->m_mediaGenerationKnown)entry.generation=volume->m_mediaGeneration;
  break;
 }
 case Kind::MountedVolume:{
  if(size_t(index)>=mounted.size())return {};
  const auto& volume=mounted[index];
  if(!volume.volume)return {};
  entry.locator=volume.key;
  entry.drive=volume.drive;
  entry.volume=volume.volume;
  break;
 }
 }
 if(entry.drive>=0&&entry.drive<26)entry.mediaEpoch=mediaEpoch_[entry.drive];
 return entry;
}

std::optional<BrowserMountInventory::Entry> BrowserMountInventory::FindWindowsDrive(int drive) const
{
 for(int index=0;index<windowsCount;++index)
  if(auto entry=Lookup({Kind::WindowsDrive,index});entry&&entry->drive==drive)return entry;
 return {};
}

std::optional<BrowserMountInventory::Entry> BrowserMountInventory::FindClassicVolume(const std::wstring& locator) const
{
 for(int index=0;index<classicCount;++index)
  if(auto entry=Lookup({Kind::ClassicVolume,index});entry&&!_wcsicmp(entry->locator.c_str(),locator.c_str()))return entry;
 return {};
}

bool BrowserMountInventory::IsCurrent(const Entry& captured) const
{
 const auto current=Lookup(captured.address);
 return current&&
     !_wcsicmp(current->locator.c_str(),captured.locator.c_str())&&
     current->drive==captured.drive&&current->mediaEpoch==captured.mediaEpoch&&
     current->generation==captured.generation&&
     !current->volume.owner_before(captured.volume)&&!captured.volume.owner_before(current->volume);
}

void BrowserMountInventory::Observe(Address address,const std::string& resource,std::optional<DWORD> windowsSerial)
{
 if(!IsBound(address))return;
 if(address.kind==Kind::WindowsDrive&&address.index>=0&&address.index<windowsCount)
  windowsSerial_[address.index]=windowsSerial;
 auto entry=Lookup(address);
 if(!entry||resource.empty())return;
 auto& known=observed_[resource];
 auto previous=std::find_if(known.begin(),known.end(),[&](const Entry& value){return value.address.SameSlot(address);});
 if(previous==known.end())known.push_back(std::move(*entry));
 else *previous=std::move(*entry);
}

const std::vector<BrowserMountInventory::Entry>& BrowserMountInventory::Observed(const std::string& resource) const
{
 static const std::vector<Entry> empty;
 const auto found=observed_.find(resource);
 return found==observed_.end()?empty:found->second;
}

void BrowserMountInventory::MediaChanged(DWORD driveMask)
{
 for(int drive=0;drive<26;++drive)
  if(driveMask&(DWORD(1)<<drive))++mediaEpoch_[drive];
 for(int index=0;index<windowsCount;++index){
  const auto drive=toupper(static_cast<unsigned char>(windows[index].m_letter))-'A';
  if(drive>=0&&drive<26&&(driveMask&(DWORD(1)<<drive)))windowsSerial_[index].reset();
 }
}
