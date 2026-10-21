#pragma once
/* Bring the internal UFS up for xnu (shim/ufs.c); ufs_ready says it worked. */
void ufs_bringup(void);
extern int ufs_ready;
