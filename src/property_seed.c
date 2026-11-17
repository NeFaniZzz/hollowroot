// Experimental property service. Not used by loader.c, which has its own minimal one.
// Builds a private property area and answers setprop requests on /dev/socket/property_service.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <grp.h>

static pid_t idmap_pid = -1;
static pid_t codec_list_pid = -1;

static void reap_codec_list(void) {
    if (codec_list_pid > 1 && waitpid(codec_list_pid, NULL, WNOHANG) == codec_list_pid) {
        printf("PRIVATE_CODEC_LIST_REAPED_PID=%d\n", codec_list_pid);
        codec_list_pid = -1;
    }
}

static int start_codec_list(void) {
    reap_codec_list();
    if (codec_list_pid > 1) return 0;
    codec_list_pid = fork();
    if (codec_list_pid < 0) return -1;
    if (!codec_list_pid) {
        for (int fd = 3; fd < 1024; fd++) close(fd);
        gid_t group = 1013;
        if (setgroups(1, &group) || setgid(1013) || setuid(1013)) _exit(91);
        char *args[] = {"/system/bin/mediacodeclist_generator", NULL};
        char *env[] = {
            "PATH=/system/bin",
            "ANDROID_ROOT=/system",
            "ANDROID_DATA=/data",
            "LD_LIBRARY_PATH=/apex/com.android.runtime/lib64/bionic:/apex/com.android.art/lib64:/apex/com.android.i18n/lib64:/apex/com.android.conscrypt/lib64:/apex/com.android.os.statsd/lib64:/apex/com.android.tethering/lib64:/system/lib64",
            NULL
        };
        execve(args[0], args, env);
        _exit(127);
    }
    printf("PRIVATE_CODEC_LIST_LAZY_STARTED_PID=%d\n", codec_list_pid);
    return 0;
}

static pid_t supplicant_pid = -1;
static int assign(const char *name, const char *value);

static void reap_private_supplicant(void) {
    if (supplicant_pid > 1) {
        int status;
        pid_t done = waitpid(supplicant_pid, &status, WNOHANG);
        if (done == supplicant_pid) {
            printf("PRIVATE_SUPPLICANT_REAPED_PID=%d\n", supplicant_pid);
            supplicant_pid = -1;
            assign("init.svc.wpa_supplicant", "stopped");
        }
    }
}

static int start_private_supplicant(void) {
    if (access("/data/local/tmp/wifi-handoff.ready", R_OK)) return -1;
    reap_private_supplicant();
    if (supplicant_pid > 1) return 0;
    supplicant_pid = fork();
    if (supplicant_pid < 0) return -1;
    if (!supplicant_pid) {
        for (int fd = 3; fd < 1024; fd++) close(fd);
        char *args[] = {"/vendor/bin/hw/wpa_supplicant", "-O/data/vendor/wifi/wpa/sockets", "-g/data/vendor/wifi/wpa/global", "-dd", NULL};
        char *env[] = {"PATH=/system/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", "LD_LIBRARY_PATH=/vendor/lib64:/system/lib64", NULL};
        execve(args[0], args, env);
        _exit(127);
    }
    printf("PRIVATE_SUPPLICANT_STARTED_PID=%d\n", supplicant_pid);
    return assign("init.svc.wpa_supplicant", "running");
}

static int stop_private_supplicant(void) {
    reap_private_supplicant();
    if (supplicant_pid <= 1) return 0;
    kill(supplicant_pid, SIGTERM);
    for (int i = 0; i < 10; i++) {
        reap_private_supplicant();
        if (supplicant_pid <= 1) return 0;
        usleep(100000);
    }
    kill(supplicant_pid, SIGKILL);
    while (waitpid(supplicant_pid, NULL, 0) < 0) if (errno != EINTR) return -1;
    supplicant_pid = -1;
    return assign("init.svc.wpa_supplicant", "stopped");
}

static void reap_private_idmap(void) {
    if (idmap_pid > 1) {
        int status;
        pid_t done = waitpid(idmap_pid, &status, WNOHANG);
        if (done == idmap_pid) {
            printf("PRIVATE_IDMAP2D_REAPED_PID=%d\n", idmap_pid);
            idmap_pid = -1;
            assign("init.svc.idmap2d", "stopped");
        }
    }
}

