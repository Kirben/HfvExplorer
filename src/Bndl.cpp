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

#include "stdafx.h"
#include "MacIcons.h"
#include "HFVExplorer.h"

#include "BrowserFolderReader.h"
#include "HFVExplorerListView.h"
#include "CFATVolume.h"
#include "special.h"
// #include "utils.h"
#include "hfs\ClassicHfs.h"
#include "hfs\interface.h"
#include "AskNewVolume.h"
#include "MainFrm.h"
#include "floppy.h"
#include "adouble.h"
#include "aspecial.h"

#include <stdlib.h>
#include "utils.h"

// #define NO_SMALL_ICONS

enum { HASH_SIZE = ICONTABLESIZE };

// Icon tables and file handles are owned by BrowserFolderReader.

void dump_data( unsigned char *fname, unsigned char *p, int count )
{
	CFile f, *fp;

	unlink( (char*)fname);

	fp = &f;

	if(do_create_file( fp, CString(fname) )) {
		TRY
		{
		fp->Write( p, count );
		}
		CATCH( CFileException, e )
		{
		}
		END_CATCH
		silent_close( fp );
	}
}

void BrowserFolderReader::init_hash_table()
{
  CString ini_name;
	CFile *fp;

	fp = &hash_file;

	if(hash_file_ok) {
		hash_file_ok = 0;
		silent_close( fp );
	}

	get_icons_file_name( ini_name );
    if(!ini_name.IsEmpty()){
        CFileException error;
        hash_file_ok=fp->Open(ini_name,CFile::modeRead|CFile::shareDenyNone,&error);
    }

	memset( hash_table, 0, sizeof(type_creator_link_t *) * HASH_SIZE );
	icons_in_cache = 0;

	load_hash_table();
}

int BrowserFolderReader::exists_icon_cache_file()
{
	CString name;
	char *cname;
	int ret;

	get_icons_file_name( name );
	cname = name.GetBuffer( _MAX_PATH );
	if(exists( cname )) {
		ret = 1;
	} else {
		ret = 0;
	}
	name.ReleaseBuffer();
	return(ret);
}

int BrowserFolderReader::delete_cache_OK()
{
	CString name;
	char *cname;
	int ret;

	get_icons_file_name( name );
	cname = name.GetBuffer( _MAX_PATH );
	if(exists( cname )) {
		silent_delete( name );
		ret = 1;
	} else {
		ret = 0;
	}
	name.ReleaseBuffer();
	return(ret);
}
 
int BrowserFolderReader::get_icon_cache_count()
{
	return(icons_in_cache);
}

void BrowserFolderReader::get_icons_file_name( CString &name )
{
	CHFVExplorerApp	*pApp;
	pApp = (CHFVExplorerApp *)AfxGetApp();

	name = "";

	char modulex[MAX_PATH];
	GetModuleFileName( pApp->m_hInstance, modulex, MAX_PATH-1 );
	
	CString module = CString( modulex );
	int i = module.ReverseFind( '\\' );
	if( i > 0 ) {
		name = module.Left( i ) + CString("\\HFVICONS.DAT");
	}
}

typedef struct {
	int i1, i2, i3;
} three_ints;

typedef struct {
	OSType type, creator;
} two_OSType;

