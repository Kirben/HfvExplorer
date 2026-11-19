// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "Mactypes.h"
#include "ClassicCatalog.h"
#include "FragmentedTestData.h"
#include <stdio.h>
#include <vector>
#include <algorithm>

static void expect(bool ok,const char* message) { if(!ok) throw message; }
static unsigned word(const BYTE* p) {return (unsigned(p[0])<<8)|p[1];}
static unsigned long number(const BYTE* p) {return (unsigned long(word(p))<<16)|word(p+2);}
static CatDataRec entryFor(const FragmentedManifest& m) {
 CatDataRec e={}; e.cdrType=cdrFilRec; e.u.f.filFlNum=m.fileID;
 e.u.f.filLgLen=e.u.f.filRLgLen=m.forkSize;
 for(unsigned i=0;i<3;++i) {
  e.u.f.filExtRec[i].xdrStABN=m.data[i].start; e.u.f.filExtRec[i].xdrNumABlks=m.data[i].count;
  e.u.f.filRExtRec[i].xdrStABN=m.resource[i].start; e.u.f.filRExtRec[i].xdrNumABlks=m.resource[i].count;
 }
 return e;
}
static ULONGLONG treePosition(const MDB& m,unsigned logical) {
 for(unsigned i=0;i<3;++i) {
  unsigned bytes=m.drXTExtRec[i].xdrNumABlks*m.drAlBlkSiz;
  if(logical<bytes) return ULONGLONG(m.drAlBlSt)*512+ULONGLONG(m.drXTExtRec[i].xdrStABN)*m.drAlBlkSiz+logical;
  logical-=bytes;
 }
 throw "negative fixture tree does not fit inline extents";
}
void RunFragmentedGuiTests(const char* largeManifest,FILE* report) {
 CString directory=largeManifest; directory=directory.Left(directory.ReverseFind('\\')+1);
 FragmentedManifest m={}; FILE* in=fopen(directory+"fragmented.bin","rb"); expect(in!=NULL,"open fragmented manifest");
 size_t n=fread(&m,sizeof(m),1,in); fclose(in); expect(n==1,"read fragmented manifest");
 CHFVVolume volume; volume.m_file_name=m.image; expect(volume.open()!=0,"GUI opens fragmented catalog volume");
 expect(volume.m_extents_file.m_header_rec.bthDepth>=2,"test indexed overflow tree");
 unsigned allocation=volume.m_MDB.drAlBlkSiz;
 CatDataRec entry=entryFor(m),found={};
 std::vector<BYTE> actual(m.catalogSize),oracle(m.catalogSize);
 in=fopen(m.catalog,"rb"); expect(in!=NULL,"open independent catalog oracle");
 n=fread(oracle.data(),1,oracle.size(),in); fclose(in); expect(n==oracle.size(),"read catalog oracle");
 volume.m_catalog_file.Seek(0,CFile::begin);
 expect(volume.m_catalog_file.Read(actual.data(),(UINT)actual.size())==actual.size() && actual==oracle,"fragmented catalog byte fidelity through all overflow records");
 unsigned files=0; bool located=false;
 std::vector<CatDataRec> fragmented;
 for(unsigned offset=0;offset+512<=actual.size();offset+=512) {
  const BYTE* node=actual.data()+offset;
  if(node[8]!=ndLeafNode) continue;
  unsigned records=word(node+10); expect(records<64,"fixture catalog record count");
  for(unsigned r=0;r<records;++r) {
   unsigned start=word(node+512-2*(r+1)); expect(start>=14 && start<490,"catalog record offset");
   unsigned skip=(node[start]+2)&~1;
   if(node[start+skip]!=cdrFilRec) continue;
   ++files;
   CatDataRec c={}; unsigned end=word(node+512-2*(r+2));
   expect(end>=start+skip+102 && end<=512,"catalog file record bounds");
   memcpy(&c,node+start+skip,min(sizeof(c),size_t(end-start-skip))); macpc_catdatarec(&c);
   if(c.u.f.filLgLen==m.forkSize) fragmented.push_back(c);
   if(c.u.f.filFlNum==m.fileID) { found=c; located=true; }
  }
 }
 expect(files==116 && located,"all files enumerated from fragmented catalog leaves");
 expect(fragmented.size()==16,"all fragmented catalog entries present");
 for(size_t index=0;index<fragmented.size();++index) for(unsigned fork=0;fork<2;++fork) {
  CHFVFile file; expect(file.open_by_CDR(&volume,&fragmented[index],fork?CHFVFile::OpenResourceFork:CHFVFile::OpenDataFork)!=0,"open fork across indexed overflow leaves");
  std::vector<BYTE> bytes(m.forkSize);
  expect(file.Read(bytes.data(),m.forkSize)==m.forkSize,"read all fragmented files across index branches");
  for(unsigned i=0;i<m.forkSize;++i) expect(bytes[i]==LargeImagePattern(i,17+fork*101),"all fragmented files byte fidelity");
 }
 expect(found.u.f.filLgLen==entry.u.f.filLgLen,"catalog lookup returns expected file metadata");
 for(unsigned fork=0;fork<2;++fork) {
  CHFVFile file; expect(file.open_by_CDR(&volume,&found,fork?CHFVFile::OpenResourceFork:CHFVFile::OpenDataFork)!=0,"open fragmented fork from real catalog");
  std::vector<BYTE> bytes(m.forkSize);
  expect(file.Read(bytes.data(),(UINT)bytes.size())==bytes.size(),"read ten-extent fork");
  for(unsigned i=0;i<bytes.size();++i) expect(bytes[i]==LargeImagePattern(i,17+fork*101),"fragmented fork byte fidelity");
  // Backward/random reads after the complete map is cached, including each
  // inline/overflow and overflow/overflow transition, plus unaligned chunks.
  for(int block=9;block>=0;--block) {
   unsigned start=block*allocation- (block?19:0),length=min(113u,m.forkSize-start);
   file.Seek(start,CFile::begin); expect(file.Read(bytes.data(),length)==length,"seek backward across overflow extents");
   for(unsigned i=0;i<length;++i) expect(bytes[i]==LargeImagePattern(start+i,17+fork*101),"random fragmented read fidelity");
  }
  // A fresh file must also resolve a direct seek into the last overflow record.
  CHFVFile fresh; expect(fresh.open_by_CDR(&volume,&found,fork?CHFVFile::OpenResourceFork:CHFVFile::OpenDataFork)!=0,"reopen fragmented fork");
  fresh.Seek(m.forkSize-23,CFile::begin); expect(fresh.Read(bytes.data(),113)==23,"direct late seek and EOF clipping");
  for(unsigned i=0;i<23;++i) expect(bytes[i]==LargeImagePattern(m.forkSize-23+i,17+fork*101),"direct late seek fidelity");
 }
 {
  auto shared=MacBrowser::OpenClassic(m.image,":",false);
  const auto records=MacBrowser::ReadClassicCatalog(*shared,nullptr);
  expect(records.size()==116,"shared classic reader enumerates fragmented catalog");
  const auto start=m.forkSize-23;
  BYTE tail[23]{};
  MacBrowser::ReadClassicArtwork(*shared,m.fileID,start,tail,sizeof(tail));
  for(unsigned i=0;i<sizeof(tail);++i)
   expect(tail[i]==LargeImagePattern(start+i,118),"shared resource reader uses overflow extents");
 }
 MDB mdb=volume.m_MDB; BTHdrRec tree=volume.m_extents_file.m_header_rec;
 // Missing records must produce a bounded short read, never guessed sectors.
 entry.u.f.filFlNum=0xfffffff0;
 CHFVFile missing; expect(missing.open_by_CDR(&volume,&entry,CHFVFile::OpenDataFork)!=0,"open missing-record fork");
 std::vector<BYTE> bytes(m.forkSize);
 expect(missing.Read(bytes.data(),m.forkSize)==3*allocation,"missing overflow record stops at mapped bytes");
 volume.close();
 {
  CHFVVolume self; self.m_file_name=directory+"fragmented-tree.dsk";
  expect(self.open()!=0,"bootstrap self-fragmented overflow tree before catalog");
  std::vector<BYTE> expected(self.m_MDB.drXTFlSize),readback(expected.size());
  in=fopen(directory+"extents.bin","rb"); expect(in!=NULL,"open overflow-tree oracle");
  n=fread(expected.data(),1,expected.size(),in); fclose(in); expect(n==expected.size(),"read overflow-tree oracle");
  self.m_extents_file.Seek(0,CFile::begin);
  expect(self.m_extents_file.Read(readback.data(),(UINT)readback.size())==readback.size() && readback==expected,"self-fragmented overflow tree byte fidelity");
  self.m_catalog_file.Seek(0,CFile::begin);
  expect(self.m_catalog_file.Read(actual.data(),(UINT)actual.size())==actual.size() && actual==oracle,"fragmented catalog via self-fragmented overflow tree");
  CHFVFile file; entry=entryFor(m); expect(file.open_by_CDR(&self,&entry,CHFVFile::OpenResourceFork)!=0,"resource fork via self-fragmented tree");
  expect(file.Read(bytes.data(),m.forkSize)==m.forkSize,"read resource fork via self-fragmented tree");
  for(unsigned i=0;i<m.forkSize;++i) expect(bytes[i]==LargeImagePattern(i,118),"self-fragmented tree payload fidelity");
  self.close();
 }
 // Independently corrupt copies: zero progress, out-of-volume range, bad
 // record-offset table, and cyclic index child. No source fixture is modified.
 for(unsigned mode=0;mode<4;++mode) {
  CString bad; bad.Format("%sfragmented-bad-%u.dsk",(LPCSTR)directory,mode);
  expect(CopyFileA(m.image,bad,TRUE)!=FALSE,"exclusive corrupt fixture copy");
  CFile raw; expect(raw.Open(bad,CFile::modeReadWrite|CFile::shareExclusive)!=FALSE,"open corrupt fixture copy");
  BYTE node[512]; unsigned logical=tree.bthRoot*512;
  if(mode<2) {
   bool patched=false;
   for(unsigned index=0;index<tree.bthNNodes && !patched;++index) {
    raw.Seek(treePosition(mdb,index*512),CFile::begin); expect(raw.Read(node,512)==512,"read corruptible overflow leaf");
    if(node[8]!=ndLeafNode) continue;
    for(unsigned r=0;r<word(node+10);++r) {
     unsigned start=word(node+512-2*(r+1)); BYTE* key=node+start;
     if(number(key+2)==m.fileID && key[1]==0 && word(key+6)==3) {
      if(mode==0) memset(key+8,0,12);
      else { key[8]=key[9]=0xff; }
      raw.Seek(treePosition(mdb,index*512),CFile::begin); raw.Write(node,512); patched=true; break;
     }
    }
   }
   expect(patched,"patch targeted overflow record");
  } else {
   raw.Seek(treePosition(mdb,logical),CFile::begin); expect(raw.Read(node,512)==512,"read overflow root");
   if(mode==2) node[510]=node[511]=0xff;
   else {
    for(unsigned r=0;r<word(node+10);++r) {
     unsigned start=word(node+512-2*(r+1)); BYTE* child=node+start+8;
     child[0]=(BYTE)(tree.bthRoot>>24);child[1]=(BYTE)(tree.bthRoot>>16);child[2]=(BYTE)(tree.bthRoot>>8);child[3]=(BYTE)tree.bthRoot;
    }
   }
   raw.Seek(treePosition(mdb,logical),CFile::begin); raw.Write(node,512);
  }
  raw.Close(); CHFVVolume broken; broken.m_file_name=bad;
  if(broken.open()) {
   CHFVFile file; entry=entryFor(m); expect(file.open_by_CDR(&broken,&entry,CHFVFile::OpenDataFork)!=0,"open corrupted fork");
   expect(file.Read(bytes.data(),m.forkSize)==3*allocation,"invalid overflow data fails safely"); broken.close();
  }
 }
 fprintf(report,"PASS: GUI overflow extents: 16 files with ten-extent data/resource forks, ten-extent catalog and overflow tree, 116 catalog files, indexed lookup, multiple overflow records, random seeks, EOF, missing/corrupt records and cycle bounds\n");
}
#endif
