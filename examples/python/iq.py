# SDRNative
# Python IQ capture example
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
from __future__ import annotations

import argparse
from pathlib import Path

from sdrnative import Device


def main() -> None:
    parser = argparse.ArgumentParser(description="Capture raw unsigned 8-bit interleaved IQ with SDRNative")
    parser.add_argument("frequency_mhz", type=float)
    parser.add_argument("--rate", type=float, default=2.4, help="sample rate in MS/s")
    parser.add_argument("--mebibytes", type=float, default=8.0)
    parser.add_argument("--output", type=Path, default=Path("capture.iq"))
    args = parser.parse_args()

    target = int(args.mebibytes * 1024 * 1024)
    with Device.open() as dev, args.output.open("wb") as out:
        tune = dev.iq_start(
            int(args.frequency_mhz * 1_000_000),
            int(args.rate * 1_000_000),
        )
        print(tune)
        print(dev.stream_info)

        total = 0
        while total < target:
            block = dev.read(min(256 * 1024, target - total))
            if not block:
                continue
            out.write(block)
            total += len(block)

    print(f"captured {total:,} bytes -> {args.output}")


if __name__ == "__main__":
    main()
