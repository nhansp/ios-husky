/*
 * Shrink a mounted f2fs online (F2FS_IOC_RESIZE_FS), to free the tail of
 * Android's userdata partition for the iOS root. Run as root on the device:
 *
 *   f2fs_shrink /data <new_size_bytes>
 *
 * The size must be a whole number of f2fs sections (2 MiB on husky). The
 * kernel migrates live blocks out of the tail first; Android never grows the
 * filesystem back (fstab.zuma's userdata entry has no "resize" flag).
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -o f2fs_shrink f2fs_shrink.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define F2FS_IOC_RESIZE_FS	_IOW(0xf5, 16, uint64_t)

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <mountpoint> <new_size_bytes>\n", argv[0]);
		return 2;
	}
	uint64_t bytes = strtoull(argv[2], NULL, 0);
	if (!bytes || bytes % 4096) {
		fprintf(stderr, "size must be a non-zero multiple of 4096\n");
		return 2;
	}
	uint64_t blocks = bytes / 4096;
	int fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	printf("resizing %s to %llu blocks (%llu bytes)...\n", argv[1],
	       (unsigned long long)blocks, (unsigned long long)bytes);
	fflush(stdout);
	if (ioctl(fd, F2FS_IOC_RESIZE_FS, &blocks) < 0) {
		fprintf(stderr, "F2FS_IOC_RESIZE_FS: %s (%d)\n", strerror(errno), errno);
		return 1;
	}
	puts("done");
	return 0;
}
