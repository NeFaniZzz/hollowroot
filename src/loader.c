#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <linux/loop.h>
#include <linux/ioctl.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <time.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/vfs.h>
#include <linux/capability.h>

// binderfs ioctl definitions, missing from the NDK headers
#define BINDERFS_MAX_NAME 255
struct binderfs_device {
    char name[BINDERFS_MAX_NAME + 1];
    __u32 major;
    __u32 minor;
};
#define BINDER_CTL_ADD _IOWR('b', 1, struct binderfs_device)

#ifndef SYS_pivot_root
#define SYS_pivot_root 41
#endif

#define ROOT_DIR "/data/local/tmp/hollowroot/root"
#define SYSTEM_IMG "/data/local/tmp/hollowroot/system.img"
#define FRP_IMG "/dev/hollowroot/frp.img"   /* has to be on tmpfs, so FRP data never touches flash */
#define TMPFS_MAGIC_VALUE 0x01021994UL

#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#define GSI_MOUNT_ATTR_RDONLY 0x00000001ULL
#define GSI_MOUNT_ATTR_NOSUID 0x00000002ULL
struct gsi_mount_attr {
    __u64 attr_set;
    __u64 attr_clr;
    __u64 propagation;
    __u64 userns_fd;
};

static void die(const char *msg) {
    fprintf(stderr, "[-] FATAL: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void safe_mkdir(const char *path, mode_t mode) {
    if (mkdir(path, mode) < 0 && errno != EEXIST) {
        die(path);
    }
}

static void die_path(const char *what, const char *path) {
    fprintf(stderr, "[-] FATAL: %s: %s: %s\n", what, path, strerror(errno));
    exit(1);
}

// Returns 1 if a mount exists below `mnt`, or if mountinfo cannot be read.
static int has_submounts(const char *mnt) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return 1;
    char line[4096];
    size_t len = strlen(mnt);
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        char *save = NULL;
        char *tok = strtok_r(line, " ", &save);
        for (int field = 1; tok && field < 5; field++) {
            tok = strtok_r(NULL, " ", &save);
        }
        if (tok && strncmp(tok, mnt, len) == 0 && tok[len] == '/') found = 1;
    }
    fclose(f);
    return found;
}

// Make a bind mount and all of its submounts read-only. MS_REMOUNT alone
// does not recurse into submounts, so mount_setattr(AT_RECURSIVE) is used.
static int make_ro_recursive(const char *dst) {
    struct gsi_mount_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_set = GSI_MOUNT_ATTR_RDONLY | GSI_MOUNT_ATTR_NOSUID;
    if (syscall(SYS_mount_setattr, AT_FDCWD, dst, AT_RECURSIVE, &attr, sizeof(attr)) == 0) {
        return 0;
    }
    if (errno != ENOSYS) return -1;
    // Old kernel without mount_setattr: only the top mount gets remounted, so give up if submounts exist.
    if (mount(NULL, dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID, NULL) < 0) return -1;
    if (has_submounts(dst)) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static void bind_ro(const char *src, const char *dst) {
    if (access(src, F_OK) != 0) {
        return;
    }
    struct stat st;
    if (stat(src, &st) == 0 && S_ISDIR(st.st_mode)) {
        safe_mkdir(dst, 0755);
    } else {
        int fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
    }
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0) {
        die_path("bind read-only source", src);
    }
    if (make_ro_recursive(dst) < 0) {
        die_path("read-only remount failed (refusing to continue)", dst);
    }
}

static void bind_rw(const char *src, const char *dst) {
    if (access(src, F_OK) != 0) {
        return;
    }
    struct stat st;
    if (stat(src, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            safe_mkdir(dst, 0755);
        } else if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
            unlink(dst);
            if (mknod(dst, st.st_mode, st.st_rdev) < 0) {
                int fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
                if (fd >= 0) close(fd);
            } else {
                chmod(dst, 0666);
            }
        } else {
            int fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
            if (fd >= 0) close(fd);
        }
    }
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0) {
        if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {
            fprintf(stderr, "[-] Warning: failed to bind %s -> %s: %s\n", src, dst, strerror(errno));
        }
    }
}

static int run_cmd_env(char *const argv[], char *const envp[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (envp) {
            execve(argv[0], argv, envp);
        } else {
            execv(argv[0], argv);
        }
        perror(argv[0]);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int run_cmd(char *const argv[]) {
    char *const default_env[] = {
        "PATH=/system/bin:/apex/com.android.runtime/bin",
        "ANDROID_ROOT=/system",
        "ANDROID_DATA=/data",
        "ANDROID_ART_ROOT=/apex/com.android.art",
        "ANDROID_I18N_ROOT=/apex/com.android.i18n",
        "ANDROID_RUNTIME_ROOT=/apex/com.android.runtime",
        "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
        NULL
    };
    return run_cmd_env(argv, default_env);
}

static pid_t g_anim_pid = -1;

static int receive_prop_string(int fd, char *text, size_t capacity) {
    uint32_t size = 0;
    if (recv(fd, &size, 4, MSG_WAITALL) != 4 || size >= capacity) return -1;
    if (size && recv(fd, text, size, MSG_WAITALL) != (ssize_t)size) return -1;
    text[size] = 0;
    return 0;
}

static void stop_bootanim(void) {
    printf("[+] Stopping BootAnimation...\n");
    fflush(stdout);
    if (g_anim_pid > 0) {
        kill(g_anim_pid, SIGTERM);
        usleep(200000);
        kill(g_anim_pid, SIGKILL);
    }
    // Only the bootanimation we started itself is killed, never anything found by name.
}

static void run_local_property_service(void) {
    unlink("/dev/socket/property_service");
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        perror("property_service socket");
        _exit(1);
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, "/dev/socket/property_service");
    if (bind(server, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("property_service bind");
        _exit(2);
    }
    chmod(address.sun_path, 0666);
    if (listen(server, 16) < 0) {
        perror("property_service listen");
        _exit(3);
    }
    printf("[+] Local property_service daemon listening on %s\n", address.sun_path);
    fflush(stdout);

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        struct timeval timeout = {.tv_sec = 2};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        uint32_t cmd = 0;
        if (recv(client, &cmd, 4, MSG_WAITALL) == 4 && cmd == 0x00020001) {
            char name[256] = {0}, val[512] = {0};
            if (!receive_prop_string(client, name, sizeof(name)) &&
                !receive_prop_string(client, val, sizeof(val))) {
                printf("[prop] %s = %s\n", name, val);
                fflush(stdout);
                if (!strcmp(name, "ctl.start") && !strcmp(val, "idmap2d")) {
                    pid_t idmap_pid = fork();
                    if (idmap_pid == 0) {
                        for (int fd = 3; fd < 256; fd++) close(fd);
                        char *args[] = {"/system/bin/idmap2d", NULL};
                        char *env[] = {
                            "PATH=/system/bin:/apex/com.android.runtime/bin",
                            "ANDROID_ROOT=/system",
                            "ANDROID_DATA=/data",
                            NULL
                        };
                        execve(args[0], args, env);
                        _exit(127);
                    }
                }
                if (!strcmp(name, "sys.boot_completed") && !strcmp(val, "1")) {
                    printf("[+] GSI FULL BOOT COMPLETED (sys.boot_completed=1)!\n");
                    fflush(stdout);
                    stop_bootanim();
                }
                if (!strcmp(name, "service.bootanim.exit") && !strcmp(val, "1")) {
                    printf("[+] SystemServer requested bootanimation exit! Stopping bootanim PID %d...\n", (int)g_anim_pid);
                    fflush(stdout);
                    stop_bootanim();
                }
            }
        }
        uint32_t status = 0; // PROP_SUCCESS
        send(client, &status, 4, MSG_NOSIGNAL);
        close(client);
    }
    _exit(0);
}

static int read_small_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return 0;
}