void BrowserFolderReader::load_hash_table()
{
	CString ini_name;
	CFile *fp;
	two_OSType owner;
	char *buf;
	HANDLE h16, h32;
	// unsigned char *cim16, *cim32;

	local_icn_sharp_t ip16;
	local_icn_sharp_t ip32;

	unsigned char *mask16 = 0;
	unsigned char *mask32 = 0;

	three_ints ints3;

	if(!hash_file_ok) return;

	h16 = GlobalAlloc( GHND, 10000 );
	if(!h16) return;

	h32 = GlobalAlloc( GHND, 10000 );
	if(!h32) return;

	mask16 = (unsigned char *)malloc( 10000 );
	if(!mask16) return;

	mask32 = (unsigned char *)malloc( 10000 );
	if(!mask32) return;

	TRY
	{
	fp = &hash_file;
	while(1) {
		ip16.icon = 0;
		ip32.icon = 0;
		ip16.h = 0;
		ip32.h = 0;
		ip16.color_icon_mask = 0;
		ip32.color_icon_mask = 0;

		if(fp->Read( &owner, sizeof(owner) ) != sizeof(owner)) break;

		if(fp->Read( &ip16.bytes, sizeof(ip16.bytes) ) != sizeof(ip16.bytes)) break;
		if(ip16.bytes > 1024 || ip16.bytes < 0) {
			delete_cache();
			goto panic;
		}
		if(ip16.bytes) {
			ip16.h = h16;
			buf = (char *)GlobalLock(ip16.h);
			if((int)fp->Read( buf, ip16.bytes ) != ip16.bytes) break;
			GlobalUnlock(ip16.h);
			if((int)fp->Read( &ip16.color_icon_mask, sizeof(ip16.color_icon_mask) ) != sizeof(ip16.color_icon_mask)) break;
			if(ip16.color_icon_mask) {
				ip16.color_icon_mask = mask16;
				if((int)fp->Read( ip16.color_icon_mask, ip16.bytes ) != ip16.bytes) break;
			}
		}
		if(fp->Read( &ints3, sizeof(ints3) ) != sizeof(ints3)) break;
		ip16.small = ints3.i1;
		ip16.iscolor = ints3.i2;
		ip16.dim = ints3.i3;

		if(fp->Read( &ip32.bytes, sizeof(ip32.bytes) ) != sizeof(ip32.bytes)) break;
		if(ip32.bytes > 1024 || ip32.bytes < 0) {
			delete_cache();
			goto panic;
		}
		if(ip32.bytes) {
			ip32.h = h32;
			buf = (char *)GlobalLock(ip32.h);
			if((int)fp->Read( buf, ip32.bytes ) != ip32.bytes) break;
			GlobalUnlock(ip32.h);
			if((int)fp->Read( &ip32.color_icon_mask, sizeof(ip32.color_icon_mask) ) != sizeof(ip32.color_icon_mask)) break;
			if(ip32.color_icon_mask) {
				ip32.color_icon_mask = mask32;
				if((int)fp->Read( ip32.color_icon_mask, ip32.bytes ) != ip32.bytes) break;
			}
		}
		if(fp->Read( &ints3, sizeof(ints3) ) != sizeof(ints3)) break;
		ip32.small = ints3.i1;
		ip32.iscolor = ints3.i2;
		ip32.dim = ints3.i3;

#ifndef NO_SMALL_ICONS
		create_cache_icon( &ip16, 0 );
#endif
		create_cache_icon( &ip32, 0 );
		add_to_icon_cache( ip16.icon );
		add_to_icon_cache( ip32.icon );
		insert_into_hash_table ( owner.type, owner.creator, &ip16, &ip32 );
	}
	}
	CATCH( CFileException, e )
	{
	}
	END_CATCH

panic: // corrupted cache

	free(mask32);
	free(mask16);

	GlobalFree( h16 );
	GlobalFree( h32 );
}

void BrowserFolderReader::delete_cache()
{
	CString name;
	silent_close( &hash_file );
	hash_file_ok = 0;
	get_icons_file_name( name );
	silent_delete( name );
}

void BrowserFolderReader::clear_hash_table()
{
 bundleIcons_.clear();
	int i;
  struct type_creator_link_str *next;
  struct type_creator_link_str *x;

	if(hash_file_ok) {
		silent_close( &hash_file );
		hash_file_ok = 0;
	}

	for(i=0; i<HASH_SIZE; i++) {
		next = hash_table[i];
		while( next ) {
			x = next->next;
			free( next );
			next = x;
		}
	}
	memset( hash_table, 0, sizeof(type_creator_link_t *) * HASH_SIZE );
	icons_in_cache = 0;
	delete_mask_bitmaps();
}