static int start_private_idmap(void) {
    reap_private_idmap();
    if (idmap_pid > 1) return 0;
    idmap_pid = fork();
    if (idmap_pid < 0) return -1;
    if (!idmap_pid) {
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (setgid(1000) || setuid(1000)) _exit(91);
        char *args[] = {"/system/bin/idmap2d", NULL};
        char *env[] = {"PATH=/system/bin", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", NULL};
        execve(args[0], args, env);
        _exit(127);
    }
    printf("PRIVATE_IDMAP2D_STARTED_PID=%d\n", idmap_pid);
    return assign("init.svc.idmap2d", "running");
}

static int (*add_property)(const char*, unsigned int, const char*, unsigned int);

static int assign(const char *name, const char *value) {
    const prop_info *info = __system_property_find(name);
    if (info) {
        int (*update)(prop_info*, const char*, unsigned int) = dlsym(RTLD_DEFAULT, "__system_property_update");
        return update ? update((prop_info*)info, value, strlen(value)) : -1;
    }
    return add_property(name, strlen(name), value, strlen(value));
}

static int gsi_properties(void) {
    FILE *input = fopen("/system/build.prop", "r");
    if (!input) return 8;
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, input) > 0) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals++ = 0;
        equals[strcspn(equals, "\r\n")] = 0;
        if (assign(line, equals)) {
            fprintf(stderr, "Cannot seed GSI property %s\n", line);
            free(line);
            fclose(input);
            return 9;
        }
    }
    free(line);
    fclose(input);
    return 0;
}

static int receive_string(int fd, char *text, size_t capacity) {
    uint32_t size;
    if (recv(fd, &size, 4, MSG_WAITALL) != 4 || size >= capacity) return -1;
    if (size && recv(fd, text, size, MSG_WAITALL) != (ssize_t)size) return -1;
    text[size] = 0;
    return 0;
}

static int test_string_protocol(void) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 90;
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char text[8];
    uint32_t size = 0;
    if (write(pair[0], &size, sizeof(size)) != sizeof(size) || receive_string(pair[1], text, sizeof(text)) || text[0]) return 91;
    size = 3;
    if (write(pair[0], &size, sizeof(size)) != sizeof(size) || write(pair[0], "abc", 3) != 3 || receive_string(pair[1], text, sizeof(text)) || strcmp(text, "abc")) return 92;
    size = sizeof(text);
    if (write(pair[0], &size, sizeof(size)) != sizeof(size) || receive_string(pair[1], text, sizeof(text)) != -1) return 93;
    close(pair[0]);
    close(pair[1]);
    puts("PRIVATE_PROPERTY_EMPTY_AND_NONEMPTY_PROTOCOL_SELF_TEST_PASSED");
    return 0;
}

