// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef WIN32
#include <io.h>
#include <windows.h>
#undef ERROR
#endif
#include <errno.h>
#include <stdio.h>
#include "libhfs/internal.h"
#include "libhfs/block.h"
#include "../Filehook.h"

static HFV_THREAD_LOCAL int rollback_status;
void hfs_note_rollback(int status) {
 if(status>rollback_status)rollback_status=status;
}
int hfs_take_rollback(void) {
 int status=rollback_status;
 rollback_status=0;
 return status;
}

/* Runtime undo for serialized classic-HFS metadata operations. This is not
 * a persistent journal: it handles reported errors, not process/power loss. */
#include "ClassicHfsSectorUndo.h"
#include "ClassicHfsVolumeSnapshot.h"
struct ClassicHfsTransaction {
 hfsvol *volume;
 ClassicHfsVolumeSnapshot snapshot;
 ClassicHfsSectorUndo undo;
};
#define active_mutation (hfs_context_get()->mutation)
static void free_mutation(struct ClassicHfsTransaction *m) {
 ClassicHfsReleaseSnapshot(&m->snapshot);
 free(m->undo.records);
 free(m);
}
int ClassicHfsBeginTransaction(hfsvol *vol) {
 struct ClassicHfsTransaction *m;
 if(vol->flags&HFS_WRITE_FAILED){ERROR(EIO,"Classic HFS writes disabled after rollback failure");return -1;}
 if(vol->flags&HFS_READONLY){ERROR(EROFS,"Volume is read-only");return -1;}
 if(active_mutation){ERROR(EBUSY,"Nested classic HFS mutation");return -1;}
 m=(struct ClassicHfsTransaction*)calloc(1,sizeof(*m));
 if(!m){ERROR(ENOMEM,"Cannot allocate HFS undo state");return -1;}
 m->volume=vol;
 if(ClassicHfsCaptureVolume(&m->snapshot,vol)<0) {
  free_mutation(m);ERROR(ENOMEM,"Cannot snapshot classic HFS volume and open files");return -1;
 }
 active_mutation=m;
 /* Earlier pending writes also need undo coverage before they reach disk. */
 if(hfs_flush_raw(vol)<0){ClassicHfsEndTransaction(vol,-1);return -1;}
 return 0;
}
int ClassicHfsCaptureBeforeWrite(hfsvol *volume, unsigned long sector)
{
    struct ClassicHfsTransaction *m = active_mutation;
    if (!m || m->volume != volume)
        return 0;
    ClassicHfsCaptureResult result = ClassicHfsCaptureSector(&m->undo, sector, volume, ClassicHfsLoadRawSector);
    if (result == ClassicHfsCaptureLimit) {
        ERROR(ENOMEM, "HFS metadata undo limit exceeded");
    } else if (result == ClassicHfsCaptureNoMemory) {
        ERROR(ENOMEM, "Cannot grow HFS undo records");
    }
    return result == ClassicHfsCaptured ? 0 : -1;
}
/* Failed rollback evidence, not a write-ahead/crash journal. The 512-byte
 * HFSUNDO1 header records source identity and volume range. Each 528-byte
 * record is LE absolute byte offset(8), CRC32(4), length(4), original bytes(512).
 * No automatic replay: source identity/media must be checked before recovery. */
