// Headless integration test of the actual GUI storage classes.
// This is compiled only into the Test configuration; no window is created.
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef HFV_TESTING
#include "stdafx.h"
#include "Mactypes.h"
#include "ImageSize.h"
#include "LargeImageTestData.h"
#include <stdio.h>
#include <vector>

static void require(bool ok,const char* operation) {
    if(!ok) throw operation;
}
int RunLargeImageGuiTest(const char* metadata,const char* reportPath) {
    FILE* report=NULL;
    HANDLE reportHandle=CreateFileA(reportPath,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,0,NULL);
    if(reportHandle==INVALID_HANDLE_VALUE) return 1;
    CloseHandle(reportHandle);
    report=fopen(reportPath,"w"); if(!report) return 1;
    try {
        LargeImageManifest m={};
        FILE* source=fopen(metadata,"rb"); require(source!=NULL,"open manifest");
        size_t got=fread(&m,sizeof(m),1,source); fclose(source);
        require(got==1 && !memcmp(m.magic,"HFVL64",7) && m.imageSize==6ULL*1024*1024*1024,"validate manifest");
        hfv_size_t parsed;
        require(ParseImageSize("2 GB",&parsed) && parsed==2ULL*1024*1024*1024,"parse 2 GiB");
        require(ParseImageSize("6 GiB",&parsed) && parsed==m.imageSize,"parse 6 GiB");
        require(ParseImageSize("6144 MB",&parsed) && parsed==m.imageSize,"parse large MB size");
        require(!ParseImageSize("-6 GB",&parsed) && !ParseImageSize("6 garbage",&parsed) &&
                !ParseImageSize("18446744073709551616 GB",&parsed),"reject invalid/overflow sizes");
        require(sizeof(LongInt)==4 && sizeof(Integer)==2,"preserve HFS field widths");
        CDSKFile raw;
        require(raw.Open(m.image,CFile::modeReadWrite|CFile::shareExclusive)!=FALSE,"GUI adapter opens 6 GiB image");
        require(raw.GetLength()==m.imageSize,"GUI adapter 64-bit length");
        const ULONGLONG gib=1024ULL*1024*1024;
        const ULONGLONG offsets[]={2*gib-257,2*gib+12345,4*gib-257,4*gib+12345,6*gib-4096};
        BYTE originals[ARRAYSIZE(offsets)][1024];
        BYTE before[1024],sentinel[1024],after[1024];
        for(unsigned n=0;n<ARRAYSIZE(offsets);++n) {
            ULONGLONG low=offsets[n]&0x7fffffffULL;
            require(raw.Seek(offsets[n],CFile::begin)==offsets[n] && raw.Read(originals[n],sizeof(originals[n]))==sizeof(originals[n]),"save target bytes");
            require(raw.Seek(low,CFile::begin)==low && raw.Read(before,sizeof(before))==sizeof(before),"capture low alias guard");
            for(unsigned i=0;i<sizeof(sentinel);++i) sentinel[i]=(BYTE)(i*71+n);
            require(raw.Seek(offsets[n],CFile::begin)==offsets[n],"GUI seek across 2/4 GiB");
            raw.Write(sentinel,sizeof(sentinel)); raw.Flush();
            require(raw.Seek(offsets[n],CFile::begin)==offsets[n] && raw.Read(after,sizeof(after))==sizeof(after) &&
                    !memcmp(after,sentinel,sizeof(after)),"GUI high-offset write/read byte fidelity");
            if(low!=offsets[n])
                require(raw.Seek(low,CFile::begin)==low && raw.Read(after,sizeof(after))==sizeof(after) &&
                        !memcmp(after,before,sizeof(after)),"high write does not alias low offset");
        }
        raw.Close();
        require(raw.Open(m.image,CFile::modeReadWrite|CFile::shareExclusive)!=FALSE,"reopen GUI adapter");
        require(raw.Seek(-4096,CFile::end)==m.imageSize-4096,"negative seek relative to 64-bit EOF");
        for(unsigned i=0;i<sizeof(sentinel);++i) sentinel[i]=(BYTE)(i*71+4);
        require(raw.Read(after,sizeof(after))==sizeof(after) && !memcmp(after,sentinel,sizeof(after)),"GUI high writes survive reopen");
        for(unsigned n=0;n<ARRAYSIZE(offsets);++n) {
            for(unsigned i=0;i<sizeof(sentinel);++i) sentinel[i]=(BYTE)(i*71+n);
            require(raw.Seek(offsets[n],CFile::begin)==offsets[n] && raw.Read(after,sizeof(after))==sizeof(after) &&
                    !memcmp(after,sentinel,sizeof(after)),"all boundary writes survive reopen");
            raw.Seek(offsets[n],CFile::begin); raw.Write(originals[n],sizeof(originals[n]));
        }
        raw.Flush(); raw.Close();
        CHFVVolume volume;
        volume.m_file_name=m.image; require(volume.open()!=0,"GUI volume parser opens large image");
        require(volume.m_length==m.imageSize,"GUI volume preserves 64-bit size");
        for(unsigned c=0;c<2;++c) {
            CatDataRec entry={};
            entry.u.f.filLgLen=m.cases[c].dataSize; entry.u.f.filRLgLen=m.cases[c].resourceSize;
            for(unsigned e=0;e<3;++e) {
                entry.u.f.filExtRec[e].xdrStABN=m.cases[c].data[e].start; entry.u.f.filExtRec[e].xdrNumABlks=m.cases[c].data[e].count;
                entry.u.f.filRExtRec[e].xdrStABN=m.cases[c].resource[e].start; entry.u.f.filRExtRec[e].xdrNumABlks=m.cases[c].resource[e].count;
            }
            if(c==0 && entry.u.f.filExtRec[0].xdrNumABlks>2 && !entry.u.f.filExtRec[1].xdrNumABlks) {
                entry.u.f.filExtRec[1].xdrStABN=entry.u.f.filExtRec[0].xdrStABN+2;
                entry.u.f.filExtRec[1].xdrNumABlks=entry.u.f.filExtRec[0].xdrNumABlks-2;
                entry.u.f.filExtRec[0].xdrNumABlks=2;
            }
            for(unsigned fork=0;fork<2;++fork) {
                CHFVFile file;
                unsigned length=fork?m.cases[c].resourceSize:m.cases[c].dataSize;
                require(file.open_by_CDR(&volume,&entry,fork?CHFVFile::OpenResourceFork:CHFVFile::OpenDataFork)!=0,"GUI opens high-extent fork");
                std::vector<BYTE> bytes(length);
                require(file.Read(bytes.data(),length)==length,"GUI reads high-extent fork");
                for(unsigned i=0;i<length;++i) require(bytes[i]==LargeImagePattern(i,m.cases[c].seed+(fork?101:0)),"GUI high-extent payload fidelity");
                file.Close();
            }
        }
        volume.close();
        fprintf(report,"PASS: GUI storage classes: 6 GiB length, seeks and persistent writes across 2/4 GiB, alias guards, high HFS extents, both forks, creation size parsing\n");
    } catch(const char* operation) { fprintf(report,"FAIL: %s\n",operation); fclose(report); return 1; }
      catch(CException* error) { char text[512]; error->GetErrorMessage(text,sizeof(text)); fprintf(report,"FAIL: MFC: %s\n",text); error->Delete(); fclose(report); return 1; }
    fclose(report); return 0;
}
#endif
