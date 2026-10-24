# SDRNative
# Python diagnostic entry point
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0

# diagnostic entry point for
# python -m sdrnative

from __future__ import annotations

from . import abi_version, list_devices, native_library_path, version


def main() -> int:
    print(f"SDRNative runtime: {version()}")
    print(f"C ABI: {abi_version()}")
    print(f"Native library: {native_library_path()}")
    try:
        devices = list_devices()
    except Exception as exc:
        print(f"Device enumeration: ERROR: {exc}")
        return 1

    print(f"Devices: {len(devices)}")
    for i, dev in enumerate(devices):
        label = " ".join(part for part in (dev.manufacturer, dev.product, dev.serial) if part).strip()
        print(f"  [{i}] {label or 'RTL2832U device'} (VID:PID {dev.vid:04X}:{dev.pid:04X})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())