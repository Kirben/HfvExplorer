// SPDX-License-Identifier: GPL-2.0-or-later
// Produce real classic HFS overflow records with the independent hfsutils writer.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#undef ERROR
#include "hfs/libhfs/internal.h"
#include "hfs/libhfs/block.h"
#include "hfs/libhfs/file.h"
#include "hfs/libhfs/record.h"
#include "hfs/libhfs/btree.h"
#include "hfs/libhfs/volume.h"
#include "FragmentedTestData.h"

static void must(int ok,const char* action) {
 if(!ok) { fprintf(stderr,"FAIL: fragmented fixture: %s (%s)\n",action,hfs_error?hfs_error:""); exit(1); }
}
static void FragmentOverflowTree(const char* source,const char* directory) {
 char image[1100],oracle[1100]; hfsvol* v; ExtDescriptor pieces[10]; ExtDataRec old;
 unsigned i,j,k,blocks,length; unsigned char* bytes; FILE* out;
 sprintf(image,"%s\\fragmented-tree.dsk",directory); sprintf(oracle,"%s\\extents.bin",directory);
 must(CopyFileA(source,image,TRUE),"copy self-fragmented tree fixture");
 v=hfs_mount(image,0,O_RDWR); must(v!=NULL,"mount self-fragmented tree fixture");
 length=v->mdb.drXTFlSize; blocks=length/v->mdb.drAlBlkSiz; must(blocks>=10,"extents tree large enough for fragmentation");
 memcpy(old,v->ext.f.cat.u.fil.filExtRec,sizeof(old));
 must(old[0].xdrNumABlks+old[1].xdrNumABlks+old[2].xdrNumABlks==blocks,"original overflow tree inline");
 for(i=0;i<10;++i) {
  v->mdb.drAllocPtr=(unsigned short)(20000+i*100); pieces[i].xdrNumABlks=(unsigned short)(i==9?blocks-9:1);
  must(v_allocblocks(v,&pieces[i])==0 && pieces[i].xdrNumABlks==(i==9?blocks-9:1),"allocate overflow-tree extent");
 }
 for(i=3;i<10;i+=3) {
  ExtDataRec data={{0}}; unsigned char rec[HFS_EXTRECMAXLEN]; int reclen;
  for(j=0;j<3 && i+j<10;++j) data[j]=pieces[i+j];
  reclen = ClassicHfsWriteExtentKey(rec,0,3,i); reclen += ClassicHfsEncodeExtents(rec + reclen, data);
  must(bt_insert(&v->ext,rec,reclen)==0,"insert self-mapping overflow record");
 }
 must(v_flush(v,0)==0 && v->mdb.drXTFlSize==length,"flush self-records without growing allocation");
 bytes=malloc(length); must(bytes!=NULL,"overflow tree snapshot allocation");
 for(i=0;i<length/512;++i) must(f_doblock(&v->ext.f,i,(block*)(bytes+i*512),b_readab)==0,"snapshot overflow tree");
 k=0;
 for(i=0;i<10;++i) for(j=0;j<pieces[i].xdrNumABlks*v->lpa;++j,++k)
  must(b_writeab(v,pieces[i].xdrStABN+j/v->lpa,j%v->lpa,(block*)(bytes+k*512))==0,"relocate overflow tree");
 memcpy(v->ext.f.cat.u.fil.filExtRec,pieces,sizeof(ExtDataRec)); memcpy(v->ext.f.ext,pieces,sizeof(ExtDataRec)); v->ext.f.fabn=0;
 memcpy(v->mdb.drXTExtRec,pieces,sizeof(ExtDataRec)); v->flags|=HFS_UPDATE_MDB;
 for(i=0;i<3;++i) if(old[i].xdrNumABlks) must(v_freeblocks(v,&old[i])==0,"release relocated extent");
 out=fopen(oracle,"wb"); must(out!=NULL,"create extents oracle");
 must(fwrite(bytes,length,1,out)==1,"save pre-relocation extents oracle"); fclose(out);
 free(bytes); must(hfs_umount(v)==0,"persist self-fragmented overflow tree");
 v=hfs_mount(image,0,O_RDONLY); must(v!=NULL,"core reopens self-fragmented tree");
 out=fopen(oracle,"rb"); must(out!=NULL,"reopen self-tree oracle");
 for(i=0;i<length/512;++i) {
  block actual,expected;
  must(fread(expected,512,1,out)==1,"read self-tree oracle block");
  { int result=f_doblock(&v->ext.f,i,&actual,b_readab);
    if(result || memcmp(actual,expected,512)) {
      fprintf(stderr,"Self-tree block %u, result %d\n",i,result);
      must(0,"core self-fragmented tree byte fidelity");
    }
  }
 }
 fclose(out);
 { hfsfile* file=hfs_open(v,"Fragments:Fragment00"); unsigned char data[113];
   must(file!=NULL && hfs_setfork(file,1)==0,"core locates resource via self-fragmented tree");
   must(hfs_lseek(file,9*v->mdb.drAlBlkSiz-19,SEEK_SET)>=0 && hfs_read(file,data,113)==113,"core reads late resource via self-tree");
   for(i=0;i<113;++i) must(data[i]==LargeImagePattern(9*v->mdb.drAlBlkSiz-19+i,118),"core self-tree payload fidelity");
   must(hfs_close(file)==0,"close core self-tree payload"); }
 must(hfs_umount(v)==0,"close core self-tree fixture");
}
void CreateFragmentedFixture(const char* directory) {
 FragmentedManifest m={0}; char path[1100],name[64],type[]="TEXT",creator[]="ttxt",label[]="Fragments";
 hfsvol* v; hfsfile* f; HANDLE h; LARGE_INTEGER size; unsigned i,j,k,blocks;
 unsigned char* payload; unsigned char* catalog; ExtDescriptor pieces[10]; ExtDataRec old;
 FILE* out;
 sprintf(m.image,"%s\\fragmented.dsk",directory); sprintf(m.catalog,"%s\\catalog.bin",directory);
 h=CreateFileA(m.image,GENERIC_WRITE,0,NULL,CREATE_NEW,0,NULL); must(h!=INVALID_HANDLE_VALUE,"create image");
 size.QuadPart=16*1024*1024; must(SetFilePointerEx(h,size,NULL,FILE_BEGIN)&&SetEndOfFile(h),"size image"); CloseHandle(h);
 must(hfs_format(m.image,0,label)==0,"format"); v=hfs_mount(m.image,0,O_RDWR); must(v!=NULL,"mount");
 m.forkSize=v->mdb.drAlBlkSiz*10-37; payload=malloc(v->mdb.drAlBlkSiz); must(payload!=NULL,"payload allocation");
 for(i=0;i<16;++i) {
  sprintf(name,"Fragments:Fragment%02u",i); must(hfs_create(v,name,type,creator)==0,"create fragmented file");
  f=hfs_open(v,name); must(f!=NULL,"open fragmented file"); f->clump=v->mdb.drAlBlkSiz;
  for(j=0;j<2;++j) {
   must(hfs_setfork(f,j)==0,"select fork");
   for(k=0;k<10;++k) {
    unsigned n,length=k==9?v->mdb.drAlBlkSiz-37:v->mdb.drAlBlkSiz;
    // Leave a physical gap after every allocation so none can coalesce.
    v->mdb.drAllocPtr=(unsigned short)(1000+i*500+j*200+k*13); v->flags|=HFS_UPDATE_MDB;
    for(n=0;n<length;++n) payload[n]=LargeImagePattern(k*v->mdb.drAlBlkSiz+n,17+j*101);
    must(hfs_write(f,payload,length)==length,"write fragmented fork");
   }
  }
  must(hfs_close(f)==0,"close fragmented file");
 }
 free(payload);
 // Grow an actual, searchable multi-level catalog before relocating it.
 for(i=0;i<100;++i) { sprintf(name,"Fragments:CatalogEntry%03u",i); must(hfs_create(v,name,type,creator)==0,"grow catalog"); }
 must(v_flush(v,0)==0,"flush before catalog relocation");
 must(v->ext.hdr.bthDepth>=2,"overflow fixture exercises index descent");
 m.catalogSize=v->mdb.drCTFlSize; blocks=m.catalogSize/v->mdb.drAlBlkSiz;
 must(blocks>=10,"catalog large enough for ten extents");
 must(v->cat.f.cat.u.fil.filExtRec[0].xdrNumABlks+v->cat.f.cat.u.fil.filExtRec[1].xdrNumABlks+
      v->cat.f.cat.u.fil.filExtRec[2].xdrNumABlks==blocks,"original catalog fits inline extents");
 catalog=malloc(m.catalogSize); must(catalog!=NULL,"catalog allocation");
 for(i=0;i<m.catalogSize/512;++i) must(f_doblock(&v->cat.f,i,(block*)(catalog+i*512),b_readab)==0,"snapshot catalog");
 memcpy(old,v->cat.f.cat.u.fil.filExtRec,sizeof(old));
 for(i=0;i<10;++i) {
  v->mdb.drAllocPtr=(unsigned short)(16000+i*100); pieces[i].xdrNumABlks=(unsigned short)(i==9?blocks-9:1);
  must(v_allocblocks(v,&pieces[i])==0 && pieces[i].xdrNumABlks==(i==9?blocks-9:1),"allocate catalog extent");
 }
 for(i=3;i<10;i+=3) {
  ExtDataRec data={{0}}; unsigned char rec[HFS_EXTRECMAXLEN]; int length;
  for(j=0;j<3 && i+j<10;++j) data[j]=pieces[i+j];
  length = ClassicHfsWriteExtentKey(rec,0,4,i); length += ClassicHfsEncodeExtents(rec + length, data);
  must(bt_insert(&v->ext,rec,length)==0,"insert catalog overflow record");
 }
 k=0;
 for(i=0;i<10;++i) for(j=0;j<pieces[i].xdrNumABlks*v->lpa;++j,++k)
  must(b_writeab(v,pieces[i].xdrStABN+j/v->lpa,j%v->lpa,(block*)(catalog+k*512))==0,"relocate catalog contents");
 memcpy(v->cat.f.cat.u.fil.filExtRec,pieces,sizeof(ExtDataRec));
 memcpy(v->cat.f.ext,pieces,sizeof(ExtDataRec)); v->cat.f.fabn=0;
 memcpy(v->mdb.drCTExtRec,pieces,sizeof(ExtDataRec)); v->flags|=HFS_UPDATE_MDB;
 for(i=0;i<3;++i) if(old[i].xdrNumABlks) must(v_freeblocks(v,&old[i])==0,"release relocated extent");
 free(catalog); must(hfs_umount(v)==0,"persist fragmented catalog");
 v=hfs_mount(m.image,0,O_RDONLY); must(v!=NULL,"independent reopen fragmented catalog");
 f=hfs_open(v,"Fragments:Fragment00"); must(f!=NULL,"find file through fragmented catalog");
 m.fileID=f->cat.u.fil.filFlNum;
 for(i=0;i<3;++i) {
  m.data[i].start=f->cat.u.fil.filExtRec[i].xdrStABN; m.data[i].count=f->cat.u.fil.filExtRec[i].xdrNumABlks;
  m.resource[i].start=f->cat.u.fil.filRExtRec[i].xdrStABN; m.resource[i].count=f->cat.u.fil.filRExtRec[i].xdrNumABlks;
 }
 payload=malloc(m.forkSize); must(payload!=NULL,"verify allocation");
 for(j=0;j<2;++j) {
  must(hfs_setfork(f,j)==0 && hfs_read(f,payload,m.forkSize)==m.forkSize,"independent fragmented fork read");
  for(i=0;i<m.forkSize;++i) must(payload[i]==LargeImagePattern(i,17+j*101),"independent fork byte fidelity");
 }
 free(payload); must(hfs_close(f)==0,"close verification fork");
 out=fopen(m.catalog,"wb"); must(out!=NULL,"create catalog oracle");
 for(i=0;i<m.catalogSize/512;++i) { block bytes; must(f_doblock(&v->cat.f,i,&bytes,b_readab)==0,"read relocated catalog"); must(fwrite(bytes,512,1,out)==1,"save catalog oracle"); }
 fclose(out); must(hfs_umount(v)==0,"close fixture");
 FragmentOverflowTree(m.image,directory);
 sprintf(path,"%s\\fragmented.bin",directory); out=fopen(path,"wb"); must(out!=NULL,"create manifest");
 must(fwrite(&m,sizeof(m),1,out)==1,"write manifest"); fclose(out);
 puts("PASS: independent fixture: ten extents per fork and catalog, multiple overflow records, indexed overflow tree, reopen verification");
}
