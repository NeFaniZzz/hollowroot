#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <sys/system_properties.h>
#include <linux/input.h>
#include <linux/magic.h>
#include <poll.h>
#include <time.h>

#define CGROUP_DIR "/sys/fs/cgroup/hollowroot"
#define LOADER_PATH "/data/local/tmp/hollowroot/loader"

/* iQOO Z9 Turbo specific. Check with `getevent -lp` and /sys/class/backlight on other devices. */
#define BACKLIGHT_DIR "/sys/class/backlight/panel0-backlight"
#define BACKLIGHT_VALUE "4000\n"
#define VOLUME_UP_DEV "/dev/input/event0"
#define VOLUME_DOWN_DEV "/dev/input/event2"
#define CHORD_HOLD_MS 2000

static volatile sig_atomic_t stop_signal;
static pid_t guest_pid = -1;

static void on_signal(int sig) { stop_signal = sig; }

static int write_control(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = strlen(value);
    ssize_t n;
    do { n = write(fd, value, len); } while (n < 0 && errno == EINTR);
    int saved = errno;
    if (close(fd) < 0 && n == (ssize_t)len) return -1;
    if (n != (ssize_t)len) { errno = n < 0 ? saved : EIO; return -1; }
    return 0;
}

/* Only an exact populated field is accepted; missing or malformed data fails closed. */
static int cgroup_populated(void) {
    char buf[512];
    int fd = open(CGROUP_DIR "/cgroup.events", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, buf, sizeof(buf) - 1); } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n <= 0 || n == (ssize_t)sizeof(buf) - 1) { errno = n < 0 ? saved : EIO; return -1; }
    buf[n] = '\0';
    char *save, *line = strtok_r(buf, "\n", &save);
    int found = -1;
    while (line) {
        if (!strcmp(line, "populated 0")) { if (found != -1) return -1; found = 0; }
        else if (!strcmp(line, "populated 1")) { if (found != -1) return -1; found = 1; }
        else if (!strncmp(line, "populated", 9)) return -1;
        line = strtok_r(NULL, "\n", &save);
    }
    return found;
}

static int setup_cgroup(void) {
    struct statfs fs;
    if (statfs("/sys/fs/cgroup", &fs) < 0 || (unsigned long)fs.f_type != CGROUP2_SUPER_MAGIC) {
        fprintf(stderr, "Not a usable cgroup v2 mount\n"); return -1;
    }
    /* Never adopt an existing group: it may belong to another session. */
    if (mkdir(CGROUP_DIR, 0755) < 0) { perror("mkdir cgroup"); return -1; }
    int kill_fd = open(CGROUP_DIR "/cgroup.kill", O_WRONLY | O_CLOEXEC);
    int procs_fd = open(CGROUP_DIR "/cgroup.procs", O_WRONLY | O_CLOEXEC);
    int empty = cgroup_populated();
    if (kill_fd < 0 || procs_fd < 0 || empty != 0) {
        fprintf(stderr, "Cgroup controls unavailable or group not empty\n");
        if (kill_fd >= 0) close(kill_fd);
        if (procs_fd >= 0) close(procs_fd);
        if (empty == 0 && rmdir(CGROUP_DIR) < 0) perror("rmdir cgroup");
        return -1;
    }
    close(kill_fd); close(procs_fd);
    return 0;
}

static int property(const char *name, const char *value) {
    if (__system_property_set(name, value) == 0) return 0;
    fprintf(stderr, "Property failed: %s=%s\n", name, value);
    return -1;
}