unsigned long BrowserFolderReader::
hash_func (OSType type, OSType creator)
{
  unsigned long retval;

  retval = ((unsigned long) type * creator) % HASH_SIZE;
  return retval;
}

type_creator_link_t ** BrowserFolderReader::
hashpp (OSType type, OSType creator)
{
  type_creator_link_t **retval;

	/*
  for (retval = &hash_table[hash_func (type, creator)];
       *retval && ((*retval)->type != type || (*retval)->creator != creator);
       retval = &(*retval)->next)
    ;
	*/

	retval = &hash_table[(int)hash_func (type, creator)];

	while( *retval ) {
		if( (*retval)->type == type && (*retval)->creator == creator ) {
			break;
		}
	  retval = &(*retval)->next;
	}
  return( retval );
}

int BrowserFolderReader::insert_into_hash_table (
	OSType type, 
	OSType creator, 
	local_icn_sharp_t *ip16,
	local_icn_sharp_t *ip32
	)
{
	/*
	if( type == 'APPL' && creator == 'SITx' ) {
		int i;
		i = 0;
	}
	*/
  if (ip16->icon || ip32->icon) {
    type_creator_link_t **linkpp;
    
    linkpp = hashpp (type, creator);
    if (!*linkpp) {
      type_creator_link_t *newlink;
      newlink = (type_creator_link_t *)malloc (sizeof (*newlink));
      if (newlink) {
        newlink->type = type;
        newlink->creator = creator;
        newlink->icon16 = ip16->icon;
        newlink->icon32 = ip32->icon;
        newlink->next = 0;
        *linkpp = newlink;
				icons_in_cache++;
				return(1);
      }
    }
  }
	return(0);
}

icn_sharp_hand BrowserFolderReader::
map_type_and_creator (
	OSType type, 
	OSType creator,
	int prefer_small )
{
  const auto bundle=bundleIcons_.find((uint64_t(uint32_t(creator))<<32)|uint32_t(type));
  if(bundle!=bundleIcons_.end()){
    const auto& icon=*bundle->second;
    return prefer_small?(icon.small?icon.small:icon.large):(icon.large?icon.large:icon.small);
  }
  type_creator_link_t **linkpp;
  icn_sharp_hand retval = 0;

  linkpp = hashpp (type, creator);
	if( *linkpp ) {
		if( prefer_small ) {
			retval = (*linkpp)->icon16;
		}
		if( !retval ) 
			retval = (*linkpp)->icon32;
	}
  return retval;
}

void BrowserFolderReader::process_bundle(const CatDataRec* record)
{
    const auto& finder=record->u.f.filUsrWds;
    const auto creator=finder.fdCreator;
    const auto key=(uint64_t(creator)<<32)|uint32_t(finder.fdType);
    if(!artworkVolume_||!(finder.fdFlags&kHasBundle)||
       bundleIcons_.size()>=512||bundleIcons_.count(key))return;
    auto icons=MacIcons::ReadClassicBundle(*artworkVolume_,record->u.f.filFlNum,record->u.f.filRLgLen);
    for(auto& item:icons)
        if(bundleIcons_.size()<512)
            bundleIcons_.try_emplace((uint64_t(creator)<<32)|item.first,std::move(item.second));
}

