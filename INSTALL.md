# Build

The library requires CMake 3.20+, a C11 compiler and libusb 1.0 development
files for hardware access.

On Windows x64, install Visual Studio C++ Build Tools, CMake and vcpkg, set
`VCPKG_ROOT`, then run `scripts\build_windows.ps1` in PowerShell. A
WinUSB-compatible driver is needed for the receiver interface.

On macOS:

```sh
brew install cmake libusb pkg-config
./scripts/build_macos.sh
```

On Linux (experimental):

```sh
./scripts/build_linux.sh
```

To build and install manually:

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON -DSDRNATIVE_REQUIRE_LIBUSB=ON
cmake --build build
cmake --install build --prefix ./_install
```

CMake consumers can use `find_package(SDRNative 0.1 CONFIG REQUIRED)` and link
`SDRNative::sdrnative`. Set `CMAKE_PREFIX_PATH` to the installation prefix
if necessary. The shared library must be on the system's library search path.

## Python

```sh
python -m pip install sdrnative
```

Requires Python 3.10+. See [bindings/python/README.md](bindings/python/README.md)
for usage.

## Node.js

```sh
npm install sdrnative
```

Requires Node.js 18+. TypeScript declarations are included. See
[bindings/node/README.md](bindings/node/README.md).

USB drivers and device permissions are platform-specific and aren't configured
by the package managers.