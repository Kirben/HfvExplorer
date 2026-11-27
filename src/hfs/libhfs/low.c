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

/* Modified for emx by Marcus Better, July 1997 */

# include <string.h>
# include <stdlib.h>
# include <errno.h>

#ifdef WIN32
#define NODEVLOCKS
# include <io.h>
# include <stdio.h>
#else
# include <unistd.h>
#endif

# include <fcntl.h>

# include "internal.h"
# include "../ClassicHfsRecordBytes.h"
# include "block.h"
# include "low.h"

/*
 * NAME:	low->lockvol()
 * DESCRIPTION:	prevent destructive simultaneous access
 */
int l_lockvol(hfsvol *vol)
{
# ifndef NODEVLOCKS

  struct flock lock;

  lock.l_type   = (vol->flags & HFS_READONLY) ? F_RDLCK : F_WRLCK;
  lock.l_start  = 0;
  lock.l_whence = SEEK_SET;
  lock.l_len    = 0;

  if (fcntl(vol->fd, F_SETLK, &lock) < 0)
    {
      ERROR(errno, "unable to obtain lock for device");
      return -1;
    }

# endif

  return 0;
}
