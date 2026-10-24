#!/usr/bin/env bash
# SDRNative
# Linux build
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DSDRNATIVE_REQUIRE_LIBUSB=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

echo
echo "build complete"
echo "probe:   $ROOT/build/sdrn_probe"
echo "DVB-T:   $ROOT/build/sdrn_dvbt_capture <freq_mhz> <6|7|8> <seconds> <output.ts>"
echo "IQ:      $ROOT/build/sdrn_iq_capture <freq_mhz> <sample_rate_hz> <seconds> <output.iq>"
