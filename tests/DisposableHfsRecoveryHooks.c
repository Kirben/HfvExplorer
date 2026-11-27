// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/ClassicHfsRecovery.h"

/* Standalone tests operate on newly created disposable images. */
int hfv_hfs_recover_image(int fd, int writable) { return 0; }
int hfv_hfs_before_sector_write(int fd, unsigned long long sector) { return 0; }
void hfv_hfs_after_sector_write(int fd) {}
