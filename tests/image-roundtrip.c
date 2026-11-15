// SPDX-License-Identifier: GPL-2.0-or-later
// Integration test against the same HFS implementation used by the GUI.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include "hfs/libhfs/hfs.h"
#include "Filehook.h"

static void require(int ok, const char *operation) {
  if (!ok) { fprintf(stderr, "FAIL: %s (%s)\n", operation, hfs_error); exit(1); }
}

int main(int argc, char **argv) {
  char volume[]="X64Test", folder[]="X64Test:Folder";
  char original[]="X64Test:Folder:Roundtrip", renamed[]="X64Test:Folder:Renamed";
  char type[]="TEXT", creator[]="ttxt";
  unsigned char data[8193], resource[1025], buffer[8193];
  hfsvol *v; hfsfile *f; hfsdirent st; hfsdir *dir;
  int fd, i, seen=0;
  if(argc!=2 && !(argc==3 && strcmp(argv[2],"--keep")==0)) { fprintf(stderr,"Usage: image-roundtrip NEW_IMAGE_PATH [--keep]\n"); return 2; }
  // Exclusive create prevents a mistaken invocation overwriting an existing image.
  fd=_open(argv[1],_O_CREAT|_O_EXCL|_O_RDWR|_O_BINARY,_S_IREAD|_S_IWRITE);
  require(fd>=0,"create new image exclusively");
  require(_chsize_s(fd,16*1024*1024)==0,"size image"); _close(fd);
  fhook_set_real_funcs();
  require(hfs_format(argv[1],0,volume)==0,"format HFS image");
  v=hfs_mount(argv[1],0,O_RDWR); require(v!=NULL,"mount read/write");
  require(hfs_mkdir(v,folder)==0,"create folder");
  require(hfs_create(v,original,type,creator)==0,"create file with Finder metadata");
  f=hfs_open(v,original); require(f!=NULL,"open file");
  for(i=0;i<sizeof(data);++i) data[i]=(unsigned char)(i*37);
  for(i=0;i<sizeof(resource);++i) resource[i]=(unsigned char)(i*71);
  require(hfs_write(f,data,sizeof(data))==sizeof(data),"write data fork");
  require(hfs_setfork(f,1)==0,"select resource fork");
  require(hfs_write(f,resource,sizeof(resource))==sizeof(resource),"write resource fork");
  require(hfs_close(f)==0,"close file");
  require(hfs_rename(v,original,renamed)==0,"rename file");
  require(hfs_flush(v)==0,"flush volume");
  require(hfs_umount(v)==0,"unmount");
  v=hfs_mount(argv[1],0,O_RDONLY); require(v!=NULL,"reopen image read-only");
  require(hfs_stat(v,renamed,&st)==0,"stat renamed file");
  require(st.u.file.dsize==sizeof(data) && st.u.file.rsize==sizeof(resource),"preserve both fork sizes");
  require(strcmp(st.u.file.type,"TEXT")==0 && strcmp(st.u.file.creator,"ttxt")==0,"preserve type/creator");
  f=hfs_open(v,renamed); require(f!=NULL,"reopen file");
  require(hfs_read(f,buffer,sizeof(data))==sizeof(data) && memcmp(buffer,data,sizeof(data))==0,"data fork byte roundtrip");
  require(hfs_setfork(f,1)==0,"reselect resource fork");
  require(hfs_read(f,buffer,sizeof(resource))==sizeof(resource) && memcmp(buffer,resource,sizeof(resource))==0,"resource fork byte roundtrip");
  require(hfs_close(f)==0,"close reopened file");
  dir=hfs_opendir(v,folder); require(dir!=NULL,"open folder");
  while(hfs_readdir(dir,&st)==0) if(strcmp(st.name,"Renamed")==0) seen=1;
  require(seen,"enumerate renamed file"); hfs_closedir(dir);
  require(hfs_umount(v)==0,"close read-only image");
  if(argc==3) { puts("PASS: populated image retained for UI verification"); return 0; }
  v=hfs_mount(argv[1],0,O_RDWR); require(v!=NULL,"remount for delete");
  require(hfs_delete(v,renamed)==0,"delete file");
  require(hfs_rmdir(v,folder)==0,"remove empty folder");
  require(hfs_umount(v)==0,"persist deletes");
  v=hfs_mount(argv[1],0,O_RDONLY); require(v!=NULL,"reopen after deletes");
  require(hfs_stat(v,folder,&st)<0,"deleted folder stays absent");
  require(hfs_umount(v)==0,"final close");
  puts("PASS: format, mount, mkdir, create, data/resource forks, metadata, rename, flush, reopen, enumerate, delete");
  return 0;
}
