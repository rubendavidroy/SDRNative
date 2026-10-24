# SDRNative
# Python DVB capture example
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
from __future__ import annotations

import argparse
import time
from pathlib import Path

from sdrnative import Device

parser = argparse.ArgumentParser(
    description="Capture the raw native RTL2832 DVB-T transport stream."
)
parser.add_argument("frequency_mhz", type=float)
parser.add_argument("bandwidth_mhz", type=int, choices=(6, 7, 8))
parser.add_argument("output", type=Path)
parser.add_argument("--seconds", type=float, default=8.0)
args = parser.parse_args()

freq = round(args.frequency_mhz * 1_000_000)
bw = args.bandwidth_mhz * 1_000_000

with Device.open() as dev:
    tune = dev.dvbt_start(freq, bw)
    print("tune:", tune)
    print("FSM:", dev.wait_lock(4000))
    print("quality:", dev.signal_quality())

    total = 0
    deadline = time.monotonic() + args.seconds
    with args.output.open("wb") as f:
        while time.monotonic() < deadline:
            block = dev.read()
            f.write(block)
            total += len(block)

    print(f"captured {total / 1024 / 1024:.2f} MiB -> {args.output}")