/*
 * hfsutils - tools for reading and writing Macintosh HFS volumes
 * Copyright (C) 1996, 1997 Robert Leslie
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

int ClassicHfsFindCatalogEntry(hfsvol *, long, char *, CatDataRec *, char *, node *);

int ClassicHfsFindCatalogThread(hfsvol *, long, CatDataRec *, node *, int);

# define v_getdthread(vol, id, thread, np)  \
    ClassicHfsFindCatalogThread(vol, id, thread, np, cdrThdRec)
# define v_getfthread(vol, id, thread, np)  \
    ClassicHfsFindCatalogThread(vol, id, thread, np, cdrFThdRec)

int ClassicHfsStoreCatalogEntry(CatDataRec *, node *);

int ClassicHfsResolvePath(hfsvol *, char *, CatDataRec *, long *, char *, node *);

void v_destruct(hfsvol *);
int v_getvol(hfsvol **);
int v_flush(hfsvol *, int);

int v_adjvalence(hfsvol *, long, int, int);
int v_newfolder(hfsvol *, long, char *);

int v_recovery_extents_valid(const ExtDataRec *, unsigned int blocks);
int v_recovery_leaf_valid(const node *, unsigned long previous);
int v_scavenge(hfsvol *);
