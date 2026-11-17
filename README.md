# Hollowroot

[Русский](README.ru.md)

> **Status: experimental.** The GSI bootanimation shows on the panel and `system_server` gets far into boot, but Android does not reach the launcher yet. The isolation code added after the last device test has not been run on hardware.

Boots a GSI (LineageOS 23.2, Android 16) on top of a running vendor Android without touching the bootloader or any partition. The phone keeps its locked bootloader, the stock kernel and the stock vendor image. Everything the guest writes lives in RAM and disappears when the session ends.

This repository is the port for the **iQOO Z9 Turbo** (V2352A, Snapdragon 8s Gen 3, OriginOS 6). Other devices need a few constants changed, see [Porting](#porting).

> Read [Safety](#safety) before running anything on a phone you care about.

## How it works

Hollowroot does not flash or boot a second system. It replaces the Android userspace of the running one:

1. The guardian stops the host SurfaceFlinger, Zygote and camera services.
2. The loader enters new PID, network, IPC, UTS and mount namespaces.
3. The GSI `system.img` is mounted read-only from a loop device, `/data` is a 512 MB tmpfs.
4. The host `/vendor`, `/odm`, the activated APEX modules, the GPU and the DRM nodes are bind mounted read-only or as device nodes.
5. `pivot_root` makes the GSI the real root. A plain chroot is not enough, Zygote checks open file descriptors against paths like `/sys/kernel/tracing/trace_marker` and rejects the chroot-prefixed ones.
6. A private binderfs and a private `servicemanager` replace the host binder.
7. The loader starts SurfaceFlinger, bootanimation, a small property service, then Zygote, which forks `system_server`.

Because the host kernel and vendor HALs are reused, the GSI draws on the real panel through the Qualcomm display composer.

## The guardian

`guardian` is the part that gets you back to normal OriginOS. It runs in the host namespace and does the following:

- Creates a cgroup v2 group (`/sys/fs/cgroup/hollowroot`) and puts the loader in it. It refuses to reuse a group that already exists.
- Stops the host graphics, Zygote and camera services, and sets `config.disable_cameraservice=true` so the guest `WindowManager` does not talk to the vendor camera server.
- Launches the loader and waits for a handshake pipe, so a failed `exec` is noticed.
- Every 500 ms writes the OLED backlight. Once the host SurfaceFlinger is stopped the panel goes dark, and normally `system_server` turns it back on, which does not exist yet during early boot.
- Watches the volume keys. Holding Volume Up and Volume Down together for 2 seconds ends the session.
- Ends the session on timer expiry, SIGINT, SIGTERM, SIGHUP, or when the guest exits.
- On exit it kills the whole cgroup with `cgroup.kill`, waits until `cgroup.events` reports `populated 0`, removes the group and only then restarts the host services. If it cannot confirm that the guest is dead, it does not restart them.

Duration is limited to 10 to 300 seconds.

## Status

Last tested on the device before the isolation rewrite described under Safety:

| Part | State |
| --- | --- |
| Mounting the GSI from a loop device under enforcing SELinux | works (needs permissive during the session) |
| SurfaceFlinger on the host display composer | works |
| LineageOS bootanimation on the OLED | works |
| Smoke tests (shell, linker checks, `derive_classpath`, Zygote start) | pass |
| `system_server` boot | reaches boot phase 500, last known blocker was the FRP service timeout |
| Launcher / SystemUI | not reached yet |
| Return to OriginOS | works |

After that the loader was changed a lot (PID/net/IPC/UTS namespaces, binderfs, dropped capabilities, read-only remounts with `mount_setattr`, no host `/metadata`, no host sockets). **None of that has been run on the device yet.** It may well break the framework or the display.

Details on every blocker that was solved are in [docs/notes.md](docs/notes.md).

## Safety

During an early test the guest `system_server` shared `/dev/binder` with OriginOS and talked to the host `vold`. `vold` did not find the user's keys in the guest and recreated `/data/media/0`, which emptied `/sdcard`. Apps and settings survived, the photos did not.

The current loader prevents that by giving the guest its own binderfs and aborting if a binder node matches a host one. This is untested on hardware, so:

- Copy `/sdcard` to a computer and check the copy before using `start` or `takeover`. The script will not run them without `--i-have-a-backup`.
- `smoke` does not stop host services, but it still runs the same namespace setup.
- Keep the phone charged and have a way to reboot it (power button for 10+ seconds).
- You are writing to loop devices and switching SELinux to permissive for the length of the session. The script restores enforcing mode on exit, but if adb dies mid session, check with `getenforce`.

## Requirements

- An Android 14+ phone with a root shell. Temporary root from SukiSU / KernelSU is enough, the bootloader can stay locked.
- A kernel with binderfs (Linux 5.0+). `mount_setattr` (5.12+) is used when present, otherwise the loader aborts if a bind mount has submounts. The test device runs 6.1.
- Free loop devices `/dev/block/loop45` and `/dev/block/loop47` (hard coded, see `src/loader.c`).
- About 3 GB free in `/data/local/tmp` for the image, and 512 MB of free RAM for the guest `/data`.
- On the computer: `adb`, `curl`, `7z`, `make`, Android NDK (r27 or newer), bash.

## Quick start

```sh
git clone https://github.com/NeFaniZzz/hollowroot
cd hollowroot

# optional: change the GSI link or NDK path
$EDITOR config.env

./hollowroot.sh fetch      # download and unpack the GSI
./hollowroot.sh build      # compile with the NDK
./hollowroot.sh push       # copy everything to /data/local/tmp/hollowroot
./hollowroot.sh smoke      # environment checks, no host services stopped
./hollowroot.sh start 120 --i-have-a-backup
./hollowroot.sh logs
```

`start` stops the host UI, shows the guest for 120 seconds and brings OriginOS back. Use `takeover` instead of `start` to run only SurfaceFlinger and the bootanimation. The first cold boot of `system_server` can take 60 to 90 seconds, so shorter sessions will end before it finishes.

If a session crashed and left loop devices attached, run `./hollowroot.sh clean`.

### GSI link

`config.env` has a `GSI_URL` field. The default points to the LineageOS 23.2 vanilla ext4 build from 2026-05-24 by [MisterZtr](https://github.com/MisterZtr/LineageOS_gsi), together with its sha256. Replace the URL (and the hash, or empty it) to use something else. The image has to be ext4 and A16 compatible. If you already have an extracted image, set `GSI_IMG` to its path.

### Running on the device directly

`hollowroot.sh` only wraps adb. On the phone itself:

```sh
su
sh /data/local/tmp/hollowroot/run_session.sh smoke
HOLLOWROOT_BACKUP_OK=1 sh /data/local/tmp/hollowroot/run_session.sh 120 --framework
```

`run_session.sh` creates a 512 KB fake FRP partition in `/dev` (tmpfs), attaches the two loop devices, sets SELinux to permissive, runs the guardian and undoes all of it on exit. It will not touch a loop device that is already in use.

After the namespace is set up the loader sends its output to `/dev/null` (Zygote refuses regular files as stdio), and the guest has no logd socket, so `logcat` shows nothing from it. `session.log` only holds the guardian's messages. For the loader's own output use `smoke`, for crashes during a session look at `dmesg` afterwards. Getting guest logs out during a full session is still open.

## Layout

```
src/loader.c           namespace setup, pivot_root, starts the guest
src/guardian.c         host side watchdog
src/property_seed.c    experimental property service, not used by the loader
tools/dump_properties.c  dumps host properties for property_seed
tests/test_pivot.c     small pivot_root check on a tmpfs
scripts/run_session.sh runs on the phone
hollowroot.sh          runs on the computer
config.env             GSI link, NDK path
docs/notes.md          problems solved so far
```

## Porting

Device specific values are near the top of `src/guardian.c` and `src/loader.c`:

- `BACKLIGHT_DIR` and `BACKLIGHT_VALUE`, find yours in `/sys/class/backlight/`.
- `VOLUME_UP_DEV` / `VOLUME_DOWN_DEV`, check with `getevent -lp`.
- `/dev/kgsl-3d0`, `/dev/dri`, `/dev/dma_heap` bind mounts in the loader (Qualcomm specific).
- The camera service names stopped by the guardian (`vivocameraserver` is a vivo name).
- Loop device numbers `loop45` and `loop47`.

## Known problems

- The loader binds the host's APEX modules. Older notes used the GSI's own APEX modules because OriginOS ships ART with OAT version 260 and this GSI needs 265. If Zygote dies with `Invalid oat version`, that is the reason and the GSI APEXes have to be unpacked and used instead.
- The loader aborts when it cannot isolate something. That is intended, but it means the first runs will probably stop with a `FATAL:` line you have to chase.
- SELinux has to be permissive during the session.
- Wi-Fi, mobile data, audio and camera are not set up.

## License

[PolyForm Noncommercial 1.0.0](LICENSE). You can use, modify and share the code for any noncommercial purpose, as long as you keep the license text and the `Required Notice:` line from `LICENSE`. Selling it, or selling a product built on it, is not allowed. This is a source-available license, not an OSI open source one. The GSI is not part of this repository and has its own licenses.