#ifdef WIN32
static void undo_u32(unsigned char *p,unsigned long n) {unsigned i;for(i=0;i<4;++i){p[i]=(unsigned char)n;n>>=8;}}
static void undo_u64(unsigned char *p,hfv_size_t n) {unsigned i;for(i=0;i<8;++i){p[i]=(unsigned char)n;n>>=8;}}
static unsigned long undo_crc(const unsigned char *p,size_t n) {
 unsigned long crc=0xffffffffUL;unsigned bit;
 while(n--){crc^=*p++;for(bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320UL:0);}
 return ~crc;
}
static int save_failed_undo(struct ClassicHfsTransaction *m,char *path) {
 char directory[MAX_PATH];unsigned char header[512]={0},record[528];HANDLE file;DWORD written;unsigned i;int ok=1;
 DWORD n=GetTempPathA(sizeof(directory),directory);
 if(!n||n>=sizeof(directory)||!GetTempFileNameA(directory,"hfu",0,path))return 0;
 file=CreateFileA(path,GENERIC_WRITE|DELETE,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
 if(file==INVALID_HANDLE_VALUE)return 0;
 memcpy(header,"HFSUNDO1",8);undo_u32(header+8,1);undo_u32(header+12,512);
 undo_u64(header+16,m->snapshot.original.vstart);undo_u64(header+24,m->snapshot.original.vlen);undo_u32(header+32,m->undo.count);
 undo_u64(header+56,((hfv_size_t)m->snapshot.original.vstart+m->snapshot.original.vlen)*512);
 memcpy(header+344,m->snapshot.original.mdb.drVN,sizeof(m->snapshot.original.mdb.drVN));undo_u32(header+376,m->snapshot.original.mdb.drCrDate);
 /* The native media adapter reserves large descriptor values, not CRT fds. */
 if(m->snapshot.original.fd>=0&&m->snapshot.original.fd<0x10000000) {
  HANDLE source=(HANDLE)_get_osfhandle(m->snapshot.original.fd);BY_HANDLE_FILE_INFORMATION info;LARGE_INTEGER size;
  if(source!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(source,&info)) {
   undo_u32(header+40,info.dwVolumeSerialNumber);undo_u32(header+44,info.nFileIndexLow);undo_u32(header+48,info.nFileIndexHigh);
   memcpy(header+64,&info.ftCreationTime,8);memcpy(header+72,&info.ftLastWriteTime,8);
   if(GetFileSizeEx(source,&size))undo_u64(header+56,size.QuadPart);
   if(GetFinalPathNameByHandleA(source,(char*)header+80,260,FILE_NAME_NORMALIZED)>=260)header[80]=0;
  }
 }
 undo_u32(header+36,undo_crc(header,sizeof(header)));
 if(!WriteFile(file,header,sizeof(header),&written,0)||written!=sizeof(header))ok=0;
 for(i=0;ok&&i<m->undo.count;++i) {
  ClassicHfsUndoRecord *b=&m->undo.records[i];undo_u64(record,((hfv_size_t)m->snapshot.original.vstart+b->sector)*512);
  undo_u32(record+8,undo_crc(b->before,512));undo_u32(record+12,512);memcpy(record+16,b->before,512);
  if(!WriteFile(file,record,sizeof(record),&written,0)||written!=sizeof(record))ok=0;
 }
 if(ok&&!FlushFileBuffers(file))ok=0;
 if(!ok){FILE_DISPOSITION_INFO removePartial={TRUE};SetFileInformationByHandle(file,FileDispositionInfo,&removePartial,sizeof(removePartial));}
 CloseHandle(file);return ok;
}
#endif
void b_quarantine_volume(hfsvol *vol,const char *message) {
 FREE(vol->failure_message);vol->failure_message=(char*)ClassicHfsCopyMemory(message,strlen(message)+1);
 vol->flags|=HFS_WRITE_FAILED|HFS_READONLY;
}
static void quarantine_mutation(struct ClassicHfsTransaction *m,hfsvol *vol) {
 char message[512];
#ifdef WIN32
 char path[MAX_PATH];
 if(save_failed_undo(m,path))snprintf(message,sizeof(message),"Classic HFS rollback failed; writes disabled. Undo records saved to \"%s\". Repair before writing.",path);
 else
#endif
 snprintf(message,sizeof(message),"Classic HFS rollback failed; writes disabled. Undo records could not be saved. Repair before writing.");
 b_quarantine_volume(vol,message);
}
static int sync_undo_volume(void *volume)
{
    return fhook_sync(((hfsvol *)volume)->fd);
}

int ClassicHfsEndTransaction(hfsvol *vol,int result) {
 struct ClassicHfsTransaction *m=active_mutation;int rollback=0,saved_errno;char *saved_error;
 if(!m||m->volume!=vol){ERROR(EIO,"Missing HFS undo state");return -1;}
 if(result==0&&hfs_flush_raw(vol)<0)result=-1;
 if(result==0&&m->undo.count&&fhook_sync(vol->fd)<0){ERROR(errno,"Error flushing HFS device");result=-1;}
 saved_errno=errno;
 saved_error=hfs_error;
 /* Replay must bypass capture, or restoring a sector would record it again. */
 active_mutation=NULL;
 if(result<0) {
  rollback=ClassicHfsRestoreSectors(m->undo.records,m->undo.count,vol,ClassicHfsStoreRawSector,ClassicHfsLoadRawSector,sync_undo_volume);
  ClassicHfsRestoreVolume(&m->snapshot,vol);
  if(m->undo.count)hfs_note_rollback(rollback?3:1);
  if(rollback)quarantine_mutation(m,vol);
 }
 free_mutation(m);
 if(rollback){ERROR(EIO,vol->failure_message?vol->failure_message:"Classic HFS rollback failed; writes disabled");return -1;}
 errno=saved_errno;hfs_error=saved_error;return result;
}
