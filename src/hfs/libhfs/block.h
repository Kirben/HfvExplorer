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

int ClassicHfsLoadVolumeHeader(hfsvol *);
int ClassicHfsStoreVolumeHeader(hfsvol *);
int ClassicHfsEnableVolumeCache(hfsvol *);
int ClassicHfsFlushVolumeCache(hfsvol *);
int ClassicHfsCloseVolumeCache(hfsvol *);

int ClassicHfsReadSector(hfsvol *, unsigned long, block *);
int ClassicHfsWriteSector(hfsvol *, unsigned long, block *);

int ClassicHfsTransferAllocationSector(hfsvol *, unsigned, unsigned, block *, int writing);

int ClassicHfsCaptureBeforeWrite(hfsvol *, unsigned long);
int ClassicHfsLoadRawSector(void *, unsigned long, block *);
int ClassicHfsStoreRawSector(void *, unsigned long, block *);
int ClassicHfsBeginTransaction(hfsvol *);
int ClassicHfsEndTransaction(hfsvol *, int);

int hfs_flush_raw(hfsvol *);
void b_quarantine_volume(hfsvol *, const char *);
