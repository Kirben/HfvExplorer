// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserMountInventory.h"
#include "Mactypes.h"
#include "BrowserDirectory.h"
#include "itemtype.h"
#include "bndl.h"
#include <map>

namespace MacBrowser { class Volume; }
class BrowserFolderReader {
public:
 struct Options {
  bool showWindowsHidden=false,showMacHidden=false;
  int bitsPerPixel=1,windowsIcons=0,windowsIconTimeout=0;
 };

 explicit BrowserFolderReader(Options options);
 ~BrowserFolderReader();
 BrowserFolderReader(const BrowserFolderReader&)=delete;
 BrowserFolderReader& operator=(const BrowserFolderReader&)=delete;

 BrowserMountInventory::ClassicMount& ClassicState(int index){return slots_[index];}
 void SetOptions(Options options){options_=options;}
 BrowserDirectory::Snapshot read_classic_browser_directory(int,long,CString,bool,long,Storage::Job* job=nullptr,MacBrowser::Volume* mounted=nullptr);
 BrowserDirectory::Snapshot read_windows_browser_directory(const std::wstring&,bool,Storage::Job&,std::optional<DWORD> expectedSerial={});
	void delete_cache();
	int exists_icon_cache_file();
	int delete_cache_OK();
	void add_to_icon_cache( HICON hicon );
	void destroy_icon_cache();
	void init_mac_icons();
	void load_custom_icons(
		int volinx,
		CatDataRec *pCDR,
		HICON *phicon16,
		HICON *phicon32
	);
	void load_custom_icons(
		CFile *fp,
		unsigned long g_offset,
		HICON *phicon16,
		HICON *phicon32
	);

	int walk_resource_fork( CFile *fp, LongInt type_to_check, Integer id_to_check );
	int walk_double_resource_fork( CFile *fp, LongInt type_to_check, Integer id_to_check );
	HANDLE mac_load_any_resource(
		int volinx,
		CatDataRec *pCDR,
		LongInt type_to_check,
		int nocheck,
		Integer id_to_check );
	HANDLE mac_load_any_resource2(
		CFile *fp,
		unsigned long g_offset,
		LongInt type_to_check,
		int nocheck,
		Integer id_to_check );
	HICON mac_load_icon( int volinx, CatDataRec *pCDR, int small );
// bundle stuff here
	unsigned long hash_func (OSType type, OSType creator);
	type_creator_link_t ** hashpp (OSType type, OSType creator);
	int insert_into_hash_table (
		OSType type,
		OSType creator,
		local_icn_sharp_t *ip16,
		local_icn_sharp_t *ip32
		);
	icn_sharp_hand map_type_and_creator (
		OSType type,
		OSType creator,
		int prefer_small );
	void process_bundle2 (
		CFile *fp,
		unsigned long fork_start,
		unsigned long creator,
		uint64_t fork_length=UINT64_MAX );
	void mac_load_icon2(
		int volinx,
		CatDataRec *pCDR,
		Integer id,
		int small,
		local_icn_sharp_t *dat );
	void mac_load_icon3(
		CFile *fp,
		unsigned long g_offset,
		Integer id,
		int small,
		local_icn_sharp_t *dat );
	void init_hash_table();
	void clear_hash_table();
	int get_apple_double(
		CString dir,
		LPSTR s,
		HICON *phicon16,
		HICON *phicon32,
		LPSTR pure_name,
		LPSTR stype,
		LPSTR screator,
		int *do_show
	);
	int get_afp(
		CString dir,
		LPSTR s,
		HICON *phicon16,
		HICON *phicon32,
		LPSTR pure_name,
		LPSTR stype,
		LPSTR screator,
		int *do_show
	);
	int get_icon_cache_count();
	void get_icons_file_name( CString &name );
	void load_hash_table();
	void create_cache_icon( local_icn_sharp_t *dat, int invert );
private:
 void process_bundle(const CatDataRec*);
 static void OwnIcons(BrowserDirectory::Snapshot&);
 bool prepare_classic_browser_row(hfs_item_type*,CatDataRec*,int,BrowserDirectory::Row&);
 std::map<uint64_t,std::shared_ptr<BrowserDirectory::OwnedIcons>> bundleIcons_;
 MacBrowser::Volume* artworkVolume_=nullptr;
 Options options_;
 std::map<int,BrowserMountInventory::ClassicMount> slots_;
 CPointerWordArray icons_;
 type_creator_link_t* hash_table[ICONTABLESIZE]{};
 int icons_in_cache=0,hash_file_ok=0,hash_file_impossible=0;
 CFile hash_file;
};