// Refuse anything that is not a loop device backed by the expected file.
static void verify_loop_device(const char *dev, const char *expected_backing,
                               int require_ro, int require_tmpfs) {
    struct stat st;
    if (stat(dev, &st) < 0) die_path("stat loop device", dev);
    if (!S_ISBLK(st.st_mode) || major(st.st_rdev) != 7) {
        fprintf(stderr, "[-] FATAL: %s is not a loop device (refusing to touch real partitions)\n", dev);
        exit(97);
    }
    unsigned maj = (unsigned)major(st.st_rdev), min = (unsigned)minor(st.st_rdev);
    char path[256], value[512];
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/loop/backing_file", maj, min);
    if (read_small_file(path, value, sizeof(value)) < 0) die_path("read loop backing_file", path);
    if (strcmp(value, expected_backing) != 0) {
        fprintf(stderr, "[-] FATAL: %s is backed by '%s', expected '%s'\n", dev, value, expected_backing);
        exit(97);
    }
    if (require_ro) {
        snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/ro", maj, min);
        if (read_small_file(path, value, sizeof(value)) < 0 || strcmp(value, "1") != 0) {
            fprintf(stderr, "[-] FATAL: %s is not attached read-only\n", dev);
            exit(97);
        }
    }
    if (require_tmpfs) {
        struct statfs sfs;
        if (statfs(expected_backing, &sfs) < 0) die_path("statfs", expected_backing);
        if ((unsigned long)sfs.f_type != TMPFS_MAGIC_VALUE) {
            fprintf(stderr, "[-] FATAL: %s is not on tmpfs (RAM)\n", expected_backing);
            exit(97);
        }
    }
    printf("[+] Verified %s -> %s%s\n", dev, expected_backing, require_ro ? " (read-only)" : "");
}

static volatile pid_t g_ns_init = -1;

static void forward_kill_to_ns(int sig) {
    (void)sig;
    if (g_ns_init > 0) kill(g_ns_init, SIGKILL);
}

// Fork into a new PID namespace where the child becomes PID 1. The parent only
// waits. If it dies, PR_SET_PDEATHSIG takes the namespace init and every guest process down with it.
static void enter_pid_namespace(void) {
    fflush(stdout);
    fflush(stderr);
    if (unshare(CLONE_NEWPID) < 0) {
        die("unshare(CLONE_NEWPID): no PID namespaces, refusing to run unisolated");
    }
    pid_t child = fork();
    if (child < 0) die("fork PID-namespace init");
    if (child == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) die("PR_SET_PDEATHSIG");
        if (getppid() == 0) _exit(98);
        if (getpid() != 1) {
            fprintf(stderr, "[-] FATAL: not PID 1 in the new PID namespace\n");
            _exit(98);
        }
        return;
    }
    g_ns_init = child;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_kill_to_ns;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    int status = 0;
    for (;;) {
        pid_t r = waitpid(child, &status, 0);
        if (r == child) break;
        if (r < 0 && errno != EINTR) {
            kill(child, SIGKILL);
            _exit(1);
        }
    }
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

static const char *const k_binder_names[3] = {"binder", "hwbinder", "vndbinder"};
static dev_t g_host_binder_rdev[3];
static int g_host_binder_known[3];
static dev_t g_host_binderfs_dev;
static int g_host_binderfs_known;

