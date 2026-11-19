// SPDX-License-Identifier: GPL-2.0-or-later
// Includes HFVExplorer resource code by Lauri Pesonen, Copyright 1997-1998 Anygraaf Oy.
#include "stdafx.h"
#include "ClassicCatalog.h"
#include "BrowserFolderReader.h"
#include "HFVExplorer.h"
#include "adouble.h"
#include "aspecial.h"
#include "utils.h"

BrowserFolderReader::BrowserFolderReader(Options options,BrowserMountInventory* borrowed)
 : options_(options),slots_(borrowed?borrowed->classic:ownedSlots_),
   revisions_(borrowed?borrowed->classicRevision:ownedRevisions_)
{
 init_mac_icons();
}

BrowserFolderReader::~BrowserFolderReader()
{
 clear_hash_table();
 destroy_icon_cache();
}

void BrowserFolderReader::init_mac_icons()
{
	icons_.SetSize( 200, 20 );
	init_hash_table();
}

void BrowserFolderReader::destroy_icon_cache()
{
	int i, count = icons_.GetSize();
	DWORD_PTR dw;

	for(i=0; i<count; i++) {
		dw = icons_.GetAt( i );
		if(dw) {
			DestroyIcon( (HICON)dw );
		}
	}
	icons_.RemoveAll();
}

void BrowserFolderReader::add_to_icon_cache( HICON hicon )
{
	if(hicon) icons_.Add( (DWORD_PTR)hicon );
}

unsigned long get_magic( CFile *fp )
{
	apple_single_double_header h;

	fp->Read( &h, sizeof(apple_single_double_header) );
	mappc_apple_single_double_header(&h);
	return(h.Magic);
}

unsigned long get_section_offset(
	CFile *fp,
	LongInt id,
	unsigned long *plength )
{
	apple_single_double_header h;
	apple_single_double_entry e;
	int i;

	if(plength) *plength = 0;
	fp->Seek( 0, CFile::begin );
	fp->Read( &h, sizeof(apple_single_double_header) );
	mappc_apple_single_double_header(&h);
	if(h.Magic == APPLE_DOUBLE_MAGIC || h.Magic == APPLE_SINGLE_MAGIC) {
		for(i=0; i<h.entry_count; i++) {
			fp->Read( &e, sizeof(apple_single_double_entry) );
			mappc_apple_single_double_entry(&e);
			if(e.id == id) {
				if(plength) *plength = e.length;
				return( e.offset );
			}
		}
	}
	return( 0 );
}

/* sample document appledouble
	id	0x0000000a  - mac file info
	offset	0x00000092
	length	0x00000004

	id	0x00000008 - file dates info
	offset	0x00000096
	length	0x00000010

	id	0x00000009 - finder info
	offset	0x000000a6
	length	0x00000020

	id	0x00000002 - res fork
	offset	0x00000200
	length	0x00000166
*/

extern "C" {
int get_apple_double_type_creator(
	char *name,
	char *stype, char *screator,
	unsigned long *pstart,
	unsigned long *plength
	)
{
	int ok = 0;
	unsigned long magic, start;
	CFile f, *fp;
	unsigned long type = 0;
	unsigned long creator = 0;
	FInfo finfo;

	fp = &f;
	finfo.fdFlags = 0;

	lstrcpy( stype, "TEXT" );
	lstrcpy( screator, "????" );

	if(!do_open_file( fp, CString(name) ) ) {
		return(0);
	}
	magic = get_magic( fp );
	if(magic == APPLE_SINGLE_MAGIC) {
		// to make this work properly we should open EVERY file.
	} else if(magic == APPLE_DOUBLE_MAGIC) {
		*pstart = get_section_offset( fp, Resource_Fork, plength );
		if(*pstart) {
			start = get_section_offset( fp, Finder_Info, NULL );
			if(start) {
				fp->Seek( start, CFile::begin );
				fp->Read( &finfo, sizeof(FInfo) );
				type = finfo.fdType;
				creator = finfo.fdCreator;
				MACPC_D(type);
				MACPC_D(creator);
				ok = 1; // Executor makes sometimes 0,0 AppleDouble.
				if(type && creator) {
					long_to_string( type, (unsigned char *)stype );
					long_to_string( creator, (unsigned char *)screator );
				}
			}
		}
	}
	silent_close( fp );

	return(ok);
}
} // extern "C"