void BrowserFolderReader::process_bundle2(CFile* file,unsigned long start,unsigned long creator,uint64_t forkLength)
{
    try{
        HfsPlus::Bytes bytes(16);
        const auto fileSize=file->GetLength();
        if(start>fileSize)return;
        const auto available=(std::min)(forkLength,fileSize-start);
        if(available<bytes.size())return;
        file->Seek(start,CFile::begin);
        if(file->Read(bytes.data(),UINT(bytes.size()))!=bytes.size())return;
        const auto size=MacIcons::ResourceForkLength(bytes);
        if(!size||size>available)return;
        bytes.resize(size);
        if(file->Read(bytes.data()+16,UINT(size-16))!=size-16)return;
        auto icons=MacIcons::DecodeBundle(bytes);
        for(auto& item:icons)
            if(bundleIcons_.size()<512)
                bundleIcons_.try_emplace((uint64_t(creator)<<32)|item.first,std::move(item.second));
    }catch(CFileException* error){error->Delete();}
}

/*
	8,6		== used to be
	10,6	== alike but without dragon
	12,8  == old fashioned (no mask)
	12,12 == about same
	8,12  == good, but NOT with white window background -- damn!
	0,12  == old fashioned, white background
*/

/*
unsigned char funfun( unsigned char a, unsigned char b, int func )
{
	unsigned char f;
	switch(func) {
		case  0: f = 0        ; 	break;
		case  1: f = (!a)&(!b);		break;
		case  2: f = (!a)&  b ;		break;
		case  3: f =  !a      ;		break;
		case  4: f =   a &(!b);		break;
		case  5: f =       !b ;		break;
		case  6: f =   a ^  b ;		break;
		case  7: f = (!a)|(!b);		break;
		case  8: f =   a &  b ;		break;
		case  9: f = !(a ^  b);		break;
		case 10: f =        b ;		break;
		case 11: f = (!a)|  b ;		break;
		case 12: f =   a      ;		break;
		case 13: f =   a |(!b);		break;
		case 14: f =   a |  b ;		break;
		case 15: f = 255      ;		break;
	}
	return(f);
}
*/

void adjust_bw_buffer( unsigned char *p, int count )
{
	int i, half = (count>>1);
	unsigned char *ANDbitmask, *XORbitmask, and;

	negate_buffer( p, count );

	ANDbitmask = p;
	XORbitmask = p + half;

	for(i=0; i<half; i++) {
		and = *ANDbitmask;
		*ANDbitmask++ &= *XORbitmask;
		*XORbitmask++ ^= and;
	}

	/*
	unsigned char a, b, a1, b1;
	int fa = ::GetProfileInt("Test","fa",0);
	int fb = ::GetProfileInt("Test","fb",0);

	for(i=0; i<half; i++) {
		a = *ANDbitmask;
		b = *XORbitmask;
		a1 = funfun( a,b,fa );
		b1 = funfun( a,b,fb );
		*ANDbitmask = a1;
		*XORbitmask = b1;
		ANDbitmask++;
		XORbitmask++;
	}
	*/
}

void BrowserFolderReader::create_cache_icon(
	local_icn_sharp_t *dat,
	int invert )
{
	unsigned char *p;
	int half;

	if(dat->h) {
		p = (unsigned char *)GlobalLock( dat->h );

		if(dat->iscolor) {
			dat->bytes = dat->small ? (COLORICONSIZE>>2) : COLORICONSIZE;
			dat->icon = map_colors( 
				(unsigned char *)p, 
				dat->small, 
				dat->bytes, 
				&dat->color_icon_mask );
		} else {
			dat->bytes = dat->small ? (BW_ICONSIZE>>2) : BW_ICONSIZE;
			half = dat->bytes >> 1;

			// dump_data( (unsigned char *)"c:\\koe2.txt", p, 1024 );

			// memset( &p[137], 0, 255 );

			if(invert) {
				adjust_bw_buffer( p, dat->bytes );
			}

			CHFVExplorerApp	*pApp = (CHFVExplorerApp *)AfxGetApp();
			dat->icon = CreateIcon( 
				pApp->m_hInstance, dat->dim, dat->dim, 1, 1, 
				(unsigned char *)p,					// AND
				(unsigned char *)p + half		// XOR
			);
		}
		GlobalUnlock( dat->h );
	}
}

