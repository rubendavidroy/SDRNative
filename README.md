# SDRNative

A library for direct control of SDR hardware, raw IQ sampling,
DVB-T transport stream reception, w/ low-level device access.
Provides tuning, synchronous and asynchronous capture.

Currently supports RTL2832U USB receivers with R820T-family tuners.

Requires CMake 3.20+, C11 and libusb 1.0 for hardware access.

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DSDRNATIVE_REQUIRE_LIBUSB=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Build and installation details: [INSTALL.md](INSTALL.md).

The API is defined in [sdrnative.h](include/sdrnative.h), with runnable programs
under [examples/](examples/). Hardware limits are listed in
[hardware.md](doc/hardware.md), and streaming and device lifetime rules in
[streaming.md](doc/streaming.md).

Python and Node.js package usage is documented in their respective
[Python](bindings/python/README.md) and [Node.js](bindings/node/README.md) READMEs.

DVB-T2 is not supported (yet!).

MPL-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