// s is of form %<name>. is there <name> ?
int BrowserFolderReader::get_apple_double(
	CString dir,
	LPSTR s,
	HICON *phicon16,
	HICON *phicon32,
	LPSTR pure_name,
	LPSTR stype,
	LPSTR screator,
	int *do_show
)
{
	HANDLE fh;
	WIN32_FIND_DATA FindFileData;
	char search_name[MAX_PATH];
	int ok = 0;

	if(*s == '%') {
		strcpy( pure_name, &s[1] );
		strcpy( search_name, pure_name );
	} else {
		strcpy( search_name, "%" );
		strcat( search_name, s );
		strcpy( pure_name, s );
	}

	*do_show = 1;

	fh = FindFirstFile( dir + search_name, &FindFileData );
	if(fh != INVALID_HANDLE_VALUE) {
		FindClose( fh );
		unsigned long magic, start;

		// pair found ...
		CFile f, *fp;
		fp = &f;
		*phicon16 = *phicon32 = 0;
		if(!do_open_file( fp, dir + CString("%") + CString(pure_name) ) ) {
			return(0);
		}
		magic = get_magic( fp );
		if(magic == APPLE_SINGLE_MAGIC) {
			// to make this work properly we should open EVERY file.
		} else if(magic == APPLE_DOUBLE_MAGIC) {
			unsigned long type = 0;
			unsigned long creator = 0;
			FInfo finfo;

			finfo.fdFlags = 0;

			// if(strcmp(pure_name,"Browser") == 0) shadow = 1;

			*stype = 0;
			*screator = 0;
			start = get_section_offset( fp, Finder_Info, NULL );
			if(start) {
				fp->Seek( start, CFile::begin );
				fp->Read( &finfo, sizeof(FInfo) );
				type = finfo.fdType;
				creator = finfo.fdCreator;
				MACPC_D(type);
				MACPC_D(creator);
				MACPC_W(finfo.fdFlags);
			}
			if(type && creator) {
				if((finfo.fdFlags & fInvisible) && !options_.showMacHidden) {
					*do_show = 0;
					return(1);
				}
				start = get_section_offset( fp, Resource_Fork, NULL );
				if(start) {
					if(finfo.fdFlags & kHasCustomIcon) {
						// AfxMessageBox( "File " + CString(pure_name) + " has custom icon." );
#ifndef ZAPPER
						load_custom_icons( fp, start, phicon16, phicon32 );
#endif
					}
					if(!*phicon32) *phicon32 = map_type_and_creator (type, creator, 0 );
					if(!*phicon16) *phicon16 = map_type_and_creator (type, creator, 1 );

					if(finfo.fdFlags & kHasBundle) {
						if(!*phicon16 || !*phicon32) {
							process_bundle2( fp, start, creator );
							if(!*phicon32) *phicon32 = map_type_and_creator (type, creator, 0 );
							if(!*phicon16) *phicon16 = map_type_and_creator (type, creator, 1 );
						}
					}
				}
				long_to_string( type, (unsigned char *)stype );
				long_to_string( creator, (unsigned char *)screator );
			}
			// if(strcmp(pure_name,"Browser") == 0) shadow = 0;
			ok = 1;
		}
		silent_close( fp );
	}
	return(ok);
}

int BrowserFolderReader::get_afp(
	CString dir,
	LPSTR s,
	HICON *phicon16,
	HICON *phicon32,
	LPSTR pure_name,
	LPSTR stype,
	LPSTR screator,
	int *do_show
)
{
	int ok = 0;
	unsigned long type = 0;
	unsigned long creator = 0;
	FInfo finfo;
	CFile f, *fp = &f;

	strcpy( pure_name, s );
	finfo.fdFlags = 0;

	*do_show = 1;

	*phicon16 = *phicon32 = 0;
	if(!do_open_file( fp, dir + s + CString(":AFP_AfpInfo") ) ) {
		return(0);
	}
	*stype = 0;
	*screator = 0;
	fp->Seek( 16, CFile::begin );
	fp->Read( &finfo, sizeof(FInfo) );
	type = finfo.fdType;
	creator = finfo.fdCreator;
	MACPC_D(type);
	MACPC_D(creator);
	MACPC_W(finfo.fdFlags);
	silent_close( fp );

	if((finfo.fdFlags & fInvisible) && !options_.showMacHidden) {
		*do_show = 0;
		return(1);
	}

	if(!do_open_file( fp, dir + s + CString(":AFP_Resource") ) ) {
		return(0);
	}

	if(type && creator) {
		if(finfo.fdFlags & kHasCustomIcon) {
			// AfxMessageBox( "File " + CString(pure_name) + " has custom icon." );
#ifndef ZAPPER
			load_custom_icons( fp, 0, phicon16, phicon32 );
#endif
		}
		if(!*phicon32) *phicon32 = map_type_and_creator (type, creator, 0 );
		if(!*phicon16) *phicon16 = map_type_and_creator (type, creator, 1 );

		if(finfo.fdFlags & kHasBundle) {
			if(!*phicon16 || !*phicon32) {
				process_bundle2( fp, 0, creator );
				if(!*phicon32) *phicon32 = map_type_and_creator (type, creator, 0 );
				if(!*phicon16) *phicon16 = map_type_and_creator (type, creator, 1 );
			}
		}
		long_to_string( type, (unsigned char *)stype );
		long_to_string( creator, (unsigned char *)screator );
	}
	ok = 1;
	silent_close( fp );
	return(ok);
}



