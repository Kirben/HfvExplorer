#include "EngineContext.h"
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

extern HFV_THREAD_LOCAL char *cpo_error;

int cpo_macb(hfsvol *, char *, char *);
int cpo_macb1(hfsvol *, char *, char *); /* BinHex 5.0 / MacBinary I */
int cpo_binh(hfsvol *, char *, char *);
int cpo_text(hfsvol *, char *, char *);
int cpo_raw(hfsvol *, char *, char *);

// LAURI
int cpo_raw_resource( hfsvol *, char *, char *);
void copyout_set_last_open_file( const char *path );
void copyout_get_last_open_file( char *path, int maxpath );

// Optional observer; byte counts are source bytes read, before conversion.
typedef void (*copyout_progress_fn)(const char*,unsigned long,unsigned long long,int,void*);
void copyout_set_progress(copyout_progress_fn,void*);
typedef int (*copyout_overwrite_fn)(const char*,void*);
void copyout_set_overwrite(copyout_overwrite_fn,void*);
int copyout_was_skipped(void);
/* Shared confirmation for converted imports and classic-HFS copies. */
int copyout_hfs_overwrite(const char *name, int exists);
int copyout_destination_opened(void);

#ifdef HFV_TESTING
/* Failure injection for host-output preservation tests: write, flush, publish. */
void copyout_test_fault(int stage);
#endif

int copyout_host_copy(const char *source,const char *destination);
