#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#ifndef SYS_pivot_root
#define SYS_pivot_root 41
#endif

int main() {
    printf("[*] Testing pivot_root in isolated namespace...\n");
    if (unshare(CLONE_NEWNS) < 0) {
        perror("unshare");
        return 1;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("mount private");
        return 2;
    }

    const char *tmp = "/data/local/tmp/hollowroot/test_piv";
    mkdir(tmp, 0755);

    if (mount("tmpfs", tmp, "tmpfs", 0, "size=16m,mode=0755") < 0) {
        perror("mount tmpfs");
        return 3;
    }

    char mnt_dir[256];
    snprintf(mnt_dir, sizeof(mnt_dir), "%s/mnt", tmp);
    mkdir(mnt_dir, 0755);

    if (chdir(tmp) < 0) {
        perror("chdir tmp");
        return 4;
    }

    if (syscall(SYS_pivot_root, ".", "mnt") < 0) {
        perror("syscall pivot_root");
        return 5;
    }

    if (chdir("/") < 0 || chroot(".") < 0) {
        perror("chroot .");
        return 6;
    }

    printf("[+] pivot_root succeeded! Current root is now tmpfs.\n");

    if (umount2("/mnt", MNT_DETACH) < 0) {
        perror("umount2 /mnt");
        return 7;
    }

    printf("[+] Detached old root cleanly! Test passed 100%%!\n");
    return 0;
}
