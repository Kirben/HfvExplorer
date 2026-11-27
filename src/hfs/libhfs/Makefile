###############################################################################
#
# hfsutils - tools for reading and writing Macintosh HFS volumes
# Copyright (C) 1997 Marcus Better
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#
###############################################################################

EXTRA_CFLAGS = -DNODEVLOCKS

ARFLAGS =	rcs

###############################################################################

NAME =		libhfs
TARGET =	$(NAME).a
OBJS =		../ClassicHfsTime.o ../ClassicHfsTransaction.o ../ClassicHfsAllocationRecovery.o ../ClassicHfsDirectoryIO.o ../ClassicHfsVolumeIO.o ../ClassicHfsForkStorage.o ../ClassicHfsMetadata.o ../ClassicHfsTreeMutation.o ../ClassicHfsNodeMutation.o  \
		../ClassicHfsMount.o

###############################################################################

include ../Rules.make

all :: $(TARGET)

clean ::
	-del *.o 2> NUL:
	-del $(TARGET) 2> NUL:
	-del *~ 2> NUL:

###############################################################################

$(TARGET): $(OBJS)
	$(AR) $(ARFLAGS) $@ $(OBJS)

### DEPENDENCIES FOLLOW #######################################################

../ClassicHfsVolumeIO.o: ../ClassicHfsVolumeIO.c internal.h hfs.h ../ClassicHfsTime.h ../ClassicHfsBytes.h block.h
../ClassicHfsMetadata.o: ../ClassicHfsMetadata.c internal.h hfs.h volume.h ../ClassicHfsEntry.h
../ClassicHfsTreeMutation.o: ../ClassicHfsTreeMutation.c internal.h hfs.h ../ClassicHfsTime.h ../ClassicHfsBytes.h block.h file.h ../ClassicHfsTreeLookup.h ../ClassicHfsTreeNodes.h
../ClassicHfsTime.o: ../ClassicHfsTime.c ../ClassicHfsTime.h
../ClassicHfsForkStorage.o: ../ClassicHfsForkStorage.c internal.h hfs.h ../ClassicHfsTime.h ../ClassicHfsBytes.h block.h file.h ../ClassicHfsTreeLookup.h \
 volume.h
../ClassicHfsMount.o: ../ClassicHfsMount.c internal.h hfs.h ../ClassicHfsTime.h ../ClassicHfsBytes.h block.h file.h ../ClassicHfsTreeLookup.h \
 ../ClassicHfsTreeNodes.h volume.h
../ClassicHfsNodeMutation.o: ../ClassicHfsNodeMutation.c internal.h hfs.h ../ClassicHfsTime.h ../ClassicHfsBytes.h ../ClassicHfsTreeLookup.h ../ClassicHfsTreeNodes.h
