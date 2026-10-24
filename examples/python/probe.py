# SDRNative
# Python device probe example
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
from sdrnative import Device, list_devices, version

print("SDRNative", version())
for i, info in enumerate(list_devices()):
    print(i, info)

with Device.open(0) as dev:
    print("info:", dev.info)
    print("capabilities:", dev.capabilities)
    print("profile:", dev.profile)