void BrowserFolderReader::mac_load_icon2(
	int volinx, 
	CatDataRec *pCDR,
	Integer id, 
	int small,
	local_icn_sharp_t *dat )
{
	dat->h = 0;
	dat->icon = 0;
	dat->dim = small ? 16 : 32;
	dat->iscolor = 0;
	dat->small = small;
	dat->color_icon_mask = 0;

#ifdef NO_SMALL_ICONS
	if(small) return;
#endif

	if(options_.bitsPerPixel >= 8) {
		dat->h = mac_load_any_resource ( 
					volinx, pCDR,
					dat->small ? (unsigned long)'ics8' :	(unsigned long)'icl8',
					0, id );
	}
	if(dat->h) {
		dat->iscolor = 1;
	} else {
		dat->h = mac_load_any_resource( 
					volinx,
					pCDR,
					small ? (unsigned long)'ics#' : (unsigned long)'ICN#',
					0, id );
	}
	create_cache_icon( dat, 1 );
}

void BrowserFolderReader::mac_load_icon3(
	CFile *fp,
	unsigned long g_offset,
	Integer id, 
	int small,
	local_icn_sharp_t *dat )
{
	dat->h = 0;
	dat->icon = 0;
	dat->dim = small ? 16 : 32;
	dat->iscolor = 0;
	dat->small = small;
	dat->color_icon_mask = 0;

#ifdef NO_SMALL_ICONS
	if(small) return;
#endif

	if(options_.bitsPerPixel >= 8) {
		dat->h = mac_load_any_resource2 ( 
					fp, g_offset,
					dat->small ? (unsigned long)'ics8' :	(unsigned long)'icl8',
					0, id );
	}
	if(dat->h) {
		dat->iscolor = 1;
	} else {
		dat->h = mac_load_any_resource2 ( 
					fp, g_offset,
					dat->small ? (unsigned long)'ics#' : (unsigned long)'ICN#',
					0, id );
	}
	create_cache_icon( dat, 1 );
}

// do not hash, do not save to disk, DO add to memory cache
void BrowserFolderReader::load_custom_icons(
	int volinx, 
	CatDataRec *pCDR,
	HICON *phicon16, 
	HICON *phicon32 
)
{
	local_icn_sharp_t ic16, ic32;

	mac_load_icon2( volinx, pCDR, kCustomIconResource, 1, &ic16 );
	mac_load_icon2( volinx, pCDR, kCustomIconResource, 0, &ic32 );
	add_to_icon_cache( ic16.icon );
	add_to_icon_cache( ic32.icon );
	*phicon16 = ic16.icon;
	*phicon32 = ic32.icon;
	// MISSING_FREE - FIXED
	if(ic16.color_icon_mask) free( ic16.color_icon_mask );
	if(ic32.color_icon_mask) free( ic32.color_icon_mask );
	// if(ic16.icon) AfxMessageBox( "File has small 0xBFB9." );
	// if(ic32.icon) AfxMessageBox( "File has large 0xBFB9." );
}

void BrowserFolderReader::load_custom_icons(
	CFile *fp,
	unsigned long g_offset,
	HICON *phicon16, 
	HICON *phicon32 
)
{
	local_icn_sharp_t ic16, ic32;
	mac_load_icon3( fp, g_offset, kCustomIconResource, 1, &ic16 );
	mac_load_icon3( fp, g_offset, kCustomIconResource, 0, &ic32 );
	add_to_icon_cache( ic16.icon );
	add_to_icon_cache( ic32.icon );
	*phicon16 = ic16.icon;
	*phicon32 = ic32.icon;
	// MISSING_FREE - FIXED
	if(ic16.color_icon_mask) free( ic16.color_icon_mask );
	if(ic32.color_icon_mask) free( ic32.color_icon_mask );
	// if(ic16.icon) AfxMessageBox( "File has small 0xBFB9." );
	// if(ic32.icon) AfxMessageBox( "File has large 0xBFB9." );
}
