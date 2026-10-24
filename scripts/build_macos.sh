#!/usr/bin/env bash
# SDRNative
# macOS build
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

for cmd in cmake cc; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "error: '$cmd' is required" >&2
    exit 2
  fi
done
if ! command -v pkg-config >/dev/null 2>&1; then
  echo "error: pkg-config is required (brew install pkg-config)" >&2
  exit 2
fi
if ! pkg-config --exists libusb-1.0; then
  echo "error: libusb-1.0 not found (brew install libusb)" >&2
  exit 2
fi

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=ON \
  -DSDRNATIVE_REQUIRE_LIBUSB=ON \
  -DSDRNATIVE_BUILD_TESTS=ON \
  -DSDRNATIVE_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

echo
echo "build complete"
echo "probe:   $ROOT/build/sdrn_probe"
echo "DVB-T:   $ROOT/build/sdrn_dvbt_capture <freq_mhz> <6|7|8> <seconds> <output.ts>"
echo "IQ:      $ROOT/build/sdrn_iq_capture <freq_mhz> <sample_rate_hz> <seconds> <output.iq>"
