// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "BrowserMountInventory.h"
#include "BrowserDirectory.h"
#include "itemtype.h"
#include "bndl.h"

class BrowserFolderReader {
public:
 struct Options {
  bool showWindowsHidden=false,showMacHidden=false;
  int bitsPerPixel=1,windowsIcons=0,windowsIconTimeout=0;
 };

 explicit BrowserFolderReader(Options options,BrowserMountInventory* borrowed=nullptr);
 ~BrowserFolderReader();
 BrowserFolderReader(const BrowserFolderReader&)=delete;
 BrowserFolderReader& operator=(const BrowserFolderReader&)=delete;

 BrowserMountInventory::ClassicSlots& Slots(){return slots_;}
 uint64_t& Revision(int index){return revisions_[index];}
 void SetOptions(Options options){options_=options;}
 BrowserDirectory::Snapshot read_classic_browser_directory(int,long,CString,bool,long,Storage::Job* job=nullptr);
 BrowserDirectory::Snapshot read_windows_browser_directory(int,const std::wstring&,bool,Storage::Job&,std::optional<DWORD> expectedSerial={});
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
	HANDLE get_fref( int volinx, CatDataRec *pCDR, INTEGER id );
	HANDLE get_fref( CFile *fp, unsigned long g_offset, INTEGER id );
	unsigned long hash_func (OSType type, OSType creator);
	type_creator_link_t ** hashpp (OSType type, OSType creator);
	int insert_into_hash_table (
		OSType type,
		OSType creator,
		local_icn_sharp_t *ip16,
		local_icn_sharp_t *ip32
		);
	bndl_section_t *extract_bndl_section (bndl_t **bndl_h, ResType code);
	int find_local_icon(
		INTEGER local_id,
		local_icn_sharp_t
		local_icns[],
		INTEGER n_icn );
	void process_bndl (
		int volinx,
		CatDataRec *pCDR,
		bndl_t **bndl_h,
		OSType creator
	);
	void process_bndl2 (
		CFile *fp,
		unsigned long g_offset,
		bndl_t **bndl_h,
		OSType creator
	);
	icn_sharp_hand map_type_and_creator (
		OSType type,
		OSType creator,
		int prefer_small );
	void process_bundle(
		int volinx,
		CatDataRec *pCDR,
		unsigned long creator );
	void process_bundle2 (
		CFile *fp,
		unsigned long fork_start,
		unsigned long creator );
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
	void save_hash_table(
		OSType type,
		OSType creator,
		local_icn_sharp_t *ip16,
		local_icn_sharp_t *ip32
	);
	void create_cache_icon( local_icn_sharp_t *dat, int invert );
private:
 static void OwnIcons(BrowserDirectory::Snapshot&);
 bool prepare_classic_browser_row(hfs_item_type*,CatDataRec*,int,BrowserDirectory::Row&);
 Options options_;
 BrowserMountInventory::ClassicSlots ownedSlots_;
 BrowserMountInventory::ClassicSlots& slots_;
 uint64_t ownedRevisions_[MAX_HFS_VOLUMES]{};
 uint64_t* revisions_;
 CPointerWordArray icons_;
 type_creator_link_t* hash_table[ICONTABLESIZE]{};
 int icons_in_cache=0,hash_file_ok=0,hash_file_impossible=0;
 bool hash_file_writable=false;
 CFile hash_file;
};
