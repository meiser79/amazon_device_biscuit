# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

@AGENTS.md

`AGENTS.md` (imported above) holds the binding agent workflow, device-safety, flashing, and `biscuit_service` rules. This file adds only the build/test commands and the architecture you need to read several files to understand.

## What this repo is

A CM14.1 (LineageOS 14.1, Android 7.1) port for the Amazon Echo Dot 2nd gen ("Biscuit", MT8163, 512 MB RAM, no display). The repo holds no Android tree. Instead, it has tracked *inputs* (manifests, patch series, device/vendor overlay, scripts). These regenerate a disposable, git-ignored `workspace/`, which contains `workspace/cm14.1` (the repo checkout), downloads, extracted Fire OS images, ccache and build output.

Two products are built from the same tree:

| Product | Make target | Patch profile |
| --- | --- | --- |
| `cm_biscuit-userdebug` | `make full` (alias `make build`) | `patches/full/` |
| `biscuit_minimal-userdebug` | `make minimal` | `patches/minimal/` (also sets `CLEAN_BISCUIT_OUT=1`) |

Per `AGENTS.md`, the full product has priority. Minimal-product work waits until the full product is stable.

## Commands

```sh
scripts/sync.sh          # repo sync of workspace/cm14.1 (needs git-lfs); writes manifest/lock.xml on first success
make stage               # stage overlay + full patches + blobs + kernel, no build
make stage-minimal       # same for the minimal profile
make full | make minimal # stage, then launch detached Docker build container
docker logs -f cm14.1-biscuit-build
make test                # all static tests
git diff --check
```

Run a single test directly, e.g. `bash tests/test-compatibility.sh` or `python3 tests/test-ota-slot-paths.py`. `tests/test-hwc-host.sh` compiles `device/amazon/biscuit/hwcomposer/hwcomposer.cpp` on the host in plain, ASan/UBSan, and TSan variants. It needs AOSP headers from a synced `workspace/cm14.1` (or `HWC_HEADERS`/`SYSCORE_HEADERS`) and prints `SKIP` when they are missing.

Useful `scripts/build.sh` env overrides: `BUILD_TARGET` (default `otapackage`), `BUILD_JOBS`, `CM14`, `CCACHE_DIR`. Output goes to `workspace/cm14.1/out-docker/target/product/biscuit/` (`out` is symlinked to `out-docker`). The Docker images `cm14.1-ubuntu20:latest` and `biscuit-kernel-builder:latest` are built from `docker/` (see README).

## Build pipeline (how the pieces connect)

`scripts/build.sh` → `scripts/stage-tree.sh` → Docker `lunch` + `make otapackage`. `stage-tree.sh` does, in order:

1. Downloads and sha256-verifies the Fire OS 6.5.7.1 Echo Dot source archive into `workspace/downloads` and `workspace/upstream`.
2. `rsync --delete` copies `device/amazon/biscuit` and `vendor/amazon` into `workspace/cm14.1`. Edits made directly in the workspace copy are lost, so always edit the tracked copies.
3. Removes a legacy radio block from the synced `mt8163-common` `init.mt8163.rc` (inline Python).
4. **Profile switching:** records the active profile in `workspace/cm14.1/.repo/biscuit-patch-state/`. On a profile change or a changed patch-set hash, it reverse-applies both profiles and restores (via `git checkout`) or deletes every file touched by any `+++ b/` path in either series. Then it reapplies the selected profile.
5. `scripts/apply-patches.sh <root> <strip> <dir>` applies every `*.patch` in lexical order with `--fuzz=0`. It is idempotent and skips when the directory's sha256 manifest is unchanged. Product patches use `-p1` against `workspace/cm14.1`.
6. `scripts/extract-fireos6-blobs.sh` extracts proprietary blobs from verified Fire OS images. The file lists come from `vendor/amazon/mt8163-common/*-files.txt` (audio from the 6.5.7.4 OTA, headless HWC closure from stock 272.6.4.1). It also generates the vendor makefiles from the tracked templates.
7. Extracts the Fire OS 3.18 kernel (`kernel/mediatek/mt8163/3.18_hl`) into `workspace/cm14.1/kernel/amazon/biscuit` and applies `patches/kernel/` at `-p4`.

### Patch series conventions

- Numbering is part of the build contract. Shared patches are **duplicated** in `patches/full/` and `patches/minimal/` on purpose, so each profile reproduces on its own.
- `tests/test-compatibility.sh` pins the exact count and filenames of every series (full, minimal, kernel). When you add, remove, or rename a patch, update that test and the patch tables in `docs/building-cm14.1-images.md`.
- Any change to `workspace/cm14.1` must end up as a patch or a script change (see `AGENTS.md`).

## Device overlay highlights (`device/amazon/biscuit`)

- `cm_biscuit.mk`/`device.mk` define the full product. `biscuit_minimal.mk`/`biscuit_minimal_device.mk` define the framework-free product. Both build on `device/amazon/mt8163-common` (synced from amazon-oss and pinned in `manifest/local.xml`).
- `releasetools.py`, together with patches `001`/`002`, implements the amonet v2 A/B contract. Runtime fstab uses `slotselect`. OTA edify writes only TWRP's `/dev/block/current-{system,boot}`. `tests/test-ota-slot-paths.py` and `tests/test-amonet2-bcb-slotselect.sh` guard it.
- `hwcomposer/` is a custom headless HWC 1.1 (no panel, no framebuffer driver). It is selected via `ro.hardware.hwcomposer=biscuit`, and the display size is set by `debug.biscuit.hwc.size`. SwiftShader (pinned in `manifest/local.xml`) provides software GLES.
- `biscuit-service/` contains the Java system service behind `/system/bin/biscuit_service`, the native LED daemon `biscuit-ledd`/`biscuit-ledctl` with `animations/`, and `i2c-poke`. `tests/test-led-countdown.sh` host-tests the LED countdown logic.
- `amznaec/` holds the Amazon AEC preload with a bundled speexdsp. `audio/` and `audio-debug/` hold the audio route wrapper and its debug tools (they pair with patches `025`–`029`).

## Reference docs

- `docs/building-cm14.1-images.md`: the per-patch explanation, artifacts, and the OTA preflight checklist.
- `docs/amonet-biscuit-unlock.md`: the amonet v2 / TWRP contract and the two-install A/B procedure.
- `docs/sources.md`: input URLs, hashes, and sourcing policy.
- `docs/lineage-18.1-port-study.md`: the LineageOS 18.1 study, status and decision record. The port itself lives in standalone sibling trees (`android_{device,kernel,vendor}_amazon_biscuit`, branches `lineage-18.1` and kernel `lineage-18.1-3.18`, the 32-bit Fire OS 6 3.18 kernel on amazon-oss `mt8163-common`); nothing in this repository builds it.
