# Notes: problems solved so far

Short history of what blocked the boot and what fixed it. Useful if you hit the same error on another device.

## Mounting the image

- `mount` of the loop device returned EIO. The kernel worker could not read `system.img` under enforcing SELinux because of its file context. Fix: `chcon u:object_r:media_rw_data_file:s0` on the image, and permissive mode during the session.
- Kernel 6.1 rejects `noload` on an ext4 image that needs journal replay. Fix: do not pass it, and mount read-only.

## Display

- The guest SurfaceFlinger crashed with `Expecting header 0x53595354 but found 0x0. Mixing copies of libbinder?`. It shared `/dev/socket` with the host `init`, so its `ctl.start bootanim` launched the vendor's bootanimation, built against a different libbinder. Fix: private tmpfs for `/dev/socket`.
- Frames were rendered but the screen stayed black. Stopping the host SurfaceFlinger makes the display driver turn the backlight off, and it is normally turned on again by `LightsService` in `system_server`. Fix: the guardian writes `bl_power` and `brightness` in a loop.

## Runtime and classpath

- Zygote died with `Invalid oat version, expected 0x32363000, got 0x32363500`. The host ART is OAT 260, the GSI is built for 265. Fix at the time: unpack the GSI's `.capex` modules and use them as `/apex`. See the note about APEX in the README, the current loader binds the host modules instead.
- `libc++.so not found` from `libicu.so`. Relative symlinks inside the unpacked APEX folders pointed to `/apex/system/lib64`. Fix: absolute `/system/lib64/...` targets.
- `derive_classpath` aborted with `must not export a jar from outside of the apex`. A symlink `/apex/system -> /system` made it treat `system` as an APEX. Fix: remove the symlink.
- `forkSystemServer` died with `Not allowlisted: /data/local/tmp/.../sys/kernel/tracing/trace_marker`. Fix: `pivot_root` instead of `chroot`, so the paths look like `/sys/...`.
- Same error for the log file Zygote inherited as stdout. Fix: stdio to `/dev/null`, close all other descriptors.

## system_server

| Error | Cause | Fix |
| --- | --- | --- |
| `failed to set system property` | no `/dev/socket/property_service` | small property service in the loader |
| `Unable to stat ashmem device` | libcutils wants `/dev/ashmem<boot_id>` | create that node or a symlink |
| `Unable to parse: /data/apex/decompressed/...` | APEX packages are listed by the host's apex-info-list | expose the host `/system/apex`, `/system_ext/apex` |
| `idmap2d` crash on `/data/resource-cache` | directory missing on the fresh tmpfs | create it, owner system |
| `LineageSettingsProvider` cannot open database | `user_de/0/...` missing | pre-create the directories |
| NPE in `ICameraService.addListener` | vendor camera server answers an AOSP 16 client with a null | `config.disable_cameraservice=true`, stop the camera services for the session |
| `ParcelFormatException` in `IVold.needsCheckpoint` from `fstrim` | no `/data/system/last-fstrim`, so fstrim ran at boot | create the file with the current time |
| `PersistentDataBlockService init timeout` (phase 500) | `ro.frp.pst` points to a partition that does not exist in the guest | RAM backed fake FRP device |

## The storage wipe

With the host binder visible, the guest `StorageManagerService` called the host `vold` (`prepareUserStorage(0)`). `vold` had no keys from the guest and created `/data/media/0` from scratch.

Current countermeasures in `loader.c`:

- host binder nodes are never mounted into the guest
- a private binderfs, with a check that the new nodes do not share a device number with the host ones
- no host `/metadata`, no host sockets, no host `/data` binds
- every host bind mount is made read-only recursively, or the loader aborts
- PID, network, IPC and UTS namespaces, risky capabilities dropped, `no_new_privs`
- loop devices are checked: right major, right backing file, read-only for the system image, tmpfs for FRP

All of this was written without a second test run. Treat it as unverified.