static int serve(void) {
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, "/dev/socket/property_service");
    unlink(address.sun_path);
    const char *socket_context = "u:object_r:property_socket:s0";
    if (server < 0 || bind(server, (struct sockaddr*)&address, sizeof(address)) ||
        chmod(address.sun_path, 0666) ||
        lsetxattr(address.sun_path, "security.selinux", socket_context, strlen(socket_context) + 1, 0) ||
        listen(server, 8)) {
        perror("property_service bind/listen failed");
        return 20;
    }
    setbuf(stdout, NULL);
    puts("PRIVATE_PROPERTY_SERVICE_READY_NO_INIT_CONTROL");
    for (;;) {
        reap_private_idmap();
        reap_private_supplicant();
        reap_codec_list();
        struct pollfd ready = {.fd = server, .events = POLLIN};
        int pending = poll(&ready, 1, 500);
        if (pending < 0) {
            if (errno == EINTR) continue;
            return 21;
        }
        if (!pending) continue;
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            return 21;
        }
        struct timeval timeout = {.tv_sec = 2};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        uint32_t command = 0, status = 0x0004;
        char name[1024], value[8192];
        if (recv(client, &command, 4, MSG_WAITALL) == 4 && command == 0x00020001 &&
            !receive_string(client, name, sizeof(name)) &&
            !receive_string(client, value, sizeof(value))) {
            if (!strcmp(name, "ctl.start") && !strcmp(value, "idmap2d")) status = start_private_idmap() ? 0x0008 : 0;
            else if ((!strcmp(name, "ctl.interface_start") && !strcmp(value, "aidl/media.codeclist.generator")) || (!strcmp(name, "ctl.start") && !strcmp(value, "media.codeclist.generator"))) status = start_codec_list() ? 0x0008 : 0;
            else if ((!strcmp(name, "ctl.start") && !strcmp(value, "wpa_supplicant")) || (!strcmp(name, "ctl.interface_start") && !strcmp(value, "aidl/android.hardware.wifi.supplicant.ISupplicant/default"))) status = start_private_supplicant() ? 0x0008 : 0;
            else if (!strcmp(name, "ctl.stop") && !strcmp(value, "wpa_supplicant")) status = stop_private_supplicant() ? 0x0008 : 0;
            else if (!strncmp(name, "ro.", 3) || !strncmp(name, "ctl.", 4) || !strcmp(name, "sys.powerctl")) status = 0x0020;
            else status = assign(name, value) ? 0x0008 : 0;
            printf("PRIVATE_SETPROP name=%s status=%u\n", name, status);
        }
        send(client, &status, 4, MSG_NOSIGNAL);
        close(client);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test-protocol")) return test_string_protocol();
    int serving = argc == 2 && !strcmp(argv[1], "--serve");
    int display = argc == 2 && !strcmp(argv[1], "--display");
    if (argc != 1 && !serving && !display) return 1;

    int (*area_init)(void) = dlsym(RTLD_DEFAULT, "__system_property_area_init");
    add_property = dlsym(RTLD_DEFAULT, "__system_property_add");
    if (!area_init || !add_property) {
        fprintf(stderr, "Missing __system_property_area_init or __system_property_add\n");
        return 2;
    }
    int result = area_init();
    printf("PRIVATE_PROPERTY_AREA_INIT=%d\n", result);
    if (result) return 3;

    if (gsi_properties()) {
        fprintf(stderr, "Failed to load GSI properties from build.prop\n");
        return 9;
    }

    const char *seed_file = "/data/local/tmp/hollowroot/property-seed.bin";
    if (access(seed_file, R_OK) != 0) {
        seed_file = "/data/local/tmp/property-seed.bin";
    }
    FILE *input = fopen(seed_file, "rb");
    unsigned count = 0;
    if (input) {
        char name[1024], value[8192];
        int first;
        while ((first = fgetc(input)) != EOF) {
            size_t n = 0;
            name[n++] = (char)first;
            while (name[n-1]) {
                int c = fgetc(input);
                if (c == EOF || n >= sizeof(name)) break;
                name[n++] = (char)c;
            }
            n = 0;
            do {
                int c = fgetc(input);
                if (c == EOF || n >= sizeof(value)) break;
                value[n++] = (char)c;
            } while (value[n-1]);

            if (!strncmp(name, "init.svc.", 9) || !strncmp(name, "init.svc_debug_pid.", 19)) continue;
            if (!__system_property_find(name) && assign(name, value)) {
                // some host properties cannot be added, skip them
            } else {
                count++;
            }
        }
        fclose(input);
    } else {
        printf("[!] Warning: could not open %s\n", seed_file);
    }

    assign("ro.zygote.disable_gl_preload", "0");
    assign("hwservicemanager.disabled", "false");
    assign("dalvik.vm.jdwp-provider", "none");

    if (serving) {
        assign("sys.boot_completed", "0");
        assign("dev.bootcomplete", "0");
        assign("ro.zygote.disable_gl_preload", "1");
        assign("hwservicemanager.disabled", "false");
        assign("hwservicemanager.ready", "false");
        assign("ro.zygote", "zygote64");
        assign("ro.product.cpu.abilist", "arm64-v8a");
        assign("ro.product.cpu.abilist64", "arm64-v8a");
        assign("ro.product.cpu.abilist32", "");
        assign("ro.cp_system_other_odex", "0");
        assign("ro.crypto.state", "unencrypted");
        assign("ro.crypto.type", "");
        assign("ro.frp.pst", "");
        assign("pm.dexopt.first-boot", "verify");
        assign("init.svc.idmap2d", "stopped");
        assign("bpf.progs_loaded", "0");
        assign("ro.bpf.kver_override", "3.18.0");
        assign("ro.fuse.bpf.is_running", "false");
        assign("persist.sys.fuse.passthrough.enable", "false");
        assign("ro.hardware.audio.primary", "pineapple");
        assign("net.dns1", "8.8.8.8");
        assign("net.dns2", "1.1.1.1");
        assign("net.dns3", "9.9.9.9");
        puts("PRIVATE_PROPERTIES_SEEDED_SUCCESSFULLY");
    }

    if (display) {
        assign("sys.boot_completed", "0");
        assign("dev.bootcomplete", "0");
        assign("debug.sf.nobootanimation", "1");
    }

    char sdk[PROP_VALUE_MAX] = {0};
    __system_property_get("ro.build.version.sdk", sdk);
    printf("PRIVATE_GSI_PROPERTIES_READY sdk=%s inherited=%u\n", sdk, count);

    // Android 14 (SDK 34) or newer
    int sdk_num = atoi(sdk);
    if (sdk_num < 34) {
        fprintf(stderr, "SDK version too low: %s\n", sdk);
        return 10;
    }

    return serving ? serve() : 0;
}
