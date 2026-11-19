/*
 * HFVExplorer
 * Copyright (C) 1997-1998 by Anygraaf Oy
 * Author: Lauri Pesonen, email: lpesonen@clinet.fi or lauri.pesonen@anygraaf.fi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef _DOC_INCLUDED
#define _DOC_INCLUDED

#include "MacTypes.h"
#include "itemtype.h"
#include "BrowserDirectory.h"
#include "bndl.h"
#include "BrowserSession.h"

// #define USE_FILE_MAPPING

#define MAX_HFS_VOLUMES 50
#define MAX_FAT_VOLUMES 50

#include "mactypes.h"
#include "CFATVolume.h"

#define SEARCH_TYPE_ID			0
#define SEARCH_TYPE_FILES		1
#define SEARCH_TYPE_FOLDERS 2
#define SEARCH_TYPE_ALL			3
#define SEARCH_TYPE_THISFOLDER	4
#define SEARCH_TYPE_FIND_NAME		6

#define ID_GOTO_PARENT 0x12345678

namespace BrowserNavigation { struct Request; }
class CHFVExplorerDoc : public CDocument
{
 friend class MainMacBrowser;
 friend class BrowserSession;
 std::unique_ptr<BrowserSession> m_browserSession;
 bool browserOpened_=false;
 int BuildBrowserRoots(int preferred, bool readClassic=true);
 bool NavigateInitialFolder(int firstClassic=0);
 uint64_t m_classicRevision[MAX_HFS_VOLUMES]{};
 type_creator_link_t* hash_table[ICONTABLESIZE]{};
 int icons_in_cache=0,hash_file_ok=0,hash_file_impossible=0;
 bool hash_file_writable=false;
 CFile hash_file;
 CHFVFile* speed_fp=nullptr;
protected: // create from serialization only
	CPointerWordArray *m_icon_array;
	CHFVExplorerDoc(bool discover = true);
	DECLARE_DYNCREATE(CHFVExplorerDoc)
	void OnCleanDocument();
	void enum_floppies( void );
	void set_floppy_volume_dirty( char *path );
	void CheckNewMedia( BOOL check_floppies, BOOL check_cds, BOOL check_hds );
	void CheckNewFloppy( int index );

	int ignore_this( CObArray *parr, WIN32_FIND_DATA *fd );
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

	BOOL find_volume_MDB();
	void hfs_init();

	void new_hfs_walk( int volinx, long id, CString searchname );

	BOOL hfs_find_item( LongInt node_index, hfs_item_type *pitem );
	BOOL hfs_find_first( 
		int volinx,
		long id, 
		long id2, 
		CString searchname,
		hfs_item_type *pitem, 
		int stype );
	BOOL hfs_find_next( hfs_item_type *pitem );

    bool prepare_classic_browser_row(hfs_item_type*,CatDataRec*,int,BrowserDirectory::Row&);
    BrowserDirectory::Snapshot read_classic_browser_directory(int,long,CString,bool,long,Storage::Job* job=nullptr);
    BrowserDirectory::Snapshot read_windows_browser_directory(int,const char*,bool,Storage::Job&,std::optional<DWORD> expectedSerial={});
    void apply_browser_directory(const BrowserDirectory::Snapshot&,int,int,uint32_t,const std::string&,bool);
    bool NavigateStartupDirectory(const std::string& path,bool mac);
    bool NavigateClassicFolder(int index, uint32_t id, bool refresh = false, const BrowserNavigation::Request* find = nullptr);
    void PublishClassicFolder(int index, uint32_t id, const std::string& path,
                              const BrowserDirectory::Snapshot&);
    bool NavigateWindowsFolder(int index, const std::string& path, bool refresh = false, const BrowserNavigation::Request* find = nullptr);
    void PublishWindowsFolder(int index, const std::string& path, const BrowserDirectory::Snapshot&);
    void CopyNavigationIcons(BrowserDirectory::Snapshot&);
	void fat_walk( int volinx, LPSTR path, int list );
	void open_sub_directory( int volinx, unsigned long id, CString partial_name );
	void open_fat_directory( int volinx, LPSTR path, int list );
	void build_pc_tree( int volinx, CString path );
	CString hfs_find_match( LPSTR type, LPSTR creator, int prefered_volinx );
	void check_hfv_file_times(int ask);
	int get_volume_index_by_filename( CString name );
	int get_volume_index_by_volumename( CString name );
	int insert_volume( CString name, int is_floppy, int is_cd, int is_hd, int is_removable );
	void enum_hfs_volumes(LPSTR mask,BOOL floppies);
	void enum_volumes_ext( LPSTR mask, LPSTR ext );
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
	void open_parent_directory( unsigned long id );
	CString get_directory( HTREEITEM hItem, int *pmac );
	int my_split_path( CString path, CString &volname, int *volinx, CString &dir, int mac );
	void my_chdir( CString path, int mac, int preferredVolume = -1 );
	void set_current_hitem( HTREEITEM hItem );
	HTREEITEM get_current_hitem();

	// FAT stuff
	HICON get_fat_icon( UINT type );
	int insert_fat_volume( char letter, UINT type );
	void enum_fat_volumes();

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
	void CHFVExplorerDoc::save_hash_table(
		OSType type, 
		OSType creator, 
		local_icn_sharp_t *ip16,
		local_icn_sharp_t *ip32
	);
	void create_cache_icon( local_icn_sharp_t *dat, int invert );
	void update_invisible_attributes();
	void set_hfs_volume_clean( char *path );

// Attributes
public:
 BrowserSession& GetBrowserSession()const{return *m_browserSession;}
    class VolumeSlots {
        std::unique_ptr<CHFVVolume> volumes_[MAX_HFS_VOLUMES];
    public:
        CHFVVolume& operator[](int index) {
            auto& volume = volumes_[index];
            if (!volume) volume = std::make_unique<CHFVVolume>();
            return *volume;
        }
        void ReplaceFrom(int index, VolumeSlots& source, int slot) {
            volumes_[index].swap(source.volumes_[slot]);
        }
    } m_volumes;
	CFATVolume m_fats[MAX_FAT_VOLUMES];
	int m_hfs_count;
	int m_fat_count;
	int m_bits_per_pixel;
	int m_mac_show_invisibles;
	int m_pc_show_invisibles;

// Operations
public:

// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CHFVExplorerDoc)
	public:
	virtual BOOL OnNewDocument();
	virtual void Serialize(CArchive& ar);
	virtual BOOL OnOpenDocument(LPCTSTR lpszPathName);
	virtual void OnCloseDocument();
 void FinishBackgroundClose();
	//}}AFX_VIRTUAL

// Implementation
public:
	virtual ~CHFVExplorerDoc();
#ifdef _DEBUG
	virtual void AssertValid() const;
	virtual void Dump(CDumpContext& dc) const;
#endif

protected:

// Generated message map functions
protected:
	//{{AFX_MSG(CHFVExplorerDoc)
	afx_msg void OnViewRefresh();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

/////////////////////////////////////////////////////////////////////////////
#endif