int BrowserFolderReader::walk_resource_fork( CFile *fp, LongInt type_to_check, Integer id_to_check )
{
	resource_header res_header;
	resource_map_header map_header;
	resource_type_item type_item;
	resource_ref_list_entry ref_entry;
	int res, typ;
	hfv_offset_t offs1, offs2, res_data_offset;
	Integer type_count1; // count-1
	LongInt data_len;
	hfv_offset_t g_offset;

	UPDATE_TIME_CONSUMING(1);

	g_offset = fp->Seek( 0, CFile::current );

	fp->Read( &res_header, sizeof(resource_header) );
	macpc_resource_header( &res_header );
	fp->Seek( (hfv_offset_t)g_offset+res_header.resource_map_offset, CFile::begin );
	fp->Read( &map_header, sizeof(resource_map_header) );
	macpc_resource_map_header( &map_header );

	offs1 = (hfv_offset_t)g_offset + res_header.resource_map_offset + map_header.type_list_offset;
	fp->Seek( offs1, CFile::begin );
	fp->Read( &type_count1, sizeof(type_count1) );
	MACPC_W(type_count1);
	offs1 += sizeof(type_count1);

	// Bad mistake! Can't do it this way. They use 65535 as zero...
	// for( typ=0; typ<=type_count1; typ++ ) {

	type_count1++;
	for( typ=0; typ<type_count1; typ++ ) {


		fp->Seek( offs1 + typ * sizeof(resource_type_item), CFile::begin );
		fp->Read( &type_item, sizeof(resource_type_item) );
		macpc_resource_type_item( &type_item );
		if(type_item.res_type == type_to_check) {
			offs2 = (hfv_offset_t)res_header.resource_map_offset +
							map_header.type_list_offset + type_item.ref_list_offset;
			offs2 += g_offset;

			Integer res_count = type_item.res_count1 + 1;
			for( res=0; res<res_count; res++ ) {
				fp->Seek( offs2 + res * sizeof(resource_ref_list_entry), CFile::begin );
				fp->Read( &ref_entry, sizeof(resource_ref_list_entry) );
				macpc_resource_ref_list_entry( &ref_entry );
				if( ref_entry.res_id == id_to_check ) {
					// found item
					res_data_offset =
							(hfv_offset_t)g_offset + res_header.resource_data_offset +
							( ((long)ref_entry.res_data_offset_hi << 16) |
								ref_entry.res_data_offset_lo );
					fp->Seek( res_data_offset, CFile::begin );
					// now read data
					fp->Read( &data_len, sizeof(data_len) );
					MACPC_D(data_len);
					// read "data_len" bytes
				}
			}
		}
	}
	return(1);
}

int BrowserFolderReader::walk_double_resource_fork( CFile *fp, LongInt type_to_check, Integer id_to_check )
{
	apple_single_double_header h;
	apple_single_double_entry e;
	int i;

	fp->Read( &h, sizeof(apple_single_double_header) );
	mappc_apple_single_double_header(&h);
	if(h.Magic == APPLE_DOUBLE_MAGIC) {
		for(i=0; i<h.entry_count; i++) {
			fp->Read( &e, sizeof(apple_single_double_entry) );
			mappc_apple_single_double_entry(&e);
			if(e.id == Resource_Fork) {
				fp->Seek( e.offset, CFile::begin );
				walk_resource_fork( fp, type_to_check, id_to_check );
				break;
			}
		}
	}
	return(1);
}

