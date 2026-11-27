// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#undef ERROR
#include "hfs/libhfs/hfs.h"
#include "hfs/ClassicHfsRecordBytes.h"
#include "hfs/ClassicHfsCatalogKey.h"
#include "hfs/ClassicHfsExtents.h"
#include "hfs/ClassicHfsNode.h"
#include "hfs/ClassicHfsGeometry.h"
#include "hfs/ClassicHfsBitmap.h"
#include "hfs/ClassicHfsTreeHeader.h"
#include "hfs/ClassicHfsTime.h"
#include "hfs/copyin.h"
#include "hfs/copyout.h"
#include "hfs/crc.h"
/* This transfer-format harness operates only on its own new image, without
   a recovery transaction. The release build links the real sector hooks. */
int hfv_hfs_recover_image(int fd, int writable) { return 0; }
int hfv_hfs_before_sector_write(int fd, unsigned long long sector) { return 0; }
void hfv_hfs_after_sector_write(int fd) {}

static void must(int ok,const char* action){if(!ok){fprintf(stderr,"FAIL: BinHex 5: %s (%s; %s; %s)\n",action,hfs_error?hfs_error:"",cpi_error,cpo_error);exit(1);}}
static void save(const char* path,const unsigned char* bytes,unsigned size){HANDLE f=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);DWORD wrote=0;must(f!=INVALID_HANDLE_VALUE,"create host fixture");must(WriteFile(f,bytes,size,&wrote,NULL)&&wrote==size,"write host fixture");CloseHandle(f);}
static unsigned char* load(const char* path,unsigned* size){FILE*f=fopen(path,"rb");unsigned char*b;must(f!=NULL,"open export");fseek(f,0,SEEK_END);*size=(unsigned)ftell(f);rewind(f);b=malloc(*size);must(b!=NULL&&fread(b,1,*size,f)==*size,"read export");fclose(f);return b;}
static unsigned char value(unsigned i,unsigned fork){return (unsigned char)((i*37)^(i>>3)^(fork*91));}
static void verify(hfsvol*v,char*name,unsigned d,unsigned r){hfsfile*f=hfs_open(v,name);unsigned fork;unsigned char b[5000];must(f!=NULL,"open imported forks");for(fork=0;fork<2;fork++){unsigned n=fork?r:d,i;must(hfs_setfork(f,fork)==0,"choose fork");must(hfs_read(f,b,n)==n,"read imported fork");for(i=0;i<n;i++)must(b[i]==value(i,fork),"fork byte fidelity");must(hfs_read(f,b,1)==0,"fork EOF");}must(hfs_close(f)==0,"close imported forks");}
int main(int argc,char**argv){
 char image[1024],path[1024],badpath[1024],source[]="BH5:Source",target[]="BH5:Imported",label[]="BH5";
 HANDLE host;LARGE_INTEGER length;hfsvol*v;unsigned cases[][2]={{0,0},{1,0},{0,1},{127,129},{128,128},{129,127},{4097,3073}};unsigned k,fork;
 {
  unsigned char bytes[8] = {0x55, 0, 0, 0, 0, 0, 0, 0xaa};
  ClassicHfsWrite32(bytes + 1, UINT32_C(0x80fe01ff));
  ClassicHfsWrite16(bytes + 5, UINT16_C(0xfedc));
  must(memcmp(bytes, "\x55\x80\xfe\x01\xff\xfe\xdc\xaa", 8) == 0, "unaligned big-endian writes");
  must(ClassicHfsRead32(bytes + 1) == UINT32_C(0x80fe01ff) &&
       ClassicHfsRead16(bytes + 5) == UINT16_C(0xfedc), "high-bit big-endian reads");
 }
 {
  unsigned char fields[10] = {0};
  unsigned char *cursor = fields;
  char name[4];
  ClassicHfsAppendName(&cursor, "Mac", 4);
  ClassicHfsAppendName(&cursor, "X", 4);
  ClassicHfsAppend16(&cursor, 0xfedc);
  must(cursor == fields + sizeof(fields) && !memcmp(fields, "\x03Mac\x01X\0\0\xfe\xdc", 10),
       "fixed Pascal fields and padding");
  fields[0] = 0xff;
  cursor = fields;
  ClassicHfsTakeName(&cursor, name, sizeof(name));
  must(!name[0] && cursor == fields + 4, "invalid name consumes its field");
  ClassicHfsTakeName(&cursor, name, sizeof(name));
  must(!strcmp(name, "X") && ClassicHfsTake16(&cursor) == 0xfedc, "following fields stay aligned");
 }
 {
  time_t now = time(NULL);
  struct tm utc = *gmtime(&now);
  utc.tm_isdst = localtime(&now)->tm_isdst;
  uint32_t epoch = (uint32_t)(INT64_C(2082844800) + now - mktime(&utc));
  must(ClassicHfsFromUnixSeconds(0) == epoch && ClassicHfsToUnixSeconds(epoch) == 0,
       "classic local epoch compatibility");
  must(ClassicHfsFromUnixSeconds(ClassicHfsToUnixSeconds(UINT32_MAX)) == UINT32_MAX &&
       ClassicHfsFromUnixSeconds(ClassicHfsToUnixSeconds(0)) == 0, "classic date limits");
 }
 {
  unsigned char first[8], last[8];
  ClassicHfsWriteExtentKey(first, 0xff, 1, 0xffff);
  ClassicHfsWriteExtentKey(last, 0, UINT32_MAX, 0);
  must(!memcmp(first, "\x07\xff\0\0\0\x01\xff\xff", 8), "extent key disk layout");
  must(ClassicHfsCompareExtentKeys(first, last) < 0 && ClassicHfsCompareExtentKeys(last, first) > 0,
       "unsigned file IDs take precedence over fork and block");
  ClassicHfsWriteExtentKey(last, 0, 1, 0xffff);
  must(ClassicHfsCompareExtentKeys(last, first) < 0, "data fork precedes resource fork");
  ClassicHfsWriteExtentKey(last, 0xff, 1, 0x8000);
  must(ClassicHfsCompareExtentKeys(last, first) < 0 && ClassicHfsCompareExtentKeys(first, first) == 0,
       "extent block order and equality");
 }
 {
  unsigned char bytes[39];
  ClassicHfsCatalogKey decoded;
  const char full[] = "1234567890123456789012345678901";
  memset(bytes, 0xcc, sizeof(bytes));
  must(ClassicHfsWriteCatalogKey(bytes, 42, full) == 38 && bytes[38] == 0xcc &&
       ClassicHfsReadCatalogKey(bytes, 38, &decoded) && decoded.ckrParID == 42 &&
       !strcmp(decoded.ckrCName, full), "maximum catalog key round trip");
  must(!ClassicHfsReadCatalogKey(bytes, 37, &decoded), "truncated catalog key");
  must(ClassicHfsWriteCatalogKey(bytes, 42, "12345678901234567890123456789012") < 0 &&
       bytes[0] == 37, "long name rejected before encoding");
  must(ClassicHfsWriteCatalogKey(bytes, 1, "") == 8 && bytes[7] == 0 &&
       ClassicHfsReadCatalogKey(bytes, 8, &decoded) && !decoded.ckrCName[0], "thread key padding");
  unsigned char thread[] = {6, 0, 0, 0, 0, 1, 0};
  unsigned char leaf[] = {7, 0, 0, 0, 0, 1, 1, 0x80};
  unsigned char index[38] = {37, 0, 0, 0, 0, 1, 1, 0x8a};
  must(ClassicHfsCompareCatalogKeys(thread, leaf) < 0 &&
       ClassicHfsCompareCatalogKeys(leaf, index) == 0, "thread, leaf and padded index collation");
  ClassicHfsWrite32(index + 2, UINT32_MAX);
  must(ClassicHfsCompareCatalogKeys(index, leaf) > 0 && ClassicHfsCompareCatalogKeys(leaf, index) < 0,
       "catalog parent IDs compare unsigned");
  leaf[6] = 31;
  must(ClassicHfsCatalogNameLength(leaf) == 0, "name cannot extend beyond its key");
 }
 {
  ClassicHfsExtent runs[3] = {{0xfedc, 2}, {0, 0}, {0x8000, 0xffff}}, decoded[3];
  unsigned char bytes[21] = {7};
  bytes[20] = 0x55;
  must(ClassicHfsEncodeExtents(bytes + 8, runs) == 12 &&
       !memcmp(bytes + 8, "\xfe\xdc\0\2\0\0\0\0\x80\0\xff\xff", 12) && bytes[20] == 0x55,
       "three extent descriptors on disk");
  ClassicHfsDecodeExtents(bytes + 8, decoded);
  must(!memcmp(decoded, runs, sizeof(runs)), "extent descriptor round trip");
  must(ClassicHfsValidExtentRecord(bytes, 20) && !ClassicHfsValidExtentRecord(bytes, 19),
       "overflow extent record boundary");
  bytes[0] = 6;
  must(!ClassicHfsValidExtentRecord(bytes, 20), "invalid overflow extent key length");
 }
 {
  unsigned char bytes[ClassicHfsNodeSize];
  ClassicHfsNodeDescriptor header = {0xabcdef01, 3, -1, 1, 2, -2}, decoded;
  uint16_t offsets[ClassicHfsMaxRecords+1] = {14, 32, 506}, readback[ClassicHfsMaxRecords+1];
  memset(bytes, 0x55, sizeof(bytes));
  must(ClassicHfsEncodeNode(bytes, &header, offsets) && bytes[14] == 0x55 &&
       ClassicHfsRead16(bytes + 506) == 506 && ClassicHfsRead16(bytes + 510) == 14,
       "reverse node offsets leave payload intact");
  must(ClassicHfsDecodeNode(bytes, &decoded, readback) && decoded.ndType == -1 &&
       decoded.ndFLink == header.ndFLink && decoded.ndResv2 == -2 &&
       !memcmp(offsets, readback, 6), "classic node descriptor round trip");
  ClassicHfsWrite16(bytes + 508, 12);
  must(!ClassicHfsDecodeNode(bytes, &decoded, readback), "reject descending node offsets");
  offsets[2] = 508;
  must(!ClassicHfsEncodeNode(bytes, &header, offsets) && ClassicHfsRead16(bytes + 508) == 12,
       "reject overlap with offset table before modifying node");
 }
 {
  unsigned char bytes[512] = {0};
  uint16_t offsets[] = {14, 16, 18, 24}, count = 3;
  memcpy(bytes + 18, "abcdef", 6);
  offsets[2] = 17;
  must(!ClassicHfsCompactNode(bytes, offsets, &count) && count == 3 &&
       offsets[1] == 16 && !memcmp(bytes + 18, "abcdef", 6), "reject malformed compaction");
  offsets[2] = 18;
  must(ClassicHfsCompactNode(bytes, offsets, &count) && count == 1 && offsets[0] == 14 &&
       offsets[1] == 20 && !memcmp(bytes + 14, "abcdef", 6), "compact overlapping live record");
  bytes[14] = 0;
  must(ClassicHfsCompactNode(bytes, offsets, &count) && !count && offsets[0] == 14,
       "compact fully deleted node");
 }
 {
  unsigned char bytes[512] = {0}, record[494] = {1, 2};
  uint16_t offsets[ClassicHfsMaxRecords + 1] = {14}, count = 0;
  must(ClassicHfsInsertNodeRecord(bytes, offsets, &count, 0, record, 490),
       "insert large record in empty node");
  must(!ClassicHfsInsertNodeRecord(bytes, offsets, &count, 2, record, 2) &&
       !ClassicHfsInsertNodeRecord(bytes, offsets, &count, 0, record, 4) &&
       count == 1 && offsets[1] == 504 && bytes[14] == 1,
       "reject invalid slot and offset-table overlap before insertion");
  record[0] = 3;
  must(ClassicHfsInsertNodeRecord(bytes, offsets, &count, 0, record, 2) &&
       count == 2 && offsets[1] == 16 && offsets[2] == 506 &&
       bytes[14] == 3 && bytes[16] == 1, "insert before large record at exact capacity");
 }
 {
  unsigned char left[512] = {0}, right[512] = {0};
  uint16_t offsets[ClassicHfsMaxRecords + 1] = {14, 502};
  uint16_t sibling[] = {18, 20, 22}, count = 1;
  memcpy(right + 18, "abcd", 4);
  must(!ClassicHfsMergeNodeRecords(left, offsets, &count, right, sibling, 2) &&
       left[502] == 0 && count == 1 && offsets[2] == 0, "oversized merge preserves left node");
  offsets[1] = 500;
  must(ClassicHfsMergeNodeRecords(left, offsets, &count, right, sibling, 2) &&
       count == 3 && offsets[2] == 502 && offsets[3] == 504 &&
       !memcmp(left + 500, "abcd", 4) && !memcmp(right + 18, "abcd", 4),
       "exact capacity merge rebases sibling offsets and retains record order");
 }
 {
  unsigned char map[] = {0xff, 0x40};
  must(!ClassicHfsClaimTreeNode(map, 1, 10, 1) && map[1] == 0x40,
       "tree allocation stays within installed map");
  must(ClassicHfsClaimTreeNode(map, 2, 10, 1) == 8 && map[1] == 0xc0,
       "tree allocation reaches next map byte");
  must(!ClassicHfsReleaseTreeNode(map, 2, 10, 0, 0) &&
       ClassicHfsReleaseTreeNode(map, 2, 10, 0, 8) &&
       !ClassicHfsReleaseTreeNode(map, 2, 10, 1, 8) && map[1] == 0x40,
       "protect tree header and reject double release");
  map[0] = 0;
  must(!ClassicHfsClaimTreeNode(map, 2, 10, 2) && map[0] == 0,
       "reject missing tree header bit");
 }
 {
  unsigned char source[106], output[107];
  ClassicHfsTreeHeader header;
  size_t i;
  for (i = 0; i < sizeof(source); ++i) source[i] = (unsigned char)(0x80 + i);
  memset(output, 0x55, sizeof(output));
  must(!ClassicHfsDecodeTreeHeader(source, 105, &header) &&
       ClassicHfsDecodeTreeHeader(source, 106, &header) && header.bthDepth == 0x8081 &&
       header.bthFree == UINT32_C(0x9a9b9c9d), "tree header boundaries and field order");
  must(!ClassicHfsEncodeTreeHeader(output, 105, &header) && output[0] == 0x55 &&
       ClassicHfsEncodeTreeHeader(output, 106, &header) && !memcmp(output, source, 106) &&
       output[106] == 0x55, "preserve tree header reserved bytes and following record");
 }
 {
  unsigned char bytes[2] = {0};
  ClassicHfsSetBit(bytes, 0); ClassicHfsSetBit(bytes, 7); ClassicHfsSetBit(bytes, 8);
  ClassicHfsClearBit(bytes, 7);
  must(bytes[0] == 0x80 && bytes[1] == 0x80, "most significant allocation bit first");
  for (unsigned mask = 0; mask < 256; ++mask) {
   bytes[0] = (unsigned char)mask;
   for (unsigned hint = 0; hint <= 8; ++hint) for (unsigned request = 1; request <= 9; ++request) {
    unsigned best = 0, expected = 99, actual = 99;
    for (unsigned step = 0; step < 8; ++step) {
     unsigned start = (hint + step) % 8, run = 0;
     while (start + run < 8 && run < request && !(mask & (128u >> (start + run)))) ++run;
     if (run > best) { best = run; expected = start; }
    }
    must(ClassicHfsFindFreeRun(bytes, 8, hint, request, &actual) == best && actual == expected,
         "allocation hint, partial extent and wraparound");
   }
  }
 }
 {
  unsigned char map = 0xf0;
  uint16_t hint = 4, freeBlocks = 9, first = 99;
  must(!ClassicHfsAllocateBlocks(&map, 8, &hint, &freeBlocks, 4, &first) &&
       map == 0xf0 && hint == 4 && freeBlocks == 9 && first == 99,
       "impossible allocation count preserves bitmap, hint and extent");
  freeBlocks = 4;
  must(ClassicHfsAllocateBlocks(&map, 8, &hint, &freeBlocks, 6, &first) == 4 &&
       map == 0xff && hint == 0 && freeBlocks == 0 && first == 4,
       "partial allocation updates bitmap and wraps hint");
 }
 {
  unsigned char map[2] = {0x07, 0xe0};
  uint16_t freeBlocks = 10;
  must(!ClassicHfsReleaseBlocks(map, 16, 5, 7, &freeBlocks) &&
       map[0] == 7 && map[1] == 0xe0 && freeBlocks == 10, "partial double free is atomic");
  must(!ClassicHfsReleaseBlocks(map, 16, 15, 2, &freeBlocks) &&
       !ClassicHfsReleaseBlocks(map, 16, UINT32_MAX, 1, &freeBlocks), "invalid release range");
  freeBlocks = 15;
  must(!ClassicHfsReleaseBlocks(map, 16, 5, 6, &freeBlocks) &&
       map[0] == 7 && map[1] == 0xe0 && freeBlocks == 15, "invalid free count preserves bitmap");
  freeBlocks = 10;
  must(ClassicHfsReleaseBlocks(map, 16, 5, 6, &freeBlocks) &&
       map[0] == 0 && map[1] == 0 && freeBlocks == 16, "release across bitmap bytes");
  must(!ClassicHfsReleaseBlocks(map, 16, 5, 6, &freeBlocks) && freeBlocks == 16,
       "reject repeated release");
 }
 {
  ClassicHfsExtent runs[3] = {{18, 2}, {4, 3}, {0, 1}};
  unsigned location = 99;
  must(ClassicHfsLocateBlock(runs, 20, 7, 8, &location) == ClassicHfsMappedBlock && location == 19,
       "last physical allocation block");
  must(ClassicHfsLocateBlock(runs, 20, 7, 9, &location) == ClassicHfsMappedBlock && location == 4 &&
       ClassicHfsLocateBlock(runs, 20, 7, 12, &location) == ClassicHfsMappedBlock && location == 0,
       "fragment boundaries");
  must(ClassicHfsLocateBlock(runs, 20, 7, 13, &location) == ClassicHfsNextExtentRecord && location == 13,
       "next overflow key");
  runs[0].xdrNumABlks = 3;
  must(ClassicHfsLocateBlock(runs, 20, 7, 7, &location) == ClassicHfsInvalidExtent && location == 13,
       "reject physically truncated run before I/O");
  runs[0].xdrNumABlks = 0;
  must(ClassicHfsLocateBlock(runs, 20, 7, 7, &location) == ClassicHfsInvalidExtent,
       "missing mapping cannot advance overflow search");
 }
 {
  uint32_t sector = 77;
  must(ClassicHfsAllocationSector(16, 65535, 256, 16776978, 65534, 255, &sector) &&
       sector == 16776975, "last allocation sector on a large image");
  must(!ClassicHfsAllocationSector(16, 65535, 256, sector, 65534, 255, &sector) &&
       !ClassicHfsAllocationSector(16, 65535, 256, UINT32_MAX, 1, 256, &sector) &&
       !ClassicHfsAllocationSector(16, 65535, 0x100000, UINT32_MAX, 4096, 0, &sector) &&
       sector == 16776975, "reject partition overrun, invalid index and wrapped sector");
  must(ClassicHfsVolumeSectors(16, 65535, 256, &sector) && sector == 16776978 &&
       !ClassicHfsVolumeSectors(16, 65535, 0x100000, &sector), "checked classic volume length");
 }
 must(argc==2&&strlen(argv[1])<900,"test directory argument");must(CreateDirectoryA(argv[1],NULL),"exclusive test directory");
 sprintf(image,"%s\\binhex5.dsk",argv[1]);sprintf(path,"%s\\roundtrip.bin",argv[1]);sprintf(badpath,"%s\\bad.bin",argv[1]);
 host=CreateFileA(image,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL);must(host!=INVALID_HANDLE_VALUE,"create synthetic image");length.QuadPart=16*1024*1024;must(SetFilePointerEx(host,length,NULL,FILE_BEGIN)&&SetEndOfFile(host),"size image");CloseHandle(host);
 must(hfs_format(image,0,label)==0,"format image");v=hfs_mount(image,0,O_RDWR);must(v!=NULL,"mount image");
 must(ClassicHfsCompareNames("A", "a") == 0 && ClassicHfsCompareNames("a", "ab") < 0 &&
      ClassicHfsCompareNames("\x80", "\x8a") == 0 && ClassicHfsCompareNames("a", "\x80") < 0 &&
      ClassicHfsCompareNames(" ", "\xca") == 0, "classic MacRoman name ordering");
 must(hfs_create(v,"BH5:\x80","TEXT","TEST")==0,"create accented catalog name");
 must(hfs_create(v,"bh5:\x8a","TEXT","TEST")<0,"reject equivalent catalog name");
 for(k=0;k<sizeof(cases)/sizeof(cases[0]);k++){
  hfsfile*f;hfsdirent original,restored;unsigned char bytes[5000],*exported;unsigned size,i;unsigned d=cases[k][0],r=cases[k][1];
  if(k)must(hfs_delete(v,source)==0,"remove previous source");
  must(hfs_create(v,source,"APPL","TEST")==0,"create source");f=hfs_open(v,source);must(f!=NULL,"open source");
  for(fork=0;fork<2;fork++){unsigned n=fork?r:d;for(i=0;i<n;i++)bytes[i]=value(i,fork);must(hfs_setfork(f,fork)==0&&hfs_write(f,bytes,n)==n,"write source fork");}
  must(hfs_fstat(f,&original)==0,"source metadata");original.fdflags=HFS_FNDR_ISINVISIBLE|HFS_FNDR_HASBUNDLE|HFS_FNDR_COLOR;original.fdlocation.v=-11;original.fdlocation.h=37;original.crdate=1600000000;original.mddate=1600000010;must(hfs_fsetattr(f,&original)==0&&hfs_close(f)==0,"set source metadata");
  must(cpo_macb1(v,source,path)==0,"BinHex 5 export");exported=load(path,&size);
  must(size==128+((d+127)&~127u)+((r+127)&~127u),"MacBinary I aligned file length");
  must(exported[0]==0&&exported[74]==0&&exported[82]==0,"legacy header zero bytes");for(i=99;i<128;i++)must(exported[i]==0,"MacBinary I reserved tail zero");
  must(!memcmp(exported+65,"APPLTEST",8)&&exported[73]==(unsigned char)(original.fdflags>>8),"type creator high Finder flags");
  must((unsigned long)ClassicHfsRead32(exported+83)==d&&(unsigned long)ClassicHfsRead32(exported+87)==r,"unsigned fork lengths");
  must(cpi_macb(path,v,target)==0,"MacBinary I import");verify(v,target,d,r);must(hfs_stat(v,target,&restored)==0,"imported metadata");
  must(restored.fdflags==(original.fdflags&0xff00)&&restored.fdlocation.v==-11&&restored.fdlocation.h==37&&restored.crdate==original.crdate&&restored.mddate==original.mddate,"legacy metadata and low-flag limitation");
  // A final fork need not carry transport padding. Inter-fork padding remains.
  if(d||r){unsigned minimum=128+(r?((d+127)&~127u)+r:d);save(badpath,exported,minimum);must(cpi_macb(badpath,v,target)==0,"unpadded final fork import");verify(v,target,d,r);}
  // Structural errors must be rejected before overwriting an existing target.
  if(d||r){unsigned minimum=128+(r?((d+127)&~127u)+r:d);save(badpath,exported,minimum-1);must(cpi_macb(badpath,v,target)==-1&&strstr(cpi_error,"truncated"),"truncated source preflight");verify(v,target,d,r);}
  exported[110]=1;save(badpath,exported,size);must(cpi_macb(badpath,v,target)==-1,"invalid legacy reserved tail");verify(v,target,d,r);free(exported);
  must(cpo_macb(v,source,path)==0,"existing MacBinary II export");exported=load(path,&size);
  must(exported[122]==129&&exported[123]==129&&(unsigned short)ClassicHfsRead16(exported+124)==crc_macb(exported,124,0),"MacBinary II version and CRC unchanged");
  must(cpi_macb(path,v,target)==0,"MacBinary II import");verify(v,target,d,r);
  exported[65]^=1;save(badpath,exported,size);must(cpi_macb(badpath,v,target)==-1&&strstr(cpi_error,"CRC"),"corrupt II must not downgrade to I");verify(v,target,d,r);free(exported);
 }
 {
  unsigned char* b;unsigned size;hfsdirent e;unsigned i;
  must(cpo_macb1(v,source,path)==0,"prepare legacy header edge tests");b=load(path,&size);
  b[1]=63;memset(b+2,'A',63);save(badpath,b,size);must(cpi_macb(badpath,v,target)==0,"Pascal 63-byte name without terminator");verify(v,target,4097,3073);
  b[1]=0;save(badpath,b,size);must(cpi_macb(badpath,v,target)==-1,"empty header name rejected");
  b[1]=4;memcpy(b+2,"X:Y!",4);save(badpath,b,size);must(cpi_macb(badpath,v,target)==-1,"colon in name rejected");
  b[1]=4;memcpy(b+2,"Name",4);b[81]=1;save(badpath,b,size);must(cpi_macb(badpath,v,target)==0&&hfs_stat(v,target,&e)==0&&(e.flags&HFS_ISLOCKED),"protected flag restored");
  must(cpo_macb1(v,target,path)==0,"protected file export");free(b);b=load(path,&size);must(b[81]==1,"protected flag exported");free(b);
  // Versioned files may have a secondary header; its rounded size is honored.
  must(cpo_macb(v,source,path)==0,"secondary header fixture");b=load(path,&size);b=realloc(b,size+128);must(b!=NULL,"secondary header allocation");memmove(b+256,b+128,size-128);memset(b+128,0x5a,128);ClassicHfsWrite16(b+120,3);ClassicHfsWrite16(b+124,crc_macb(b,124,0));save(badpath,b,size+128);
  {char other[]="BH5:Secondary";must(cpi_macb(badpath,v,other)==0,"MacBinary II secondary header skip");verify(v,other,4097,3073);}
  // No silent signed-length overflow on either legacy fork.
  for(i=0;i<2;i++){memset(b,0,128);b[1]=1;b[2]='X';b[83+4*i]=0x80;save(badpath,b,128);{char absent[]="BH5:TooLarge";must(cpi_macb(badpath,v,absent)==-1&&strstr(cpi_error,"bad file length")&&hfs_stat(v,absent,&e)==-1,"legacy fork limit preflight");}}
  free(b);
 }
 must(hfs_umount(v)==0,"persist image");v=hfs_mount(image,0,O_RDONLY);must(v!=NULL,"reopen image");verify(v,target,4097,3073);verify(v,"bh5:\x8a",0,0);must(hfs_umount(v)==0,"close image");
 puts("PASS: BinHex 5/MacBinary I import/export, empty and aligned/unaligned forks, metadata/protected flag, Pascal names, unpadded final fork, preflight preservation, II CRC downgrade protection, secondary headers and Apple fork limits; synthetic files only");return 0;
}
