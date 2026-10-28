// fsprobe PATH...: what Foundation and the kernel report about a path (file protection, readability, volume flags).
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <unistd.h>

static void probe(NSString *path)
{
    NSURL *url = [NSURL fileURLWithPath:path];
    NSArray *keys = @[NSURLFileProtectionKey, NSURLIsReadableKey, NSURLIsDirectoryKey, NSURLVolumeSupportsFileProtectionKey];
    NSError *err = nil;
    NSDictionary *v = [url resourceValuesForKeys:keys error:&err];
    printf("FSPROBE %s: foundation %s err %s\n", path.UTF8String, v.description.UTF8String,
           err ? err.description.UTF8String : "-");

    struct attrlist al = {.bitmapcount = ATTR_BIT_MAP_COUNT, .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_DATA_PROTECT_FLAGS};
    struct { uint32_t len; attribute_set_t returned; uint32_t cls; } __attribute__((packed)) buf = {0};
    int rc = getattrlist(path.fileSystemRepresentation, &al, &buf, sizeof(buf), FSOPT_NOFOLLOW);
    printf("FSPROBE %s: getattrlist rc %d returned.common 0x%x class %u\n", path.UTF8String, rc, buf.returned.commonattr, buf.cls);

    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    int pc = fd >= 0 ? fcntl(fd, F_GETPROTECTIONCLASS) : -2;
    if (fd >= 0)
        close(fd);
    struct statfs sf;
    rc = statfs(path.fileSystemRepresentation, &sf);
    printf("FSPROBE %s: F_GETPROTECTIONCLASS %d, statfs rc %d type %s flags 0x%x (MNT_CPROTECT %d)\n", path.UTF8String, pc, rc,
           rc == 0 ? sf.f_fstypename : "-", rc == 0 ? sf.f_flags : 0, rc == 0 ? !!(sf.f_flags & MNT_CPROTECT) : -1);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        for (int i = 1; i < argc; i++)
            probe(@(argv[i]));
    }
    return 0;
}