HANDLE BrowserFolderReader::mac_load_any_resource(
    int, CatDataRec* record, LongInt type, int anyId, Integer resourceId)
{
    if(!artworkVolume_||!record->u.f.filRLgLen)return nullptr;
    const auto size=uint64_t(uint32_t(record->u.f.filRLgLen));
    const auto read=[&](uint64_t offset,void* data,size_t length){
        MacBrowser::ReadClassicArtwork(*artworkVolume_,record->u.f.filFlNum,offset,data,length);
    };
    const auto contains=[](uint64_t regionSize,uint64_t offset,uint64_t length){
        return offset<=regionSize&&length<=regionSize-offset;
    };
    try{
        resource_header header{};
        if(size<sizeof(header))return nullptr;
        read(0,&header,sizeof(header));
        macpc_resource_header(&header);
        if(!contains(size,header.resource_map_offset,header.resource_map_length)||
           !contains(size,header.resource_data_offset,header.resource_data_length)||
           header.resource_map_length<sizeof(resource_map_header))return nullptr;

        resource_map_header map{};
        read(header.resource_map_offset,&map,sizeof(map));
        macpc_resource_map_header(&map);
        const auto types=uint64_t(uint16_t(map.type_list_offset));
        uint16_t count{};
        if(!contains(header.resource_map_length,types,sizeof(count)))return nullptr;
        read(header.resource_map_offset+types,&count,sizeof(count));
        MACPC_W(count);
        for(uint16_t index=0;index<=count&&count!=UINT16_MAX;++index){
            resource_type_item item{};
            const auto offset=types+sizeof(count)+index*sizeof(item);
            if(!contains(header.resource_map_length,offset,sizeof(item)))return nullptr;
            read(header.resource_map_offset+offset,&item,sizeof(item));
            macpc_resource_type_item(&item);
            if(item.res_type!=type)continue;

            const auto refs=types+uint16_t(item.ref_list_offset);
            const auto last=uint16_t(item.res_count1);
            for(uint16_t index=0;index<=last&&last!=UINT16_MAX;++index){
                resource_ref_list_entry ref{};
                const auto offset=refs+index*sizeof(ref);
                if(!contains(header.resource_map_length,offset,sizeof(ref)))return nullptr;
                read(header.resource_map_offset+offset,&ref,sizeof(ref));
                macpc_resource_ref_list_entry(&ref);
                if((ref.res_attrib&resCompressed)||(!anyId&&ref.res_id!=resourceId))continue;

                const auto data=(uint32_t(ref.res_data_offset_hi)<<16)|uint16_t(ref.res_data_offset_lo);
                uint32_t length{};
                if(!contains(header.resource_data_length,data,sizeof(length)))return nullptr;
                const auto start=uint64_t(header.resource_data_offset)+data;
                read(start,&length,sizeof(length));
                MACPC_D(length);
                if(length>LONG_MAX||!contains(header.resource_data_length,data+sizeof(length),length))
                    return nullptr;
                // The icon decoder uses the zero-filled tail as scratch space.
                auto handle=GlobalAlloc(GHND,SIZE_T(length)+2000);
                if(!handle)return nullptr;
                auto buffer=GlobalLock(handle);
                if(!buffer){GlobalFree(handle);return nullptr;}
                try{
                    read(start+sizeof(length),buffer,length);
                }catch(...){
                    GlobalUnlock(handle);
                    GlobalFree(handle);
                    throw;
                }
                GlobalUnlock(handle);
                return handle;
            }
        }
    }catch(const HfsPlus::Error&){
        // An unreadable resource fork does not prevent the folder from opening.
    }
    return nullptr;
}

