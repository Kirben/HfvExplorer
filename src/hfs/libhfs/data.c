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
 * NAME:	data->fetchb()
 * DESCRIPTION:	incrementally retrieve a byte of data
 */
void d_fetchb(unsigned char **ptr, char *dest)
{
  *dest = (char)**ptr;
  *ptr += 1;
}

/*
 * NAME:	data->fetchw()
 * DESCRIPTION:	incrementally retrieve a word of data
 */
void d_fetchw(unsigned char **ptr, short *dest)
{
  *dest = ClassicHfsRead16(*ptr);
  *ptr += 2;
}

/*
 * NAME:	data->fetchl()
 * DESCRIPTION:	incrementally retrieve a long word of data
 */
void d_fetchl(unsigned char **ptr, long *dest)
{
  *dest = ClassicHfsRead32(*ptr);
  *ptr += 4;
}

/*
 * NAME:	data->fetchs()
 * DESCRIPTION:	incrementally retrieve a string
 */
void d_fetchs(unsigned char **ptr, char *dest, int size)
{
  int len;
  char blen;

  d_fetchb(ptr, &blen);
  len = blen;

  if (len > 0 && len < size)
    memcpy(dest, *ptr, len);
  else
    len = 0;

  dest[len] = 0;

  *ptr += size - 1;
}

/*
 * NAME:	data->storeb()
 * DESCRIPTION:	incrementally store a byte of data
 */
void d_storeb(unsigned char **ptr, char data)
{
  **ptr = (unsigned char)data;
  *ptr += 1;
}

/*
 * NAME:	data->storew()
 * DESCRIPTION:	incrementally store a word of data
 */
void d_storew(unsigned char **ptr, short data)
{
  ClassicHfsWrite16(*ptr, data);
  *ptr += 2;
}

/*
 * NAME:	data->storel()
 * DESCRIPTION:	incrementally store a long word of data
 */
void d_storel(unsigned char **ptr, long data)
{
  ClassicHfsWrite32(*ptr, data);
  *ptr += 4;
}

/*
 * NAME:	data->stores()
 * DESCRIPTION:	incrementally store a string
 */
void d_stores(unsigned char **ptr, char *src, int size)
{
  int len;

  len = strlen(src);
  if (len > --size)
    len = 0;

  d_storeb(ptr, (unsigned char) len);

  memcpy(*ptr, src, len);
  memset(*ptr + len, 0, size - len);

  *ptr += size;
}

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
