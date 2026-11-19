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

std::optional<BrowserMountInventory::Address> BrowserMountInventory::FromTreeBinding(int type,int index)
{
 if(index<0)return {};
 switch(type){
 case LIST_FAT:return Address{Kind::WindowsDrive,index};
 case LIST_HFS:return Address{Kind::ClassicVolume,index};
 case LIST_MAC:return Address{Kind::MountedVolume,index};
 default:return {};
 }
}

std::optional<BrowserMountInventory::Entry> BrowserMountInventory::Lookup(Address address) const
{
 if(address.index<0)return {};
 Entry entry{address};
 const auto index=address.index;
 switch(address.kind){
 case Kind::WindowsDrive:{
  if(index>=windowsCount)return {};
  const auto& volume=windows[index];
  entry.root=volume.m_rootitem;
  entry.locator=VolumeCoord::Wide(volume.m_volume_name.GetString());
  entry.drive=toupper(static_cast<unsigned char>(volume.m_letter))-'A';
  entry.generation=windowsSerial_[index];
  break;
 }
 case Kind::ClassicVolume:{
  if(index>=classicCount)return {};
  const auto* volume=classic.Find(index);
  if(!volume||volume->m_free||volume->m_file_name.IsEmpty())return {};
  const auto* file=volume->m_file_name.GetString();
  entry.root=volume->m_rootitem;
  entry.locator=VolumeCoord::Wide(file);
  if(volume->m_is_floppy||volume->m_is_cd)entry.drive=toupper(static_cast<unsigned char>(file[0]))-'A';
  if(volume->m_mediaGenerationKnown)entry.generation=volume->m_mediaGeneration;
  break;
 }
 case Kind::MountedVolume:{
  if(size_t(index)>=mounted.size())return {};
  const auto& volume=mounted[index];
  if(!volume.volume)return {};
  entry.root=volume.root;
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
 return current&&current->root==captured.root&&
     !_wcsicmp(current->locator.c_str(),captured.locator.c_str())&&
     current->drive==captured.drive&&current->mediaEpoch==captured.mediaEpoch&&
     current->generation==captured.generation&&
     !current->volume.owner_before(captured.volume)&&!captured.volume.owner_before(current->volume);
}

void BrowserMountInventory::Observe(Address address,const std::string& resource,std::optional<DWORD> windowsSerial)
{
 if(address.kind==Kind::WindowsDrive&&address.index>=0&&address.index<windowsCount)
  windowsSerial_[address.index]=windowsSerial;
 auto entry=Lookup(address);
 if(!entry||!entry->root||resource.empty())return;
 auto& known=observed_[resource];
 auto previous=std::find_if(known.begin(),known.end(),[&](const Entry& value){return value.address==address;});
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
}
