#!/usr/bin/env bash
# Host test for the Biscuit headless HWC (device/amazon/biscuit/hwcomposer).
# Needs the AOSP headers from a synced CM14.1 tree (or HWC_HEADERS/SYSCORE_HEADERS
# pointing at include dirs that provide hardware/*.h, cutils/native_handle.h and
# system/graphics.h). Skips when the headers or a C++ compiler are missing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CM14="${CM14:-$ROOT/workspace/cm14.1}"
HWC_HEADERS="${HWC_HEADERS:-$CM14/hardware/libhardware/include}"
SYSCORE_HEADERS="${SYSCORE_HEADERS:-$CM14/system/core/include}"
SRC="$ROOT/device/amazon/biscuit/hwcomposer/hwcomposer.cpp"
TEST="$ROOT/tests/hwc/host-test.cpp"
STUBS="$ROOT/tests/hwc/stubs"
CXX="${CXX:-g++}"

if ! command -v "$CXX" >/dev/null; then echo "SKIP: no C++ compiler ($CXX)"; exit 0; fi
if [[ ! -f "$HWC_HEADERS/hardware/hwcomposer.h" || ! -f "$SYSCORE_HEADERS/cutils/native_handle.h" || ! -f "$SYSCORE_HEADERS/system/graphics.h" ]]; then
  echo "SKIP: AOSP headers not found (set HWC_HEADERS and SYSCORE_HEADERS or sync CM14.1)"; exit 0
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
INC=(-I"$STUBS" -I"$HWC_HEADERS" -I"$SYSCORE_HEADERS")
WARN=(-std=gnu++11 -Wall -Werror -g -O1)

run_variant() {
  local name="$1"; shift
  echo "== $name =="
  "$CXX" "${WARN[@]}" "$@" -fPIC -shared "${INC[@]}" "$SRC" -o "$TMP/hwc-$name.so" -lpthread
  "$CXX" "${WARN[@]}" "$@" "${INC[@]}" "$TEST" -o "$TMP/test-$name" -ldl -lpthread
  "$TMP/test-$name" "$TMP/hwc-$name.so" 320x320
  PROP_debug_biscuit_hwc_size=1x1 "$TMP/test-$name" "$TMP/hwc-$name.so" 1x1
  PROP_debug_biscuit_hwc_size=240x320 "$TMP/test-$name" "$TMP/hwc-$name.so" 240x320
  # invalid values fall back to the default
  PROP_debug_biscuit_hwc_size=banana "$TMP/test-$name" "$TMP/hwc-$name.so" 320x320 2>/dev/null
  PROP_debug_biscuit_hwc_size=0x10 "$TMP/test-$name" "$TMP/hwc-$name.so" 320x320 2>/dev/null
  PROP_debug_biscuit_hwc_size=320x320x1 "$TMP/test-$name" "$TMP/hwc-$name.so" 320x320 2>/dev/null
}

can_sanitize() {
  echo 'int main(){}' | "$CXX" -x c++ - "$@" -o "$TMP/probe" 2>/dev/null && "$TMP/probe" 2>/dev/null
}

run_variant plain
if can_sanitize -fsanitize=address,undefined; then
  run_variant asan -fsanitize=address,undefined -fno-sanitize-recover=undefined
else echo "SKIP asan/ubsan: not supported by $CXX"; fi
if can_sanitize -fsanitize=thread; then
  run_variant tsan -fsanitize=thread
else echo "SKIP tsan: not supported by $CXX"; fi
echo "PASS test-hwc-host"
