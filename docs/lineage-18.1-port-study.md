# LineageOS 18.1 port study (full product)

Status: **back on the 32-bit 3.18 kernel (2026-10-16); see
[Decision](#decision-2026-10-16-back-to-the-32-bit-318-kernel).** The 4.9
port ([2026-10-14 decision](#decision-2026-10-14-linux-49-and-the-maintained-common-tree))
cannot boot: the amonet v2 boot chain does not start 64-bit kernels. The 3.18
kernel is a LineageOS kernel branch, and the 3.18 build boots to a working
full product; see [Status](#status-2026-10-17). The study scopes a port of the full `cm_biscuit-userdebug` product to
LineageOS 18.1 (Android 11) and defines the gates that must pass before the
expensive work (audio, Bluetooth, services) starts.

## Status (2026-10-17)

`lineage_biscuit-userdebug` on the `lineage-18.1-3.18` kernel boots on amonet
v2, completes Android 11 boot and stays within the Gate 1 memory budget.
Working:

- Wi-Fi (STA) and Bluetooth;
- speaker playback through the Fire OS 6 HAL with Amazon's audio processing
  (libasp with a `liblipc` stub, device volume at 0 dB), without the
  first-playback crackle;
- microphone capture through the TI ADC route with echo cancellation: plain
  `MIC` captures are opened as `VOICE_COMMUNICATION`, because AFE.cfg's Mic
  path is empty. Chimes, repeated wake words and "stop" during playback work
  in a kiosk voice satellite running as HOME;
- static AudioTracks such as intercom ring tones, after the kernel branch
  picked up upstream ashmem `97fbfef6bd59` (`FMODE_LSEEK`), without which
  AudioFlinger's `checkIMemory` rejected them;
- Magisk, installed from TWRP and preserved by addon.d across OTAs.

## Decision (2026-10-16): back to the 32-bit 3.18 kernel

The 4.9 build never got past LK: after power-on the ring stays steady blue
(set by LK), goes dark and the device re-enters the preloader (`0e8d:` on
USB), in a loop. Two fixes on the 4.9 boot image did not change that: a
ramdisk address inside the 512 MB of RAM (mt8163-common's `0x69244e00` lies
beyond it) and the MediaTek `KERNEL` mkimg header that stock kernels carry.
The diagnosis, from TWRP and the stock images:

- The installed `boot_a` (the active slot) was byte-identical to the build.
- amonet's TWRP runs a 32-bit 3.18 kernel (`armv7l`,
  `bootopt=64S3,32N2,32N2`), and so do Fire OS 6 and CM14. Its kernel has no
  mkimg header, so LK does not need one.
- The LK on the device is the Fire OS 6 LK (`lk_build_desc=63cb91b-20221007_072309`).
  It has a 64-bit path ("jump to K64") that hands over to ATF through an SMC;
  the ATF image in the Fire OS 6 `tee` partition is encrypted, so that path
  cannot be checked statically.
- Every kernel that reaches device probing starts the IS31FL3236
  `play-boot-animation` (TWRP's pulsing cyan); the 4.9 DTS has the same node,
  and the ring never left LK's blue. The 4.9 kernel therefore died at the
  handoff or in its first moments.
- A test boot image with the stock Fire OS 5 272.6.4.1 arm64 kernel (which
  boots on stock firmware), the LineageOS ramdisk and the same layout showed
  the same steady blue. The amonet v2 chain does not start 64-bit kernels at
  all, so no arm64 kernel, 4.9 or otherwise, can boot Biscuit.

The LineageOS 18.1 userspace on mt8163-common is 32-bit already
(`TARGET_ARCH := arm`), so only the kernel changes. It is now the
`lineage-18.1-3.18` branch of `android_kernel_amazon_biscuit`:

| Commit | Content |
| --- | --- |
| Import | `kernel/mediatek/mt8163/3.18_hl` from the 6.5.7.1 `platform.tar`, unmodified |
| `patches/kernel/` 010, 020, 030 | As in CM14; with them the tree is byte-identical to `workspace/cm14.1/kernel/amazon/biscuit` |
| verity-keys | Amazon's verity certificate in the source tree, read from `$(srctree)` (Fire OS and `scripts/build.sh` copy it into the object tree) |
| `mmput_async()` | Upstream `ec8d7c14ea14` as carried by AOSP android-3.18; the binder allocator needs it |
| Binder | The final AOSP `deprecated/android-3.18` binder (`839523fca2e`, 2021): `binder`/`hwbinder`/`vndbinder`, `BINDER_SET_CONTEXT_MGR_EXT`, the CVE-2019-2215 `POLLFREE` fix, task-based LSM hooks matching Fire OS 3.18. `ANDROID_BINDER_IPC_32BIT` gets a prompt and is unset |
| Slot exporter | The 4.9 branch's amonet v2 `misc+0x360` reader, appending `androidboot.slot_suffix` before init |
| taskstats, zram LZ4 | linux-stable v3.18.19 files missing from the Fire OS drop, enabled in `biscuit_defconfig` with `TASK_XACCT`, `TASK_IO_ACCOUNTING` and `ZRAM_LZ4_COMPRESS` |
| No modules | The four unused `=m` options (bridge netfilter, `physdev` match, two DVB tuners) are off: the drop has no `scripts/Makefile.modinst`, and LineageOS `kernel.mk` runs `modules_install` whenever `.config` matches `=m` |
| `androidboot.hardware` in the DT | `kernel.mk`'s bare `grep '=m'` also matched `hardware=mt8163` in `CONFIG_CMDLINE`, and with no modules its install step then fails (`cp` of an empty list). `biscuit.dtsi` now carries `/firmware/android` (`compatible = "android,firmware"`, `hardware = "mt8163"`), which Android 11 reads before the command line (fs_mgr and first-stage init) and imports as `ro.boot.hardware`; the forced cmdline no longer names it |

It builds with the LineageOS tree's `arm-linux-androideabi-4.9` GCC into a
`zImage-dtb` whose `evt`/`min_evt` DTBs match TWRP's kernel. The device tree
switches to the Fire OS 6 layout (kernel `0x40008000`, ramdisk `0x44000000`,
`bootopt=64S3,32N2,32N2`), uses the built-in connsys (no `mt66xx` modules,
same `wmt_loader`/`wmt_launcher`), the 3.18 sysfs paths, the `android_usb`
gadget instead of configfs, and SwiftShader GLES on `gralloc.default`
because the kernel has no GPU driver.

The branch carries the recommended taskstats and zram LZ4 restorations of
the earlier `patches/kernel-los18/` series (051, 052), with the config
symbols set in `biscuit_defconfig` itself instead of a fragment. The rest of
that series is superseded: the branch takes binder from android-3.18 instead of
`deprecated/android-4.9-o`, and LineageOS needs `merge_config.sh` (050) only
for a `TARGET_KERNEL_ADDITIONAL_CONFIG` fragment, which the branch does not
use.

## Decision (2026-10-14): Linux 4.9 and the maintained common tree

Superseded on 2026-10-16: the kernel does not boot (see above). The
common tree, device and vendor trees stay; the kernel is back on 3.18.

Gate 1 was skipped at the user's request. The port now targets the
maintained amazon-oss `lineage-18.1` common tree and its Linux 4.9.337
kernel instead of the stale 3.18 `legacy/lineage-18.1` tree. This became
practical because the 4.9 kernel gained `rook` (Echo Spot, the same
TLV320 ADC3101/AIC32x4 audio parts and MT6323 PMIC as Biscuit), while
douglas/karnak already run the MT8163 connsys through the out-of-tree
`mt66xx` modules.

The result lives in three standalone LineageOS trees next to this repo
(branch `lineage-18.1`, published under
[github.com/meiser79](https://github.com/meiser79); see the README):

| Tree | Content |
| --- | --- |
| `android_kernel_amazon_biscuit` | amazon-oss kernel plus Biscuit commits: `biscuit_defconfig`, Biscuit DTS (from the 3.18 source), IS31FL3236 LED driver, thermal table, 8-mic FPGA firmware, two dormant build fixes (8-mic `dough.h`, BT DAI), the forced cmdline with safe LK arguments, and an amonet v2 slot export (`misc+0x360` to `androidboot.slot_suffix`) that replaces the CM14 `fs_mgr` patch |
| `android_device_amazon_biscuit` | Device tree on `mt8163-common`: connsys Wi-Fi/BT, headless HWC 1.1, slotselect fstab, OTAs restricted to `current-{system,boot}`, TWRP protected from `install-recovery.sh`; its README has the local manifest |
| `android_vendor_amazon_biscuit` | Generated by the device tree's `extract-files.sh` from the 6.5.7.4 blobs (connsys loader, launcher, firmware) |

Verified on the host: the kernel and the `mt66xx` modules build with the
`mt8163/android_prebuilts_linaro` aarch64 GCC, and host tests pass for the
headless HWC (Android 11 headers), the bootargs allowlist, the amonet slot
parser and the OTA write-target checks. Still open on hardware: LK booting
an arm64 kernel, Mali (disabled by Fire OS on Biscuit), the Fire OS 6
connsys userspace with the newer modules, and the `system.img` size.

Every CM14 full-product feature is ported (see the device README): audio
with microphone AGC, the LED daemon, BiscuitService, the Bluetooth sink,
`amz_priv`, the ambient light sensor (TSL2584TSV/TSL2540 drivers plus
`sensors.biscuit`), KSM and the debug interfaces. Left out on purpose: the
Amazon AEC preload, the audio-debug probes and the mDNS hostname patch. As on Fire OS and CM14, the kernel owns
microphone privacy: the 4.9 keypad driver hands the PMIC power key (the
microphone button) to `amz_priv`, which mutes GPIO87 (global GPIO 444)
300 ms after a release and unmutes on the next one, independent of Android.
Software can enter privacy through `privacy_trigger` but never leave it; the
HAL wrapper refuses an unmute while `privacy_state` is 1, and
`biscuit-privacyd` mirrors `privacy_state` into Android's microphone mute
flag. GPIO88 (`bright-state-gpio`) is exported but unused, and mute is not
shown on the LED ring (CM14 did not show it either).

## Verdict

A LineageOS 18.1 full product for Biscuit is **technically feasible but
high-risk**, and the risk sits in two places that cannot be resolved on paper:

1. **RAM.** Biscuit has 512 MB LPDDR3 (Micron MCP, 4 Gbit DRAM + 4 GB eMMC).
   Every active LineageOS 18.1 MT8163 target has at least 1 GB. Android 11 with
   `system_server`, SystemUI, Bluetooth, audioserver and the Amazon audio
   closure may not stay resident in 512 MB even with `ro.config.low_ram`,
   zram and Go defaults.
2. **Headless software GLES.** Android 11 removed libagl (`libGLES_android`);
   SurfaceFlinger's RenderEngine and HWUI need a real EGL/GLES 2 driver. This
   branch already runs SwiftShader GLES (android-8.1.0_r45 snapshot, Subzero
   ARM32 JIT) on Biscuit under CM14, which de-risks the approach; what is
   still open is which SwiftShader revision builds and runs inside the 18.1
   tree.

Everything else has a working precedent on the same SoC in the
`amazon-oss` LineageOS 18.1 trees (see below), which turns most of the port
into adaptation rather than invention.

Recommendation: proceed **only** through Gate 0 and Gate 1 (kernel + boot to
ADB with memory measurement). Do not port audio, Bluetooth or BiscuitService
before `sys.boot_completed=1` is reached with acceptable free memory.

## Precedents found in amazon-oss (checked 2026-10-14)

| Repository / branch | Commit | What it proves |
| --- | --- | --- |
| [`android_device_amazon_mt8163-common` `legacy/lineage-18.1`](https://github.com/amazon-oss/android_device_amazon_mt8163-common/tree/legacy/lineage-18.1) | `04adbe4` | Android 11 on MT8163 with the **3.18** kernel and pre-Treble (Fire OS 5, Android 5.1) blobs: audio 2.0 HIDL over a legacy HAL wrapper, composer 2.1, bluetooth 1.0 + `libbt-vendor`, sepolicy with `SELINUX_IGNORE_NEVERALLOWS`, ABI shims. |
| [`android_kernel_amazon_mt8163` `legacy/lineage-18.1`](https://github.com/amazon-oss/android_kernel_amazon_mt8163/tree/legacy/lineage-18.1) | `b13c1b0` | Linux 3.18.140 MT8163 kernel running Android 11. Its Android-11-specific delta is small (29 commits since 2022, listed in [Kernel](#kernel)). |
| [`android_device_amazon_mt8163-common` `lineage-18.1`](https://github.com/amazon-oss/android_device_amazon_mt8163-common/tree/lineage-18.1) | `1cbe667` | Actively maintained (Sept 2026) MT8163 Android 11 common tree with **32-bit userspace** (`TARGET_ARCH := arm`), Go optimisations, low-RAM properties. Uses the 4.9 kernel and Fire OS 7 blobs. |
| [`android_device_amazon_cronos` / `checkers` / `crown` `lineage-18.1`](https://github.com/amazon-oss/android_device_amazon_cronos/tree/lineage-18.1) | `0abbd82` (cronos) | Echo Show devices: A2DP sink + AVRCP controller configuration via `bdroid_buildcfg.h` and a Bluetooth overlay. |
| [`android_hardware_amazon` `lineage-18.1`](https://github.com/amazon-oss/android_hardware_amazon/tree/lineage-18.1) | `5dca03c` | `audio.primary.amazon_wrapper` for Amazon legacy audio HALs including **ASP capture processing** (`asp.cpp`), `sensors.amazon` for Echo devices, `libamazonlog`, ABI shims (`libbinder`, `libutils`, `libcutils`, `libtinyalsa`, ...), Amazon power HAL. |
| [`android_hardware_mediatek_mt66xx` `lineage-18.1`](https://github.com/amazon-oss/android_hardware_mediatek_mt66xx/tree/lineage-18.1) | `b3a5b24` | Open-source MT66xx connsys `wifi_hal`, `lib_driver_cmd_mtk` supplicant library and `libbt-vendor`. Biscuit's radio is an MT6625LN on the MT8163 connsys (`WIFI_RAM_CODE_8163`, `wmt_loader`), i.e. the same family. |

Not usable directly: the 4.9 kernel (`android_kernel_amazon_mt8163`
`lineage-18.1`, Linux 4.9.337) has defconfigs for `checkers`, `cronos`,
`crown`, `douglas` and `karnak` only. Moving Biscuit to 4.9 would require
porting its board support (TI DAC3203 + TLV320ADC3101 audio path, LED/button
MCU, GPIO87 mic mute, device tree) out of Amazon's 3.18 source, and the Fire
OS 6 audio HAL is bound to the 3.18 ALSA control layout. That is a larger and
less certain project than keeping 3.18; it is listed under
[Alternatives](#alternatives).

## Chosen architecture

Keep what CM14 already proved on the device and change only what Android 11
forces:

| Area | CM14.1 today | LineageOS 18.1 plan | Basis |
| --- | --- | --- | --- |
| Kernel | Fire OS 6.5.7.1 Linux 3.18.19, `ARCH=arm`, `zImage-dtb` | Same source, plus Android 11 backports (binder, ABI, config) | `android_kernel_amazon_biscuit` `lineage-18.1-3.18` |
| Userspace ABI | 32-bit, `zygote32` | 32-bit (`TARGET_ARCH := arm`, armv8-a, cortex-a53), `zygote32` | main 18.1 common |
| Blobs | Fire OS 6.5.7.4 (Android 7.1.2) | Same blobs, re-shimmed for API 30; Wi-Fi/BT userspace replaced by mt66xx sources | legacy 18.1 shims |
| Partitions | `boot`/`system` A/B via Amonet v2, no vendor partition | Unchanged; `TARGET_COPY_OUT_VENDOR := system/vendor` | legacy 18.1 |
| Boot flow | `root=/dev/ram rdinit=/init`, CM14 init | Same kernel cmdline; Android 11 first-stage init in ramdisk mounts `system` and switches root | Android 10+ legacy-device flow |
| HALs | Legacy `hw_get_module` | HIDL passthrough/binderized wrappers around the legacy modules | legacy 18.1 manifest |
| OTA | Custom releasetools to `current-system`/`current-boot` | Port the extension to Android 11 releasetools (Python 3), same contract | this repo |
| SELinux | Permissive | Permissive, policy compiled with `SELINUX_IGNORE_NEVERALLOWS` | legacy 18.1 |

## Gates

### Gate 0 — kernel config audit (hours, no build)

The audit ran with `scripts/audit-kernel-los18.sh`
against the staged `biscuit_defconfig` (or the running device's
`/proc/config.gz` if exported). It reports the options listed under
[Kernel](#kernel). This could not be done while writing this study because the
Fire OS source archive was not reachable from the study environment.

Pass: every **required** item is present or has a known backport source.

Result from the device's `/proc/config.gz` (CM14 full image, 2026-10-14):

| Item | State | Action |
| --- | --- | --- |
| `ANDROID_BINDER_IPC_32BIT` | `y` (promptless in Fire OS) | `040` gives it a prompt; fragment unsets it |
| `ANDROID_BINDER_DEVICES` | absent | `040`: binder from `deprecated/android-4.9-o`; fragment sets `binder,hwbinder,vndbinder` |
| `NETFILTER_XT_MATCH_OWNER` | unset | Not needed: `NETFILTER_XT_MATCH_QTAGUID=y` registers the `owner` match, and the device's `/proc/net/ip_tables_matches` lists `owner` |
| `TASKSTATS`, `TASK_IO_ACCOUNTING` (recommended) | unset / absent | Fragment (`TASK_XACCT` + `TASK_IO_ACCOUNTING`); `051` restores `kernel/taskstats.c` and `kernel/tsacct.c`, missing from the Fire OS drop |
| `UID_SYS_STATS` (recommended) | absent | Optional backport; storaged degrades without it |
| `SDCARD_FS` (recommended) | absent | Skip; Android 11 uses FUSE |
| zram LZ4 (recommended) | `ZRAM_LZ4_COMPRESS` unset | Fragment; `052` restores `zcomp_lz4.[ch]` and `lib/lz4/lz4_compress.c`, missing from the Fire OS drop (3.18 zram does not use `CRYPTO_LZ4`) |
| SECCOMP filter, SELinux develop, ashmem, ION, initrd, cgroups, FUSE, cpusets, tracing, ZRAM, LMK, quota, qtaguid | present | — |

Gate 0 passes. Binder is the only functional code change. Two more code
changes are restorations: Amazon's GPL drop omits sources its own config does
not build (`taskstats`, zram LZ4, and `scripts/kconfig/merge_config.sh`, which
LineageOS 18.1 `kernel.mk` calls for `TARGET_KERNEL_ADDITIONAL_CONFIG`). The
config changes live in the fragment `arch/arm/configs/biscuit_los18.config`,
so `biscuit_defconfig` itself stays untouched. (Earlier revisions of the audit
script flagged `BLK_DEV_RAM` and `DEVTMPFS` — false positives, the boot ramdisk
is an initramfs and Android 11 first-stage init mounts a tmpfs on `/dev` — and
stopped at the first recommended miss.)

Compile result (2026-10-14, `scripts/build-kernel-los18.sh`, `arm-eabi-4.8`):
`zImage-dtb` and modules build, with no warnings in the binder, zram, LZ4 or
taskstats sources. `scripts/audit-kernel-los18.sh` on the resulting `.config`
reports *Gate 0: all required items present*; only `UID_SYS_STATS` and
`SDCARD_FS` (recommended) remain. Against a plain `biscuit_defconfig` the
merged `.config` differs only in the fragment symbols plus `LZ4_COMPRESS`
(selected) and `TASK_DELAY_ACCT` (newly visible, unset).

#### Kernel series (`patches/kernel-los18/`)

Superseded by the `lineage-18.1-3.18` kernel branch (see the 2026-10-16
decision), which carries 051 and 052 and takes binder from AOSP
`deprecated/android-3.18`. The series (`040` binder from
`deprecated/android-4.9-o`, `050` `merge_config.sh`, `051` taskstats, `052`
zram LZ4, `060` config fragment) and its `scripts/build-kernel-los18.sh` and
`scripts/audit-kernel-los18.sh` were removed from this repository; they
remain in commit `70f0c56`.

### Gate 1 — boot to ADB with memory budget (first real milestone)

Product: `lineage_biscuit-userdebug` with framework, headless graphics and
nothing else (no audio, no Bluetooth, no BiscuitService).

Pass criteria:

- `adb shell getprop sys.boot_completed` returns `1` on Amonet v2, both slots.
- `adb shell cat /proc/meminfo` after 10 minutes idle: `MemAvailable` ≥ 120 MB
  with zram active, and no LMK kills of persistent processes in `logcat -b events`.
- `system.img` fits the 768 MiB `system` partition.

Fail on RAM: stop the full-product port; see [Alternatives](#alternatives).

Measure the CM14 full image the same way before starting, so the comparison is
on identical hardware state.

### Gate 2 — radio

Wi-Fi STA connects via the framework and survives reboot; Bluetooth enables.

### Gate 3 — audio and speaker role

Speaker playback, A2DP sink, microphone capture with mute, LED ring.

## Kernel

Required changes on top of Fire OS 6.5.7.1 (3.18.19):

| Item | Why | Source |
| --- | --- | --- |
| `CONFIG_ANDROID_BINDER_IPC_32BIT` **unset** | Android 9+ requires the 64-bit binder ABI; on a 32-bit 3.18 kernel this option defaults to `y`. Flip it and build userspace with the 64-bit ABI (`TARGET_USES_64_BIT_BINDER := true`). | AOSP libbinder history: the option has been required unset since Android P |
| Binder with `hwbinder`/`vndbinder` devices (`CONFIG_ANDROID_BINDER_DEVICES`) | HIDL needs `/dev/hwbinder`; vendor services need `/dev/vndbinder`. | **Done** on the kernel branch (AOSP `deprecated/android-3.18`); earlier the removed `patches/kernel-los18/040` (AOSP `deprecated/android-4.9-o` head; legacy kernel `a42a1fb`, `c2a7dd7`, `af0ad75` as reference) |
| An iptables `owner` match | netd uid firewall chains. On 3.18 `xt_qtaguid` provides it (confirmed: the device lists `owner` in `/proc/net/ip_tables_matches`); `CONFIG_NETFILTER_XT_MATCH_OWNER` is the ≥4.9 way and is not needed. | upstream 3.18 |
| `CONFIG_SECCOMP` + `CONFIG_SECCOMP_FILTER` | zygote, mediacodec and mediaextractor install seccomp filters. ARM 3.18 supports it; verify it is enabled. | upstream |
| `CONFIG_SECURITY_SELINUX_DEVELOP` | Keeps `androidboot.selinux=permissive` effective. | legacy kernel `6c075ad` |
| Firmware and module paths under `/vendor` | Connsys firmware moves to `/system/vendor/firmware`, reachable through `/vendor`. | legacy kernel `b13c1b0`; `firmware_class.path=/vendor/firmware` |
| Keep `020-force-ramdisk-root` and `030-filter-bootloader-cmdline` | Android 11 first-stage init still starts from the ramdisk. | this repo |

Recommended (reduce userspace workarounds):

| Item | Source |
| --- | --- |
| `CONFIG_TASKSTATS`, `CONFIG_TASK_IO_ACCOUNTING` | **Done** on the kernel branch (linux-stable v3.18.19 restoration, set in `biscuit_defconfig`) |
| uid_sys_stats, cpufreq uid stats | legacy kernel `84b75d1` and the uid/cpufreq series |
| tracefs | legacy kernel tracing series (`1e5f20c`, `72a29b1`, ...) |
| sdcardfs (optional on Android 11, which uses FUSE) | legacy kernel `13a071f` and series |
| ION use-after-free fixes | legacy kernel `92b35dc`, `3e24850` |
| ZRAM with LZ4 | **Done** on the kernel branch (linux-stable v3.18.19 restoration, set in `biscuit_defconfig`; legacy kernel `27ccc30`, `89c75aa`, `b227c1c` for reference) |
| Merge linux-stable 3.18.y up to 3.18.140 | The legacy tree is at 3.18.140; Biscuit is at 3.18.19. Optional, but it reduces backport conflicts. |

The legacy kernel is arm64; its patches touching `arch/arm64` must be adapted
to `arch/arm`. Binder, tracefs, sdcardfs, ION and zram patches are
architecture-independent.

## Userspace work by area

### Boot, partitions and OTA

- First-stage ramdisk fstab (`fstab.mt8163` in `$(TARGET_COPY_OUT_RAMDISK)`) with
  `system` `slotselect,first_stage_mount`, keeping the
  `/dev/block/platform/bootdevice/by-name` contract from `001-amonet-fstab`.
- `030-filter-bootloader-cmdline` removes `androidboot.slot_suffix`, so
  first-stage init needs the Amonet v2 slot from elsewhere. **Done** in the
  kernel instead of `fs_mgr`: the slot exporter reads the BCB at `misc+0x360`
  (CM14's `002-amonet2-bcb-slotselect` rules) once eMMC is up and appends
  `androidboot.slot_suffix` to `/proc/cmdline` before init runs. Test it with
  both slots.
- Port `device/amazon/biscuit/releasetools.py` to Android 11 releasetools
  (Python 3). The generated updater must still write only
  `/dev/block/current-system` and `/dev/block/current-boot`;
  `tests/test-ota-slot-paths.py` carries over.
- Non-A/B block OTA (`AB_OTA_UPDATER` unset) remains supported in 18.1.
- A clean `userdata` format is required when moving from CM14.

### Headless graphics (Gate 1 risk)

- Composer: `android.hardware.graphics.composer@2.1-service`. The LineageOS
  18.1 passthrough loader wraps HWC1 modules with `HWC2On1Adapter`
  (`hardware/interfaces/graphics/composer/2.1/utils/passthrough/.../HwcLoader.h`),
  but it **rejects HWC 1.0** (`"hwcomposer 1.0 is not supported"`). The stock
  `hwcomposer.mt8163.so` from 272.6.4.1 declares API 1.0 and therefore cannot
  be used. The source-built headless HWC 1.1 in
  `device/amazon/biscuit/hwcomposer` (software vsync, no fbdev, size from
  `debug.biscuit.hwc.size`) is exactly what the adapter needs and should carry
  over with only build-file changes. `020-headless-hwc1-fake-display` should
  then become unnecessary; verify.
- Allocator/mapper: `android.hardware.graphics.allocator@2.0` and
  `mapper@2.0` passthrough over `gralloc.default` (as CM14 does today).
- EGL/GLES — candidates in order (**chosen: 1**, with `gralloc.default`,
  because the 3.18 kernel has no GPU driver):
  1. SwiftShader GLES from the 18.1 manifest's `external/swiftshader`, loaded
     with `ro.hardware.egl=swiftshader`. Confirm it still builds
     `libEGL_swiftshader`/`libGLESv2_swiftshader` for `arm` and that its JIT
     backend runs on ARM32 (CM14 needed the 8.1 snapshot because the 7.1
     revision could not run on ARM).
  2. Carry the proven android-8.1.0_r45 SwiftShader snapshot into the 18.1
     tree (it builds from `Android.mk`, which 18.1 still accepts). Android 11
     no longer has libagl to fall back to, so the `017` loader patch reduces to
     the property.
  3. Mali-T720 userspace from another MT8163 product (the SoC has the GPU;
     Fire OS 6 Biscuit lacks the userspace and the kernel display/GPU parts).
  4. ANGLE on SwiftShader Vulkan — heaviest; last resort.
- Keep `config_avoidGfxAccel`, `ro.config.no_gpu`-style framework guards only
  where Android 11 still crashes without them (`018`/`019` are expected to
  shrink or disappear).

### Shims and blob ABI

Start from `hardware/amazon/libshims` 18.1 (`libbinder`, `libutils`,
`libcutils`, `libtinyalsa`, `libui`, `libgui`, `libcrypto`, `libssl`) and
`libamazonlog`, which replaces `004-amazon-log-shim`. Re-derive the needed set
with `readelf -d` / undefined-symbol scans of the Fire OS 6 closure against the
18.1 build, and extend the shims (`005-audio-legacy-symbols` content) as needed.
Fire OS 6 is Android 7.1, so the gap is smaller than the Fire OS 5 gap that the
legacy tree already bridged.

### Wi-Fi

- Replace Fire OS Wi-Fi userspace with `hardware/mediatek/mt66xx` (`wifi_hal`,
  `lib_driver_cmd_mtk`); keep only `wmt_loader`, `wmt_launcher` and the
  connsys firmware from `biscuit-radio-files.txt`.
- `android.hardware.wifi@1.x` service + supplicant HIDL + wificond.
- `008`, `022` and `024` become device configuration: `wifi.interface`, a
  STA-only overlay, and simply not shipping
  `android.hardware.wifi.direct.xml`.

### Bluetooth

- `android.hardware.bluetooth@1.0-impl` with the mt66xx `libbt-vendor`.
- A2DP sink / AVRCP controller exactly as cronos does it:
  `bdroid_buildcfg.h` (`BTA_AV_SINK_INCLUDED`, `BTA_AR_INCLUDED`) plus a
  Bluetooth app overlay that disables source profiles and PAN (replaces `013`).
- Re-port the no-input pairing behaviour (`009`–`012`) to `system/bt` 18.1.

### Audio

- Use `audio.primary.amazon_wrapper` from `hardware/amazon` 18.1 with
  `android.hardware.audio@2.0-impl`/`effect@2.0-impl` (the same versions the
  Echo Show trees run).
- Re-port the Biscuit-specific parts of `025`–`028` into that wrapper (or a
  thin Biscuit layer on top): TLV320 speaker route, external amplifier
  control, GPIO87 hardware mic mute.
- Evaluate the upstream ASP capture path (`asp.cpp`) against Biscuit's
  `libasp.so` before writing anything new.
- `016-biscuit-flac-decoder` is obsolete: Android 11 ships the Codec2 FLAC
  decoder.

### Biscuit services

- `biscuit-ledd`, `biscuit-ledctl`, `i2c-poke`: native, should rebuild with
  minor fixes.
- `BiscuitService` and `BiscuitEmptyLauncher`: rebuild against API 30;
  re-check background-service, broadcast and hidden-API restrictions.
- `023-biscuit-mic-mute`, `014-insecure-adb-default-props`,
  `015-biscuit-hostname-mdns`: re-port; Android 11 has no framework mic
  privacy toggle to reuse.
- `sensors.mt8163` → evaluate `sensors.amazon` (`sensors_echo`) from
  `hardware/amazon` 18.1.

### Size and memory

- Inherit the 18.1 common Go profile (`go_opt.mk`), `ro.config.low_ram=true`,
  `MALLOC_SVELTE`, in-process network stack and tethering, zram at 50 % of RAM.
- Remove every UI application (done through the empty launcher's
  `overrides`) and build only `en_US`/`de_DE` resources. WebView stays: kiosk
  apps need it. Restrict dexpreopt to the boot image and `system_server` only
  if `system.img` still exceeds 768 MiB.

## Patch disposition

| CM14 full patch | LineageOS 18.1 |
| --- | --- |
| 001 amonet fstab | Device fstab (ramdisk + vendor), no patch |
| 002 BCB slotselect | Re-port to `fs_mgr` + first-stage `misc` node |
| 003 skip unused OTA images | Re-check; possibly board variables only |
| 004 Amazon log shim | Replaced by `libamazonlog` |
| 005 audio legacy symbols | Folded into `hardware/amazon` shims |
| 006, 007 headless props | Device properties |
| 008 Wi-Fi interface | Device property |
| 009–012 Bluetooth headless/no-input | Re-port to `system/bt` + `bdroid_buildcfg.h` |
| 013 disable PAN | Overlay only |
| 014 insecure ADB | Re-port (small) |
| 015 hostname/mDNS | Re-port |
| 016 FLAC decoder | Drop (Codec2) |
| 017 software EGL fallback | SwiftShader via `ro.hardware.egl`; libagl fallback gone |
| 018, 019 HWUI | Re-evaluate; likely smaller |
| 020 headless HWC1 | Likely obsolete with the source-built HWC 1.1 under `HWC2On1Adapter` |
| fork: SwiftShader manifest pin | Re-test with the 18.1 revision first |
| fork: headless HWC 1.1 | Keep (required: the adapter rejects HWC 1.0) |
| fork: `WITH_SU` | 18.1 `system/extras/su` is the AOSP debug `su` (root/shell only); the 14.1 `persist.sys.root_access` + per-app SU appops path is gone |
| 021 radio launchers | Device init `.rc` + mt66xx |
| 022 STA-only Wi-Fi | Overlays and supplicant config |
| 023 mic mute | Re-port |
| 024 disable P2P | Omit the permission file |
| 025–028 audio | Re-port onto `amazon_wrapper` |
| kernel 010/020/030 | Keep |

Roughly a third of the series disappears, a third becomes configuration and
a third must be rewritten against Android 11 sources.

## Build infrastructure

- LineageOS 18.1 does not use Jack; the Ubuntu 20.04 build image can stay
  but drops OpenJDK 8/legacy TLS. Expect roughly 200–250 GB for checkout plus
  output and ccache, and at least 16 GB RAM (with swap) for the build host.
- `manifest/` gains an 18.1 local manifest pinning `hardware/amazon` and
  `hardware/mediatek/mt66xx` (`lineage-18.1`) and, for reference only, the
  `legacy/lineage-18.1` common tree.
- The CM14 and 18.1 products should live on separate branches; they share
  only `device/amazon/biscuit` native sources, blob lists and tests.

## Alternatives

1. **Biscuit on the 4.9 kernel.** Use the maintained `lineage-18.1` common tree
   and Fire OS 7-era userspace. Requires porting Biscuit's board support to
   4.9 and probably replacing the Fire OS 6 audio stack. Larger, but ends on
   the maintained branch.
2. **Minimal product first.** Port the framework-free `biscuit_minimal`
   product to 18.1 userspace instead. It avoids the RAM and GLES risks but
   gains little over the CM14 minimal image.
3. **Stay on CM14.1** for the full product if Gate 1 fails on memory.

## Sources

- Echo Dot 2nd gen teardown, 512 MB LPDDR3 + 4 GB eMMC: <https://www.ednasia.com/?p=8119>
- Echo Dot 2nd gen teardown, MT6625LN, TI DAC3203, TLV320ADC3101: <https://www.allaboutcircuits.com/news/teardown-tuesday-amazon-echo-dot-v2/>
- libbinder 32-bit ABI removal (option required unset since Android P): <https://gerrit.omnirom.org/plugins/gitiles/android_frameworks_native/+/68275d7ca0951a6e46563f2578a8098113a32dc7%5E%21>
- amazon-oss repositories and commits listed in [Precedents](#precedents-found-in-amazon-oss-checked-2026-10-14).
- LineageOS 18.1 `hardware/interfaces` composer 2.1 passthrough (`HWC2On1Adapter`): <https://github.com/LineageOS/android_hardware_interfaces/tree/lineage-18.1/graphics/composer/2.1/utils/passthrough>