// Must run before pivot_root, while the host /dev is still visible.
static void record_host_binder_identity(void) {
    struct stat st;
    for (int i = 0; i < 3; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/dev/%s", k_binder_names[i]);
        if (stat(p, &st) == 0 && S_ISCHR(st.st_mode)) {
            g_host_binder_rdev[i] = st.st_rdev;
            g_host_binder_known[i] = 1;
            printf("[+] Host %s rdev = %u:%u\n", p,
                   (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
        }
    }
    if (stat("/dev/binderfs", &st) == 0) {
        g_host_binderfs_dev = st.st_dev;
        g_host_binderfs_known = 1;
    }
    if (!g_host_binder_known[0]) {
        fprintf(stderr, "[-] FATAL: cannot identify host /dev/binder; isolation cannot be verified\n");
        exit(99);
    }
}

static int is_host_binder_rdev(dev_t rdev) {
    for (int j = 0; j < 3; j++) {
        if (g_host_binder_known[j] && rdev == g_host_binder_rdev[j]) return 1;
    }
    return 0;
}

// Runs after pivot_root in every mode, before any guest binary starts.
static void setup_private_binder(void) {
    printf("[*] Creating isolated private binder devices...\n");
    if (mount("binder", "/dev/binderfs", "binder", MS_NOSUID | MS_NOEXEC, "max=8") < 0) {
        die("mount private binderfs");
    }
    struct stat fs_st;
    if (stat("/dev/binderfs", &fs_st) < 0) die("stat private binderfs");
    if (g_host_binderfs_known && fs_st.st_dev == g_host_binderfs_dev) {
        fprintf(stderr, "[-] FATAL: /dev/binderfs is the HOST binderfs instance. ABORTING.\n");
        exit(99);
    }
    int ctl = open("/dev/binderfs/binder-control", O_RDONLY | O_CLOEXEC);
    if (ctl < 0) die("open private binder-control");
    for (int i = 0; i < 3; i++) {
        char node[64], link[64];
        snprintf(node, sizeof(node), "/dev/binderfs/%s", k_binder_names[i]);
        snprintf(link, sizeof(link), "/dev/%s", k_binder_names[i]);
        if (access(node, F_OK) != 0) {
            struct binderfs_device bdev;
            memset(&bdev, 0, sizeof(bdev));
            snprintf(bdev.name, sizeof(bdev.name), "%s", k_binder_names[i]);
            if (ioctl(ctl, BINDER_CTL_ADD, &bdev) < 0 && errno != EEXIST) {
                die_path("BINDER_CTL_ADD", node);
            }
        }
        struct stat st;
        if (stat(node, &st) < 0 || !S_ISCHR(st.st_mode)) die_path("stat private binder node", node);
        if (st.st_dev != fs_st.st_dev) {
            fprintf(stderr, "[-] FATAL: %s is not on the private binderfs. ABORTING.\n", node);
            exit(99);
        }
        if (is_host_binder_rdev(st.st_rdev)) {
            fprintf(stderr, "[-] FATAL: %s collides with a HOST binder device. ABORTING.\n", node);
            exit(99);
        }
        chmod(node, 0666);
        if (unlink(link) < 0 && errno != ENOENT) die_path("unlink", link);
        if (symlink(node, link) < 0) die_path("symlink", link);
        struct stat lst;
        if (stat(link, &lst) < 0 || lst.st_rdev != st.st_rdev) {
            fprintf(stderr, "[-] FATAL: %s does not resolve to the private device. ABORTING.\n", link);
            exit(99);
        }
        printf("[+] %s -> %s (private %u:%u)\n", link, node,
               (unsigned)major(st.st_rdev), (unsigned)minor(st.st_rdev));
    }
    close(ctl);
    printf("[+] BINDER ISOLATION VERIFIED for binder, hwbinder and vndbinder\n");
}

// Abort if any inherited descriptor points at a host binder device.
static void check_no_host_binder_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) die("opendir /proc/self/fd");
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int fd = atoi(de->d_name);
        if (fd == dirfd(d)) continue;
        struct stat st;
        if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && is_host_binder_rdev(st.st_rdev)) {
            fprintf(stderr, "[-] FATAL: inherited fd %d is a HOST binder device. ABORTING.\n", fd);
            exit(99);
        }
    }
    closedir(d);
}

// Drop risky capabilities before any guest code runs. Some framework features
// that expect a privileged init may stop working because of this.
static void drop_dangerous_capabilities(void) {
    const int caps[] = {CAP_SYS_RAWIO, CAP_SYS_BOOT, CAP_MKNOD, CAP_SYS_ADMIN,
                        CAP_SYS_MODULE, CAP_SYS_PTRACE, CAP_SYSLOG,
                        CAP_NET_ADMIN, CAP_NET_RAW, CAP_DAC_READ_SEARCH,
                        CAP_SYS_TIME, CAP_SYS_TTY_CONFIG, CAP_SETFCAP,
                        CAP_SETPCAP};
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];
    memset(&hdr, 0, sizeof(hdr));
    memset(data, 0, sizeof(data));
    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    if (syscall(SYS_capget, &hdr, data) < 0) die("capget");
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (prctl(PR_CAPBSET_DROP, caps[i], 0, 0, 0) < 0) die("PR_CAPBSET_DROP");
        unsigned idx = (unsigned)caps[i] / 32, bit = 1u << ((unsigned)caps[i] % 32);
        data[idx].effective &= ~bit;
        data[idx].permitted &= ~bit;
        data[idx].inheritable &= ~bit;
    }
    if (syscall(SYS_capset, &hdr, data) < 0) die("capset");
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) die("PR_SET_NO_NEW_PRIVS");
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        if (prctl(PR_CAPBSET_READ, caps[i], 0, 0, 0) != 0) {
            fprintf(stderr, "[-] FATAL: capability %d still in bounding set\n", caps[i]);
            exit(99);
        }
    }
    printf("[+] Restricted guest capabilities and set no_new_privs\n");
}

/* Closes everything, stdio included: the caller may have passed host files or sockets. */
static void discard_inherited_fds(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) die("open fd directory");
    int scan = dirfd(d);
    struct dirent *de;
    errno = 0;
    while ((de = readdir(d))) {
        char *end;
        long fd = strtol(de->d_name, &end, 10);
        if (*de->d_name && !*end && fd >= 0 && fd != scan) close((int)fd);
        errno = 0;
    }
    if (errno) _exit(99);
    closedir(d);
    int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (fd < 0) _exit(99);
    for (int i = 0; i < 3; i++) if (dup2(fd, i) < 0) _exit(99);
    if (fd > 2) close(fd);
    for (int i = 0; i < 3; i++) if (fcntl(i, F_SETFD, 0) < 0) _exit(99);
}

