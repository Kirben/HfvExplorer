/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Serialized classic-HFS copy replacement. Keep the old forks until the new
 * file has been closed and flushed. HFS has no atomic multi-record journal:
 * interrupted publication can leave the old file under the .bak name. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef HFV_TESTING
extern void hfv_hfs_recovery_checkpoint(unsigned phase);
#endif
#ifdef HFV_TESTING
extern int hfv_replacement_fault;
#else
#define hfv_replacement_fault 0
#endif
typedef struct {
 hfsvol *volume;
 long parent;
 int active;
 char original[HFS_MAX_FLEN+1], temporary[HFS_MAX_FLEN+1], backup[HFS_MAX_FLEN+1];
} HfsReplacement;
static int replacement_unique(hfsvol *volume,char *name,const char *suffix,unsigned long *serial) {
 hfsdirent entry;unsigned attempt;
 for(attempt=0;attempt<1000;++attempt) {
  sprintf(name,"HFV%08lX.%s",++*serial,suffix);
  if(hfs_stat(volume,name,&entry)<0) {if(errno==ENOENT)return 0;return -1;}
 }
 hfs_error="Could not reserve HFS replacement names";return -1;
}
static hfsfile *replacement_open(HfsReplacement *state,hfsvol *volume,char *name,char *type,char *creator) {
 hfsdirent old;long cwd;hfsfile *file=0;
 unsigned long serial=0;
 memset(state,0,sizeof(*state));
 if(hfs_stat(volume,name,&old)<0) {
  if(errno!=ENOENT||hfs_create(volume,name,type,creator)<0)return 0;
  return hfs_open(volume,name);
 }
 if(old.flags&(HFS_ISDIR|HFS_ISLOCKED)) {hfs_error="Cannot replace a folder or locked HFS file";return 0;}
 cwd=hfs_getcwd(volume);
 if(hfs_setcwd(volume,old.parid)<0)return 0;
 state->volume=volume;state->parent=old.parid;strcpy(state->original,old.name);
 if(replacement_unique(volume,state->temporary,"tmp",&serial)==0&&replacement_unique(volume,state->backup,"bak",&serial)==0&&
    hfs_create(volume,state->temporary,type,creator)==0) {
  file=hfs_open(volume,state->temporary);
  if(file)state->active=1;else hfs_delete(volume,state->temporary);
 }
 if(hfs_setcwd(volume,cwd)<0) {if(file)hfs_close(file);return 0;}
 return file;
}
static void replacement_finish(HfsReplacement *state,int *result,char **error) {
 long cwd;int displaced=0,published=0;
 if(!state->active)return;
 cwd=hfs_getcwd(state->volume);
 if(hfs_setcwd(state->volume,state->parent)<0) {*result=-1;*error="Cannot reopen replacement directory; original retained";return;}
 if(*result==0&&hfs_flush(state->volume)<0) {*result=-1;*error="Cannot flush staged HFS file; original retained";}
 if(*result==0) {
  if(hfs_rename(state->volume,state->original,state->backup)<0) {*result=-1;*error=hfs_error;}
  else {
   displaced=1;
#ifdef HFV_TESTING
   hfv_hfs_recovery_checkpoint(4);
#endif
   if(hfv_replacement_fault==2||hfs_rename(state->volume,state->temporary,state->original)<0) {*result=-1;*error=hfs_error;}
   else published=1;
  }
 }
 if(displaced&&!published) {
  if(hfs_rename(state->volume,state->backup,state->original)<0) {
   hfs_note_rollback(3);
   *error="HFS publication and rollback failed; previous file retained under HFV*.bak";
  } else hfs_note_rollback(1);
 }
 if(published) {
  if(hfs_flush(state->volume)<0) {hfs_note_rollback(2);*result=-1;*error="HFS publication flush failed; previous file retained under HFV*.bak";}
  else if(hfs_delete(state->volume,state->backup)<0) {*result=-1;*error="New HFS file saved; previous file retained under HFV*.bak";}
 } else if(hfs_delete(state->volume,state->temporary)<0)hfs_note_rollback(3);
 else if(*result<0)hfs_note_rollback(1);
 state->active=0;
 if(hfs_setcwd(state->volume,cwd)<0) {*result=-1;*error="Could not restore HFS directory after replacement";}
}