HANDLE BrowserFolderReader::mac_load_any_resource2(
	CFile *fp,
	unsigned long g_offset,
	LongInt type_to_check,
	int nocheck,
	Integer id_to_check )
{
	resource_header res_header;
	resource_map_header map_header;
	resource_type_item type_item;
	resource_ref_list_entry ref_entry;
	int res, typ;
	hfv_offset_t offs1, offs2, res_data_offset;
	Integer type_count1; // count-1
	LongInt data_len;
	HANDLE h = 0;

	fp->Seek( g_offset, CFile::begin );
	fp->Read( &res_header, sizeof(resource_header) );
	macpc_resource_header( &res_header );
	if(!res_header.resource_data_length ||
		 !res_header.resource_map_length) return(0);
	fp->Seek( (hfv_offset_t)g_offset+res_header.resource_map_offset, CFile::begin );
	fp->Read( &map_header, sizeof(resource_map_header) );
	macpc_resource_map_header( &map_header );

	offs1 = (hfv_offset_t)g_offset + res_header.resource_map_offset + map_header.type_list_offset;
	fp->Seek( offs1, CFile::begin );
	fp->Read( &type_count1, sizeof(type_count1) );
	MACPC_W(type_count1);
	offs1 += sizeof(type_count1);
	for( typ=0; typ<=type_count1; typ++ ) {
		fp->Seek( offs1 + typ * sizeof(resource_type_item), CFile::begin );
		fp->Read( &type_item, sizeof(resource_type_item) );
		macpc_resource_type_item( &type_item );
		if(type_item.res_type == type_to_check) {
			offs2 = (hfv_offset_t)res_header.resource_map_offset +
							map_header.type_list_offset + type_item.ref_list_offset;
			offs2 += g_offset;

			Integer res_count = type_item.res_count1+1;
			for( res=0; res<res_count; res++ ) {
				fp->Seek( offs2 + res * sizeof(resource_ref_list_entry), CFile::begin );
				fp->Read( &ref_entry, sizeof(resource_ref_list_entry) );
				macpc_resource_ref_list_entry( &ref_entry );
				if( (ref_entry.res_attrib & resCompressed) == 0) {
					if( nocheck || ref_entry.res_id == id_to_check ) {
						res_data_offset =
								(hfv_offset_t)g_offset + res_header.resource_data_offset +
								( ((long)ref_entry.res_data_offset_hi << 16) |
									ref_entry.res_data_offset_lo );
						fp->Seek( res_data_offset, CFile::begin );
						// now read data
						fp->Read( &data_len, sizeof(data_len) );
						MACPC_D(data_len);
						// read "data_len" bytes
						// Add 2000 to make space for compressed icons ...
						h = GlobalAlloc( GHND, (SIZE_T)data_len+2000 );
						if(h) {
							LPSTR p = (LPSTR)GlobalLock( h );
							fp->Read( p, data_len );
							GlobalUnlock( h );
							return(h);
						}
					}
				}
			}
		}
	}
	return(h);
}

// #include "bndl.cpp"

HICON BrowserFolderReader::mac_load_icon( int volinx, CatDataRec *pCDR, int small )
{
	HICON hicon = 0;
	HANDLE h = 0;
	int dim;

	int iscolor = 0;

	dim = small ? 16 : 32;

	if(pCDR->u.f.filUsrWds.fdFlags & kHasBundle) {
		if(options_.bitsPerPixel >= 8) {
			h = mac_load_any_resource (
						volinx, pCDR,
						small ? (unsigned long)'ics8' :	(unsigned long)'icl8',
						1, 0 );
		}
		if(h) {
			iscolor = 1;
		} else {
			h = mac_load_any_resource(
						volinx,
						pCDR,
						small ? (unsigned long)'ics#' : (unsigned long)'ICN#',
						1, 0 );
		}

		if(h) {
			unsigned char *p = (unsigned char *)GlobalLock( h );
			CHFVExplorerApp	*pApp;
			pApp = (CHFVExplorerApp *)AfxGetApp();

			if(iscolor) {
				unsigned char mask[COLORICONSIZE+100];
				int bytes;

				bytes = small ? (COLORICONSIZE>>2) : COLORICONSIZE;
				memset( mask, 0, bytes );
				negate_buffer( (unsigned char *)p, bytes );

				/*
				int i;
				for(i=0; i<bytes; i++) {
					if(p[i] == (unsigned char)255) {
						mask[i] = (unsigned char)p[i];
						// p[i] = (unsigned char)0;
					}
				}
				*/

				hicon = CreateIcon(
					pApp->m_hInstance, dim, dim, 1, 8,
					(unsigned char *)mask,
					(unsigned char *)p
				);
			} else {
				int half;
				negate_buffer( (unsigned char *)p, small ? (BW_ICONSIZE>>2) : BW_ICONSIZE );
				half = (BW_ICONSIZE>>1);
				if(small) half = (BW_ICONSIZE>>3);
				hicon = CreateIcon(
					pApp->m_hInstance, dim, dim, 1, 1,
					(unsigned char *)p,
					(unsigned char *)p + half
				);
			}
			GlobalUnlock( h );
		}
	}
	return(hicon);
}
