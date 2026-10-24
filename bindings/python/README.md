# SDRNative for Python

A library for direct control of SDR hardware, raw IQ sampling,
DVB-T transport stream reception, w/ low-level device access.
Provides tuning, synchronous and asynchronous capture.

Python 3.10+ is required. Supported hardware is currently limited to RTL2832U
receivers with R820T-family tuners.

## Installation

```sh
python -m pip install sdrnative
```

A supported RTL2832U receiver and the appropriate USB driver or access
permissions are needed for hardware use. The native library is distributed
with the platform-specific package; no separate CMake build is required.

```sh
python -m sdrnative
```

This prints the native version, ABI and detected receivers.

## Basic Usage

Raw 8-bit IQ samples are returned as interleaved unsigned I/Q bytes. Frequencies
and sample rates are specified in Hz.

```python
from sdrnative import Device, list_devices

print(list_devices())

with Device.open(0) as dev:
    dev.iq_start(100_700_000, 2_400_000)
    with open("capture.iq", "wb") as output:
        for _ in range(16):
            output.write(dev.read(256 * 1024))
```

For native DVB-T reception, supply a local multiplex frequency and a supported
bandwidth (6, 7 or 8 MHz):

```python
from sdrnative import Device

with Device.open(0) as dev:
    dev.dvbt_start(557_500_000, 7_000_000)
    print("lock stage:", dev.wait_lock(5000))
    print(dev.signal_quality())
    with open("capture.ts", "wb") as output:
        for _ in range(16):
            output.write(dev.read())
```

For asynchronous reads, start the mode first and use `AsyncStream`:

```python
from sdrnative import AsyncConfig, Device

with Device.open(0) as dev:
    dev.iq_start(100_700_000)
    with dev.async_stream(AsyncConfig(queue_depth=16)) as stream:
        for block in stream.read_many(4):
            print(block.sequence, len(block.data))
```

`Device` and `AsyncStream` support context managers. Synchronous `read()` can
return empty bytes on timeout; asynchronous `read()` returns `None` on timeout,
while iteration skips timeouts. Only one asynchronous consumer is supported per
device. Native failures raise `SDRNativeError` subclasses.

The [source repository](https://github.com/rubendavidroy/SDRNative) contains
further capture programs under `examples/python/`.

MPL-2.0.