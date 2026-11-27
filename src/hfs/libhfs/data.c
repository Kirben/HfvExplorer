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

# include <string.h>
# include <time.h>

# include "internal.h"
# include "data.h"
# include "btree.h"

# define MUTDIFF  2082844800L

static
HFV_THREAD_LOCAL unsigned long tzdiff = -1;

/*
 * NAME:	calctzdiff()
 * DESCRIPTION:	calculate the timezone difference between local time and UTC
 */
static
void calctzdiff(void)
{
  time_t t;
  int isdst;
  struct tm tm, *tmp;

  time(&t);
  isdst = localtime(&t)->tm_isdst;

  tmp = gmtime(&t);
  if (tmp)
    {
      tm = *tmp;
      tm.tm_isdst = isdst;

      tzdiff = t - mktime(&tm);
    }
  else
    tzdiff = 0;
}

/*
 * NAME:	data->tomtime()
 * DESCRIPTION:	convert UNIX time to Macintosh time
 */
unsigned long d_tomtime(unsigned long secs)
{
  time_t utime = secs;

  if (tzdiff == -1)
    calctzdiff();

  return utime + tzdiff + MUTDIFF;
}

/*
 * NAME:	data->toutime()
 * DESCRIPTION:	convert Macintosh time to UNIX time
 */
unsigned long d_toutime(unsigned long secs)
{
  time_t utime = secs;

  if (tzdiff == -1)
    calctzdiff();

  return utime - MUTDIFF - tzdiff;
}
