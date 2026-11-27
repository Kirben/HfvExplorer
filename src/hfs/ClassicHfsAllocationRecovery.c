// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "libhfs/internal.h"
#include "ClassicHfsTreeLookup.h"
#include "ClassicHfsVolumeIO.h"
#include "ClassicHfsGeometry.h"

/* Check every descriptor before touching the allocation bitmap. */
int ClassicHfsRecoveryExtentsValid(const ExtDataRec *extents, unsigned int blocks)
{
  unsigned int i;
  for (i = 0; i < 3; ++i)
    {
      unsigned int start = (*extents)[i].xdrStABN;
      unsigned int count = (*extents)[i].xdrNumABlks;

      if (count && (start >= blocks || count > blocks - start))
        return 0;
    }
  return 1;
}

static int markexts(hfsvol *vol, const ExtDataRec *extents)
{
  unsigned int i;
  if (!ClassicHfsRecoveryExtentsValid(extents, vol->mdb.drNmAlBlks))
    {
      ERROR(EIO, "extent outside volume allocation bitmap during HFS recovery");
      return -1;
    }

  for (i = 0; i < 3; ++i)
    {
      unsigned int start = (*extents)[i].xdrStABN;
      unsigned int count = (*extents)[i].xdrNumABlks;
      unsigned int index;
      for (index = start; index < start + count; ++index)
        ClassicHfsSetBit(vol->vbm, index);
    }
  return 0;
}

int ClassicHfsRecoveryLeafValid(const node *leaf, unsigned long previous)
{
  /* The first leaf must link back to zero; each later leaf must link back
     to its immediate predecessor. A repeated leaf cannot satisfy this. */
  return (unsigned char)leaf->nd.ndType == ndLeafNode &&
         leaf->nd.ndNHeight == 1 && leaf->nd.ndBLink == previous;
}

static int scavenge_tree(hfsvol *vol, btree *tree, int catalog)
{
  unsigned long previous = 0;
  node leaf;
  leaf.bt = tree;

  for (leaf.nnum = tree->hdr.bthFNode; leaf.nnum; leaf.nnum = leaf.nd.ndFLink)
    {
      unsigned int i, record_limit;
      if (leaf.nnum >= tree->hdr.bthNNodes || leaf.nnum / 8 >= tree->mapsz)
        {
          ERROR(EIO, "invalid B-tree leaf number during HFS recovery");
          return -1;
        }
      if (ClassicHfsReadTreeNode(&leaf) < 0)
        return -1;
      if (!ClassicHfsRecoveryLeafValid(&leaf, previous))
        {
          ERROR(EIO, "cyclic or broken B-tree leaf chain during HFS recovery");
          return -1;
        }
      record_limit = HFS_BLOCKSZ - 2 * (leaf.nd.ndNRecs + 1);
      for (i = 0; i < leaf.nd.ndNRecs; ++i)
        {
          unsigned int start = leaf.roff[i], end = leaf.roff[i + 1], key_size;
          unsigned char *record;
          if (start < 14 || start >= end || end > record_limit)
            goto invalid_record;
          key_size = HFS_RECKEYSKIP(leaf.data + start);
          if (key_size >= end - start)
            goto invalid_record;
          record = leaf.data + start + key_size;
          if (catalog)
            {
              if (record[0] == cdrFilRec)
                {
                  CatDataRec data;
                  if (end - start - key_size < HFS_CATDATALEN)
                    goto invalid_record;
                  ClassicHfsDecodeCatalogRecord(record, end - start - key_size, &data);
                  if (markexts(vol, &data.u.fil.filExtRec) < 0 ||
                      markexts(vol, &data.u.fil.filRExtRec) < 0)
                    return -1;
                }
            }
          else
            {
              ExtDataRec data;
              if (end - start - key_size < HFS_EXTDATALEN)
                goto invalid_record;
              ClassicHfsDecodeExtents(record, data);
              if (markexts(vol, &data) < 0)
                return -1;
            }
        }
      previous = leaf.nnum;
    }
  if (previous != tree->hdr.bthLNode)
    {
      ERROR(EIO, "incomplete B-tree leaf chain during HFS recovery");
      return -1;
    }
  return 0;

invalid_record:
  ERROR(EIO, "invalid B-tree leaf record during HFS recovery");
  return -1;
}

// Tree reads consult the volume bitmap. Make newly discovered system
// extents readable during the scan, and publish the bitmap only on success.
int ClassicHfsRecoverAllocationBitmap(hfsvol *vol)
{
    if (!vol->vbm)
        return 0;
    size_t bytes = ClassicHfsBitmapSectors(vol->mdb.drNmAlBlks) * sizeof(block);
    if (!bytes) {
        ERROR(EIO, "empty classic HFS allocation bitmap");
        return -1;
    }
    block *original = vol->vbm;
    block *working = (block *)malloc(bytes);
    if (!working) {
        ERROR(ENOMEM, "cannot stage classic HFS allocation recovery");
        return -1;
    }
    memcpy(working, original, bytes);
    vol->vbm = working;
    int valid = markexts(vol, &vol->mdb.drXTExtRec) == 0 &&
                markexts(vol, &vol->mdb.drCTExtRec) == 0 &&
                scavenge_tree(vol, &vol->ext, 0) == 0 && scavenge_tree(vol, &vol->cat, 1) == 0;
    vol->vbm = original;
    if (!valid) {
        free(working);
        return -1;
    }
    unsigned freeBlocks = 0;
    for (unsigned i = 0; i < vol->mdb.drNmAlBlks; ++i)
        if (!ClassicHfsBitIsSet(working, i))
            ++freeBlocks;
    memcpy(original, working, bytes);
    free(working);
    vol->flags |= HFS_UPDATE_VBM;
    if (vol->mdb.drFreeBks != freeBlocks) {
        vol->mdb.drFreeBks = freeBlocks;
        vol->flags |= HFS_UPDATE_MDB;
    }
    return 0;
}