static int restore_host_services(void) {
    int failed = 0;
    fprintf(stderr, "[*] Terminating guest cgroup...\n");
    if (guest_pid > 0) {
        /* The child may not have joined the cgroup yet, so kill it directly too. */
        if (kill(guest_pid, SIGKILL) < 0 && errno != ESRCH) {
            perror("kill child"); return -1;
        }
        int reaped = 0;
        for (int i = 0; i < 30; i++) {
            pid_t r = waitpid(guest_pid, NULL, WNOHANG);
            if (r == guest_pid || (r < 0 && errno == ECHILD)) { reaped = 1; break; }
            if (r < 0 && errno != EINTR) { perror("waitpid"); break; }
            usleep(100000);
        }
        if (!reaped) {
            fprintf(stderr, "CRITICAL: child not confirmed dead; host services NOT restored\n");
            return -1;
        }
        guest_pid = -1;
    }
    if (write_control(CGROUP_DIR "/cgroup.kill", "1") < 0) {
        perror("cgroup.kill"); failed = 1;
    }
    int empty = 0;
    for (int i = 0; i < 30; i++) {
        int state = cgroup_populated();
        if (state == 0) { empty = 1; break; }
        if (state < 0) { fprintf(stderr, "Cannot verify cgroup.events\n"); break; }
        usleep(100000);
    }
    if (!empty) {
        fprintf(stderr, "CRITICAL: guest cgroup not verified empty; host services NOT restored\n");
        return -1;
    }
    if (rmdir(CGROUP_DIR) < 0) {
        perror("rmdir cgroup; host services NOT restored"); return -1;
    }
    usleep(300000);
    failed |= property("ctl.start", "surfaceflinger") != 0;
    usleep(500000);
    failed |= property("ctl.start", "zygote") != 0;
    failed |= property("ctl.start", "zygote_secondary") != 0;
    usleep(500000);
    failed |= property("config.disable_cameraservice", "false") != 0;
    failed |= property("ctl.start", "cameraserver") != 0;
    failed |= property("ctl.start", "vivocameraserver") != 0;
    if (failed) fprintf(stderr, "Restoration incomplete or termination reported an error\n");
    else fprintf(stderr, "Restoration commands dispatched (service state not verified)\n");
    return failed ? -1 : 0;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void force_panel_on(void) {
    if (write_control(BACKLIGHT_DIR "/bl_power", "0\n") < 0 ||
        write_control(BACKLIGHT_DIR "/brightness", BACKLIGHT_VALUE) < 0)
        perror("backlight");
}

int main(int argc, char **argv) {
    int duration_seconds = 60, result = 0, host_changed = 0;
    if (argc >= 2) {
        char *end;
        errno = 0;
        long n = strtol(argv[1], &end, 10);
        if (errno || !*argv[1] || *end || n < 10 || n > 300) {
            fprintf(stderr, "Duration must be between 10 and 300 seconds\n"); return 1;
        }
        duration_seconds = (int)n;
    }
    if (getuid() != 0) { fprintf(stderr, "Must run as root\n"); return 1; }
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    int signals[] = { SIGINT, SIGTERM, SIGHUP, SIGALRM };
    for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++)
        if (sigaction(signals[i], &sa, NULL) < 0) { perror("sigaction"); return 1; }
    if (setup_cgroup() < 0) return 1;
    if (stop_signal) { result = 1; goto cleanup; }

    host_changed = 1;
    if (property("ctl.stop", "zygote_secondary") || property("ctl.stop", "zygote")) { result = 1; goto cleanup; }
    usleep(800000);
    if (stop_signal) { result = 1; goto cleanup; }
    if (property("ctl.stop", "bootanim") || property("ctl.stop", "surfaceflinger")) { result = 1; goto cleanup; }
    usleep(500000);
    if (stop_signal) { result = 1; goto cleanup; }
    if (property("config.disable_cameraservice", "true") ||
        property("vold.checkpoint_committed", "1") || property("ctl.stop", "cameraserver") ||
        property("ctl.stop", "vivocameraserver")) { result = 1; goto cleanup; }
    usleep(200000);
    if (stop_signal) { result = 1; goto cleanup; }

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) { perror("pipe2"); result = 1; goto cleanup; }
    guest_pid = fork();
    if (guest_pid < 0) { perror("fork"); close(pipefd[0]); close(pipefd[1]); result = 1; goto cleanup; }
    if (guest_pid == 0) {
        close(pipefd[0]);
        /* A successful exec closes the pipe (O_CLOEXEC), a failure writes errno into it. */
        if (write_control(CGROUP_DIR "/cgroup.procs", "0") == 0) {
            struct sigaction def = {0};
            def.sa_handler = SIG_DFL; sigemptyset(&def.sa_mask);
            for (size_t i = 0; i < sizeof(signals)/sizeof(signals[0]); i++)
                if (sigaction(signals[i], &def, NULL) < 0) goto child_error;
            const char *mode = argc > 2 ? argv[2] : "--framework";
            char *args[] = { LOADER_PATH, "/dev/block/loop45", "/dev/block/loop46", (char *)mode, NULL };
            execv(args[0], args);
        }
child_error:;
        int error = errno;
        (void)write(pipefd[1], &error, sizeof(error));
        _exit(127);
    }
    close(pipefd[1]);
    int child_error = 0;
    ssize_t n;
    do { n = read(pipefd[0], &child_error, sizeof(child_error)); } while (n < 0 && errno == EINTR && !stop_signal);
    close(pipefd[0]);
    if (n != 0 || stop_signal) {
        if (n > 0) fprintf(stderr, "Child attach/exec failed: %s\n", strerror(child_error));
        else if (n < 0) perror("child handshake");
        result = 1; goto cleanup;
    }
    fprintf(stderr, "Guest launched: %d\n", (int)guest_pid);
    int up = open(VOLUME_UP_DEV, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int down = open(VOLUME_DOWN_DEV, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct pollfd fds[2] = { { .fd = up, .events = POLLIN }, { .fd = down, .events = POLLIN } };
    int pressed[2] = {0, 0};
    long long start = monotonic_ms(), chord = -1;
    if (start < 0) result = 1;
    while (!result && !stop_signal) {
        force_panel_on();
        int r = poll(fds, 2, 500);
        if (r < 0 && errno != EINTR) { perror("poll"); result = 1; break; }
        long long now = monotonic_ms();
        if (now < 0) { result = 1; break; }
        if (now - start >= (long long)duration_seconds * 1000) break;
        for (int i = 0; i < 2; i++) {
            struct input_event ev;
            if (fds[i].revents & POLLIN)
                while (read(fds[i].fd, &ev, sizeof(ev)) == sizeof(ev))
                    if (ev.type == EV_KEY && ev.code == (i ? KEY_VOLUMEDOWN : KEY_VOLUMEUP)) pressed[i] = ev.value > 0;
        }
        if (pressed[0] && pressed[1]) {
            if (chord < 0) chord = now;
            else if (now - chord >= CHORD_HOLD_MS) break;
        } else chord = -1;
        int status;
        pid_t w = waitpid(guest_pid, &status, WNOHANG);
        if (w == guest_pid) { guest_pid = -1; fprintf(stderr, "Guest exited (status=%d)\n", status); result = 1; break; }
        if (w < 0 && errno != EINTR) { perror("waitpid"); result = 1; break; }
    }
    if (up >= 0) close(up);
    if (down >= 0) close(down);
cleanup:
    if (stop_signal) { fprintf(stderr, "Signal %d received\n", (int)stop_signal); result = 1; }
    if (host_changed) {
        if (restore_host_services() < 0) result = 1;
    } else if (cgroup_populated() != 0 || rmdir(CGROUP_DIR) < 0) {
        fprintf(stderr, "Could not safely remove cgroup\n"); result = 1;
    }
    return result;
}