static pid_t start_private_servicemanager(void) {
    printf("[*] Starting private servicemanager on isolated binder...\n");
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) die("fork servicemanager");
    if (pid == 0) {
        char *sm_argv[] = {"/system/bin/servicemanager", NULL};
        char *sm_env[] = {"PATH=/system/bin", "ANDROID_ROOT=/system", NULL};
        execve(sm_argv[0], sm_argv, sm_env);
        perror("execve servicemanager");
        _exit(127);
    }
    usleep(500000);
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
        fprintf(stderr, "[-] FATAL: private servicemanager exited (status=0x%x)\n", status);
        exit(99);
    }
    printf("[+] Private servicemanager process running (PID %d); complete IPC isolation is not verified.\n", (int)pid);
    fflush(stdout);
    return pid;
}

// PID 1 of the guest namespace has to reap orphaned children.
static void reap_forever(void) {
    fflush(stdout);
    for (;;) {
        int status = 0;
        pid_t p = waitpid(-1, &status, 0);
        if (p > 0) {
            if (p == g_anim_pid) g_anim_pid = -1;
            printf("[ns-init] guest process %d exited (status=0x%x)\n", (int)p, status);
            fflush(stdout);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == ECHILD) {
            printf("[ns-init] no guest processes left, exiting\n");
            fflush(stdout);
            exit(0);
        }
        die("waitpid in ns-init");
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <system_block_dev> [data_block_dev] <--smoke|--takeover|--framework>\n", argv[0]);
        return 2;
    }

    const char *block_dev = argv[1];
    const char *data_dev = NULL;
    const char *mode = "--smoke";

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--smoke") || !strcmp(argv[i], "--takeover") || !strcmp(argv[i], "--framework")) {
            mode = argv[i];
        } else if (argv[i][0] == '/') {
            data_dev = argv[i];
        }
    }

    if (!data_dev) {
        data_dev = "/dev/block/loop46"; // only printed, /data is a tmpfs
    }

    int smoke_only = !strcmp(mode, "--smoke");
    int takeover = !strcmp(mode, "--takeover");
    int framework = !strcmp(mode, "--framework");

    if (getuid() != 0) {
        fprintf(stderr, "Must run as root (uid 0)\n");
        return 3;
    }

    // Own PID namespace: the guest cannot see or signal host processes.
    enter_pid_namespace();

    if (unshare(CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS) < 0)
        die("unshare network/IPC/UTS namespaces (refusing unisolated guest)");

    umask(0000);

    printf("[*] Hollowroot loader\n");
    printf("[*] Mode: %s\n", mode);
    printf("[*] System block device: %s\n", block_dev);
    printf("[*] Data block device: %s\n", data_dev);

    // 1. Unshare mount namespace
    if (unshare(CLONE_NEWNS) < 0) {
        die("unshare mount namespace");
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        die("make mounts private");
    }
    printf("[+] Private mount namespace created\n");
    record_host_binder_identity();

    // 2. Prepare root directory and mount GSI
    safe_mkdir("/data/local/tmp/hollowroot", 0755);
    safe_mkdir(ROOT_DIR, 0755);

    // Only mount a read-only loop device that is backed by our system.img.
    verify_loop_device(block_dev, SYSTEM_IMG, 1, 0);
    if (mount(block_dev, ROOT_DIR, "ext4", MS_RDONLY | MS_NOSUID | MS_NODEV, NULL) < 0) {
        die("mount GSI system partition");
    }
    printf("[+] GSI mounted at %s\n", ROOT_DIR);

    // 3. Bind core filesystems
    char target[512];

    snprintf(target, sizeof(target), "%s/proc", ROOT_DIR);
    safe_mkdir(target, 0755);
    // Fresh procfs of the guest PID namespace. A host /proc would expose
    // every host process root via /proc/<pid>/root.
    if (mount("proc", target, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
        die("mount guest procfs");
    }

    snprintf(target, sizeof(target), "%s/sys", ROOT_DIR);
    safe_mkdir(target, 0755);
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC,
              "size=1m,mode=0555") < 0) die("mount private /sys");

    // Dev tmpfs
    snprintf(target, sizeof(target), "%s/dev", ROOT_DIR);
    safe_mkdir(target, 0755);
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID, "size=16m,mode=0755") < 0) {
        die("mount /dev tmpfs");
    }

    // Host binder nodes are not exposed at all. The guest gets private ones
    // from setup_private_binder() after pivot_root.
    const char *dev_nodes[] = {
        "/dev/null", "/dev/zero", "/dev/urandom", "/dev/random",
        "/dev/ashmem",
        "/dev/kgsl-3d0", // Adreno GPU
        "/dev/fuse"
    };
    for (size_t i = 0; i < sizeof(dev_nodes)/sizeof(dev_nodes[0]); i++) {
        char dst[512];
        snprintf(dst, sizeof(dst), "%s%s", ROOT_DIR, dev_nodes[i]);
        bind_rw(dev_nodes[i], dst);
    }

    char sym_path[512];
    snprintf(sym_path, sizeof(sym_path), "%s/dev/stdin", ROOT_DIR);
    unlink(sym_path);
    symlink("/proc/self/fd/0", sym_path);
    snprintf(sym_path, sizeof(sym_path), "%s/dev/stdout", ROOT_DIR);
    unlink(sym_path);
    symlink("/proc/self/fd/1", sym_path);
    snprintf(sym_path, sizeof(sym_path), "%s/dev/stderr", ROOT_DIR);
    unlink(sym_path);
    symlink("/proc/self/fd/2", sym_path);

    // libcutils looks for /dev/ashmem<boot_id>, so make sure that name exists
    FILE *bf = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (bf) {
        char boot_id[128] = {0};
        if (fgets(boot_id, sizeof(boot_id), bf)) {
            char *p = boot_id;
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            char *end = p + strlen(p) - 1;
            while (end > p && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
                *end = '\0';
                end--;
            }
            if (strlen(p) > 0) {
                char a_boot_dst[512];
                snprintf(a_boot_dst, sizeof(a_boot_dst), "%s/dev/ashmem%s", ROOT_DIR, p);
                if (access(a_boot_dst, F_OK) != 0) {
                    char a_boot_src[512];
                    snprintf(a_boot_src, sizeof(a_boot_src), "/dev/ashmem%s", p);
                    if (access(a_boot_src, F_OK) == 0) {
                        bind_rw(a_boot_src, a_boot_dst);
                    } else {
                        symlink("/dev/ashmem", a_boot_dst);
                    }
                }
            }
        }
        fclose(bf);
    }

    // DRM nodes for the Qualcomm display composer and Adreno
    snprintf(target, sizeof(target), "%s/dev/dri", ROOT_DIR);
    safe_mkdir(target, 0755);
    bind_rw("/dev/dri", target);

    // Mount point for the private binderfs. The host binderfs is never bound.
    snprintf(target, sizeof(target), "%s/dev/binderfs", ROOT_DIR);
    safe_mkdir(target, 0755);

    // DMA heaps used by the display stack
    snprintf(target, sizeof(target), "%s/dev/dma_heap", ROOT_DIR);
    safe_mkdir(target, 0755);
    bind_rw("/dev/dma_heap", target);

    // Input devices
    snprintf(target, sizeof(target), "%s/dev/input", ROOT_DIR);
    safe_mkdir(target, 0755);
    /* Left empty on purpose: host input devices would leak keystrokes and touch events. */

    // Fake FRP partition for PersistentDataBlockService, backed by RAM
    char frp_d[512];
    snprintf(frp_d, sizeof(frp_d), "%s/dev/block", ROOT_DIR);
    safe_mkdir(frp_d, 0755);
    snprintf(frp_d, sizeof(frp_d), "%s/dev/block/bootdevice", ROOT_DIR);
    safe_mkdir(frp_d, 0755);
    snprintf(frp_d, sizeof(frp_d), "%s/dev/block/bootdevice/by-name", ROOT_DIR);
    safe_mkdir(frp_d, 0755);
    snprintf(frp_d, sizeof(frp_d), "%s/dev/block/by-name", ROOT_DIR);
    safe_mkdir(frp_d, 0755);

    // Check that loop47 is backed by the tmpfs frp.img before the guest gets a node for it
    char loop_dst[512];
    snprintf(loop_dst, sizeof(loop_dst), "%s/dev/block/loop47", ROOT_DIR);
    verify_loop_device("/dev/block/loop47", FRP_IMG, 0, 1);
    struct stat frp_stat;
    if (stat("/dev/block/loop47", &frp_stat) < 0) die("stat FRP loop");
    // Create a separate node. chmod/chown on a bind-mounted host node would
    // change the host node itself.
    if (mknod(loop_dst, S_IFBLK | 0600, frp_stat.st_rdev) < 0) die("create guest FRP node");
    if (chown(loop_dst, 1000, 1000) < 0 || chmod(loop_dst, 0660) < 0)
        die("set guest FRP node permissions");

    char frp_path[512];
    snprintf(frp_path, sizeof(frp_path), "%s/dev/block/bootdevice/by-name/frp", ROOT_DIR);
    unlink(frp_path);
    symlink("/dev/block/loop47", frp_path);

    char frp_link[512];
    snprintf(frp_link, sizeof(frp_link), "%s/dev/block/by-name/frp", ROOT_DIR);
    unlink(frp_link);
    symlink("/dev/block/loop47", frp_link);

    // Private socket directory, so the guest cannot reach host init or daemons
    snprintf(target, sizeof(target), "%s/dev/socket", ROOT_DIR);
    safe_mkdir(target, 0755);
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID, "size=16m,mode=0755") < 0) {
        die("mount /dev/socket tmpfs");
    }
    printf("[+] Private /dev/socket tmpfs mounted without host socket forwarding\n");

    // Host properties are not shared, the guest property_service fills this in
    snprintf(target, sizeof(target), "%s/dev/__properties__", ROOT_DIR);
    safe_mkdir(target, 0755);
    printf("[+] Host properties are not exposed\n");

    // 4. Bind vendor and odm
    snprintf(target, sizeof(target), "%s/vendor", ROOT_DIR);
    bind_ro("/vendor", target);

    snprintf(target, sizeof(target), "%s/odm", ROOT_DIR);
    if (access("/odm", F_OK) == 0) {
        bind_ro("/odm", target);
    }

    // Host /system/apex goes over the GSI one so the files listed in apex-info-list.xml match (e.g. com.android.bt.apex)
    snprintf(target, sizeof(target), "%s/system/apex", ROOT_DIR);
    if (access("/system/apex", F_OK) == 0 && access(target, F_OK) == 0) {
        bind_ro("/system/apex", target);
    }

    // Bind host /system_ext/apex
    snprintf(target, sizeof(target), "%s/system/system_ext/apex", ROOT_DIR);
    if (access("/system_ext/apex", F_OK) == 0 && access(target, F_OK) == 0) {
        bind_ro("/system_ext/apex", target);
    }

    // 5. APEX: bind the activated host modules
    snprintf(target, sizeof(target), "%s/apex", ROOT_DIR);
    safe_mkdir(target, 0755);
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_NODEV, "size=64m,mode=0755") < 0) {
        die("mount /apex tmpfs");
    }

    DIR *host_apex = opendir("/apex");
    if (host_apex) {
        struct dirent *de;
        while ((de = readdir(host_apex))) {
            if (de->d_name[0] == '.') continue;
            char h_src[512], h_dst[512];
            snprintf(h_src, sizeof(h_src), "/apex/%s", de->d_name);
            snprintf(h_dst, sizeof(h_dst), "%s/apex/%s", ROOT_DIR, de->d_name);
            bind_ro(h_src, h_dst);
        }
        closedir(host_apex);
    }

    // 6. Bind linkerconfig
    snprintf(target, sizeof(target), "%s/linkerconfig", ROOT_DIR);
    safe_mkdir(target, 0755);
    bind_ro("/linkerconfig", target);

    // 7. /metadata: the host one holds encryption keys and checkpoint state, so
    // the guest gets an empty tmpfs instead.
    snprintf(target, sizeof(target), "%s/metadata", ROOT_DIR);
    safe_mkdir(target, 0755);
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "size=4m,mode=0755") < 0) {
        die("mount /metadata tmpfs");
    }
    snprintf(target, sizeof(target), "%s/metadata/aconfig", ROOT_DIR);
    safe_mkdir(target, 0755);

    // 8. /data is a 512 MB tmpfs, everything the guest writes is gone after the session
    snprintf(target, sizeof(target), "%s/data", ROOT_DIR);
    safe_mkdir(target, 0777);
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID, "size=512m,mode=0777") < 0) {
        die("mount /data tmpfs");
    }
    chmod(target, 0777);
    printf("[+] Fast RAM tmpfs (512MB) mounted at /data\n");

    // Directories the framework expects to exist on first boot
    const char *data_dirs[] = {
        "system", "system/environ", "system/users", "system/users/0",
        "system_de", "system_de/0", "system_ce", "system_ce/0",
        "system_de/0/package_cache",
        "system/sync", "system/dropbox", "system/notification",
        "dalvik-cache", "dalvik-cache/arm64", "local", "local/tmp",
        "misc", "misc/keystore", "misc/gatekeeper", "misc/wifi",
        "misc_de", "misc_de/0", "misc_ce", "misc_ce/0",
        "data", "user", "user/0",
        "user_de", "user_de/0",
        "user_de/0/com.android.providers.settings",
        "user_de/0/com.android.providers.settings/databases",
        "user_de/0/org.lineageos.lineagesettings",
        "user_de/0/org.lineageos.lineagesettings/databases",
        "user_de/0/com.android.providers.telephony",
        "user_de/0/com.android.providers.telephony/databases",
        "user_de/0/com.android.providers.media.module",
        "user_de/0/com.android.providers.media.module/databases",
        "user_de/0/com.android.providers.contacts",
        "user_de/0/com.android.providers.contacts/databases",
        "user_de/0/com.android.providers.calendar",
        "user_de/0/com.android.providers.calendar/databases",
        "user_de/0/com.android.systemui",
        "user_de/0/com.android.systemui/databases",
        "user_de/0/org.lineageos.trebuchet",
        "user_de/0/org.lineageos.trebuchet/databases",
        "user_de/0/com.android.launcher3",
        "user_de/0/com.android.launcher3/databases",
        "user/0/com.android.systemui",
        "user/0/org.lineageos.trebuchet",
        "user/0/com.android.launcher3",
        "resource-cache",
        "media", "media/0"
    };
    for (size_t i = 0; i < sizeof(data_dirs)/sizeof(data_dirs[0]); i++) {
        char dirpath[512];
        snprintf(dirpath, sizeof(dirpath), "%s/data/%s", ROOT_DIR, data_dirs[i]);
        safe_mkdir(dirpath, 0777);
        chmod(dirpath, 0777);
        chown(dirpath, 1000, 1000); // uid/gid 1000 = system
    }

    // Without last-fstrim, StorageManagerService runs fstrim at boot and crashes on the vold call
    char last_fstrim[512];
    snprintf(last_fstrim, sizeof(last_fstrim), "%s/data/system/last-fstrim", ROOT_DIR);
    int fstrim_fd = open(last_fstrim, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fstrim_fd >= 0) {
        char ts[64];
        int tlen = snprintf(ts, sizeof(ts), "%ld\n", (long)time(NULL));
        write(fstrim_fd, ts, tlen);
        close(fstrim_fd);
        chmod(last_fstrim, 0666);
        chown(last_fstrim, 1000, 1000);
    }

    snprintf(target, sizeof(target), "%s/data/local/tmp", ROOT_DIR);
    safe_mkdir(target, 0777);
    // The host project directory is not mounted, not even read-only

    // No host /data/apex binds, even read-only ones count as host data access
    snprintf(target, sizeof(target), "%s/data/apex", ROOT_DIR);
    safe_mkdir(target, 0755);

    printf("[+] Namespace environment configured successfully\n");

    // After pivot_root ROOT_DIR is the real root. Zygote checks descriptor paths against its
    // whitelist, and a plain chroot leaves host-prefixed paths that get rejected.
    char mnt_old[512];
    snprintf(mnt_old, sizeof(mnt_old), "%s/mnt", ROOT_DIR);
    safe_mkdir(mnt_old, 0755);

    if (chdir(ROOT_DIR) < 0 || syscall(SYS_pivot_root, ".", "mnt") < 0 || chdir("/") < 0 || chroot(".") < 0) {
        die("pivot_root into GSI");
    }
    printf("[+] pivot_root into GSI completed! True root is now GSI\n");

    // Drop the old host root
    if (umount2("/mnt", MNT_DETACH) < 0) {
        die("detach old host root");
    }

    // Fresh /mnt with the mount points Android expects
    if (mount("tmpfs", "/mnt", "tmpfs", MS_NOSUID | MS_NODEV, "size=16m,mode=0755") < 0) {
        die("mount /mnt tmpfs");
    }
    // No shared propagation for guest storage mounts
    if (mount(NULL, "/mnt", NULL, MS_PRIVATE | MS_REC, NULL) < 0)
        die("make guest /mnt private");
    const char *storage_dirs[] = {
        "/mnt/user", "/mnt/runtime", "/mnt/runtime/default", "/mnt/runtime/read",
        "/mnt/runtime/write", "/mnt/runtime/full", "/mnt/pass_through", "/mnt/installer",
        "/mnt/androidwritable", "/mnt/expand"
    };
    for (size_t i = 0; i < sizeof(storage_dirs)/sizeof(storage_dirs[0]); i++) {
        safe_mkdir(storage_dirs[i], 0755);
    }

    // Isolation setup, identical for all modes
    setup_private_binder();
    fflush(NULL);
    discard_inherited_fds();
    check_no_host_binder_fds();
    drop_dangerous_capabilities();
    start_private_servicemanager();

    // --smoke: environment checks only
    if (smoke_only) {
        printf("[*] Running in --smoke test mode\n");
        chdir("/");

        printf("--- Test 1: GSI Shell ---\n");
        char *sh_args[] = {"/system/bin/sh", "-c", "echo 'SUCCESS: GSI shell is alive!' && getprop ro.build.version.release", NULL};
        int rc = run_cmd(sh_args);
        printf("GSI shell exit code: %d\n", rc);

        printf("--- Test 1b: Ashmem device verification ---\n");
        char *ash_args[] = {"/system/bin/sh", "-c", "ls -la /dev/*ashmem*", NULL};
        rc = run_cmd(ash_args);
        printf("Ashmem verify exit code: %d\n", rc);

        printf("--- Test 2: SurfaceFlinger Linker Dependencies ---\n");
        char *sf_args[] = {"/system/bin/linker64", "--list", "/system/bin/surfaceflinger", NULL};
        rc = run_cmd(sf_args);
        printf("SurfaceFlinger link exit code: %d\n", rc);

        printf("--- Test 3: app_process64 (Zygote) Linker Dependencies ---\n");
        char *ap_args[] = {"/system/bin/linker64", "--list", "/system/bin/app_process64", NULL};
        rc = run_cmd(ap_args);
        printf("app_process64 link exit code: %d\n", rc);

        printf("--- Test 4: BootAnimation Linker Dependencies ---\n");
        char *anim_args[] = {"/system/bin/linker64", "--list", "/system/bin/bootanimation", NULL};
        rc = run_cmd(anim_args);
        printf("bootanimation link exit code: %d\n", rc);

        printf("--- Test 5: derive_classpath generation ---\n");
        char *dc_args[] = {"/apex/com.android.sdkext/bin/derive_classpath", "/data/system/environ/classpath", NULL};
        rc = run_cmd(dc_args);
        printf("derive_classpath exit code: %d\n", rc);

        char *cat_args[] = {"/system/bin/head", "-n", "10", "/data/system/environ/classpath", NULL};
        run_cmd(cat_args);

        printf("--- Test 6: app_process64 Zygote real socket initialization test ---\n");
        unlink("/dev/socket/zygote_smoke");
        int z_smoke = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un sun_z = {.sun_family = AF_UNIX};
        strcpy(sun_z.sun_path, "/dev/socket/zygote_smoke");
        bind(z_smoke, (struct sockaddr*)&sun_z, sizeof(sun_z));
        chown(sun_z.sun_path, 0, 1000);
        chmod(sun_z.sun_path, 0660);

        char z_smoke_env[64];
        snprintf(z_smoke_env, sizeof(z_smoke_env), "ANDROID_SOCKET_zygote_smoke=%d", z_smoke);

        char *debug_sh[] = {
            "/system/bin/sh", "-c",
            "echo '>>> SCRIPT STARTED <<<'; "
            "while read command name value; do [ -z \"$command\" ] && continue; [ \"$command\" = export ] || exit 2; export \"$name=$value\" || exit 3; done < /data/system/environ/classpath; "
            "echo '>>> CLASSPATH EXPORTED <<<'; "
            "export ANDROID_SOCKET_zygote_smoke; "
            "/system/bin/app_process64 -Xzygote /system/bin --zygote --start-system-server --abi-list=arm64-v8a --socket-name=zygote_smoke > /data/system/zygote_debug.log 2>&1 & "
            "ZYG_PID=$!; "
            "echo \"[+] Zygote started with PID $ZYG_PID, waiting 3s...\"; "
            "sleep 3; "
            "if kill -0 $ZYG_PID 2>/dev/null; then echo \"[+] SUCCESS: Zygote is ALIVE and RUNNING!\"; kill -9 $ZYG_PID; exit 0; else echo \"[-] Zygote exited prematurely, log:\"; cat /data/system/zygote_debug.log; exit 1; fi",
            NULL
        };
        char *debug_env[] = {
            "PATH=/system/bin:/apex/com.android.runtime/bin",
            "ANDROID_ROOT=/system",
            "ANDROID_DATA=/data",
            "ANDROID_ART_ROOT=/apex/com.android.art",
            "ANDROID_I18N_ROOT=/apex/com.android.i18n",
            "ANDROID_RUNTIME_ROOT=/apex/com.android.runtime",
            "ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
            z_smoke_env,
            NULL
        };
        rc = run_cmd_env(debug_sh, debug_env);
        close(z_smoke);
        unlink("/dev/socket/zygote_smoke");
        printf("app_process64 real Zygote test exit code: %d\n", rc);

        printf("[*] All smoke tests complete!\n");
        return 0;
    }

    // --takeover: SurfaceFlinger and BootAnimation only
    if (takeover) {
        printf("[*] Running in --takeover mode: launching display stack\n");
        chdir("/");

        printf("[+] Starting guest SurfaceFlinger...\n");
        pid_t sf_pid = fork();
        if (sf_pid == 0) {
            char *sf_argv[] = {"/system/bin/surfaceflinger", NULL};
            char *sf_env[] = {
                "PATH=/system/bin:/apex/com.android.runtime/bin",
                "ANDROID_ROOT=/system",
                NULL
            };
            execve(sf_argv[0], sf_argv, sf_env);
            perror("execve surfaceflinger");
            _exit(127);
        }
        printf("[+] Guest SurfaceFlinger launched (PID %d)\n", (int)sf_pid);

        usleep(1500000);

        printf("[+] Starting guest BootAnimation on physical screen...\n");
        pid_t anim_pid = fork();
        if (anim_pid == 0) {
            char *anim_argv[] = {"/system/bin/bootanimation", NULL};
            char *anim_env[] = {
                "PATH=/system/bin",
                "ANDROID_ROOT=/system",
                NULL
            };
            execve(anim_argv[0], anim_argv, anim_env);
            perror("execve bootanimation");
            _exit(127);
        }
        printf("[+] Guest BootAnimation launched (PID %d)\n", (int)anim_pid);

        printf("[*] Display takeover fully active! Holding until guardian signals exit...\n");
        fflush(stdout);

        reap_forever();
    }

    // --framework: property service, display, Zygote and SystemServer
    if (framework) {
        printf("[*] Running in --framework mode: booting full GSI Android environment\n");
        chdir("/");

        // Classpath
        printf("[*] Step 1: Generating runtime classpath...\n");
        char *dc_args[] = {"/apex/com.android.sdkext/bin/derive_classpath", "/data/system/environ/classpath", NULL};
        int rc = run_cmd(dc_args);
        printf("[+] derive_classpath finished (exit code %d)\n", rc);

        // Binder is already private here (setup_private_binder). With the host
        // binder the guest StorageManagerService reached the host vold, which
        // recreated /data/media/0 and wiped /sdcard.

        // SurfaceFlinger
        printf("[*] Step 2: Starting guest SurfaceFlinger...\n");
        pid_t sf_pid = fork();
        if (sf_pid == 0) {
            char *sf_argv[] = {"/system/bin/surfaceflinger", NULL};
            char *sf_env[] = {
                "PATH=/system/bin:/apex/com.android.runtime/bin",
                "ANDROID_ROOT=/system",
                NULL
            };
            execve(sf_argv[0], sf_argv, sf_env);
            perror("execve surfaceflinger");
            _exit(127);
        }
        printf("[+] Guest SurfaceFlinger launched (PID %d)\n", (int)sf_pid);
        usleep(1500000);

        // BootAnimation
        printf("[*] Step 3: Starting guest BootAnimation...\n");
        pid_t anim_pid = fork();
        if (anim_pid == 0) {
            char *anim_argv[] = {"/system/bin/bootanimation", NULL};
            char *anim_env[] = {"PATH=/system/bin", "ANDROID_ROOT=/system", NULL};
            execve(anim_argv[0], anim_argv, anim_env);
            perror("execve bootanimation");
            _exit(127);
        }
        printf("[+] Guest BootAnimation launched (PID %d)\n", (int)anim_pid);
        g_anim_pid = anim_pid;

        // Minimal property_service
        printf("[*] Launching local property_service daemon...\n");
        pid_t prop_pid = fork();
        if (prop_pid == 0) {
            run_local_property_service();
            _exit(0);
        }
        printf("[+] Property service daemon active (PID %d)\n", (int)prop_pid);
        usleep(150000);

        // Zygote sockets
        printf("[*] Step 4: Preparing Zygote IPC sockets...\n");
        unlink("/dev/socket/zygote");
        int zygote_sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (zygote_sock < 0) die("create zygote socket");
        struct sockaddr_un sun_zygote = {.sun_family = AF_UNIX};
        strcpy(sun_zygote.sun_path, "/dev/socket/zygote");
        if (bind(zygote_sock, (struct sockaddr*)&sun_zygote, sizeof(sun_zygote)) < 0) die("bind zygote socket");
        chown(sun_zygote.sun_path, 0, 1000);
        chmod(sun_zygote.sun_path, 0660);

        unlink("/dev/socket/usap_pool_primary");
        int usap_sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (usap_sock < 0) die("create usap socket");
        struct sockaddr_un sun_usap = {.sun_family = AF_UNIX};
        strcpy(sun_usap.sun_path, "/dev/socket/usap_pool_primary");
        if (bind(usap_sock, (struct sockaddr*)&sun_usap, sizeof(sun_usap)) < 0) die("bind usap socket");
        chown(sun_usap.sun_path, 0, 1000);
        chmod(sun_usap.sun_path, 0660);

        printf("[+] Zygote sockets ready (zygote_fd=%d, usap_fd=%d)\n", zygote_sock, usap_sock);

        // Zygote and SystemServer
        printf("[*] Step 5: Launching guest Zygote and SystemServer...\n");
        pid_t zygote_pid = fork();
        if (zygote_pid == 0) {
            // Close everything except the two sockets
            DIR *fds = opendir("/proc/self/fd");
            if (fds) {
                struct dirent *fe;
                while ((fe = readdir(fds))) {
                    int fd = atoi(fe->d_name);
                    if (fd > 2 && fd != dirfd(fds) && fd != zygote_sock && fd != usap_sock) {
                        close(fd);
                    }
                }
                closedir(fds);
            }

            // stdio to /dev/null, the Zygote fd whitelist rejects regular files
            int null_fd = open("/dev/null", O_RDWR);
            if (null_fd >= 0) {
                dup2(null_fd, 0);
                dup2(null_fd, 1);
                dup2(null_fd, 2);
                if (null_fd > 2) close(null_fd);
            }

            char zygote_env[64];
            snprintf(zygote_env, sizeof(zygote_env), "ANDROID_SOCKET_zygote=%d", zygote_sock);
            char usap_env[80];
            snprintf(usap_env, sizeof(usap_env), "ANDROID_SOCKET_usap_pool_primary=%d", usap_sock);

            char zygote_cmd[2048];
            snprintf(zygote_cmd, sizeof(zygote_cmd),
                "while read command name value; do [ -z \"$command\" ] && continue; [ \"$command\" = export ] || exit 2; export \"$name=$value\" || exit 3; done < /data/system/environ/classpath; "
                "export %s; "
                "export %s; "
                "export PATH=/system/bin:/apex/com.android.runtime/bin; "
                "export ANDROID_ROOT=/system; "
                "export ANDROID_DATA=/data; "
                "export ANDROID_ART_ROOT=/apex/com.android.art; "
                "export ANDROID_I18N_ROOT=/apex/com.android.i18n; "
                "export ANDROID_RUNTIME_ROOT=/apex/com.android.runtime; "
                "export ANDROID_TZDATA_ROOT=/apex/com.android.tzdata; "
                "exec /system/bin/app_process64 -Xzygote /system/bin --zygote --start-system-server --abi-list=arm64-v8a --socket-name=zygote",
                zygote_env, usap_env);

            char *sh_argv[] = {"/system/bin/sh", "-c", zygote_cmd, NULL};
            execv(sh_argv[0], sh_argv);
            perror("execv zygote sh");
            _exit(127);
        }

        // Only Zygote keeps the sockets
        close(zygote_sock);
        close(usap_sock);

        printf("[+] Guest Zygote launched (PID %d). SystemServer will fork shortly...\n", (int)zygote_pid);
        printf("[*] Full GSI framework active! Holding until guardian signals exit...\n");
        fflush(stdout);

        reap_forever();
    }

    return 0;
}
