#!/usr/bin/env python3
"""Bundle an installed SDRNative CMake SDK and its libusb runtime."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

VERSION = "0.1.0"
TARGETS = {"windows-x64", "macos-arm64", "macos-x64", "linux-x64"}


def run(*cmd):
    return subprocess.check_output(cmd, text=True, stderr=subprocess.STDOUT).strip()


def require(path):
    if not path.is_file():
        raise SystemExit(f"missing SDK file: {path}")
    return path


def make_archive(sdk, dest, target):
    if target.startswith("windows"):
        with zipfile.ZipFile(dest, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for file in sorted(sdk.rglob("*")):
                if file.is_file():
                    z.write(file, arcname=(Path("sdrnative") / file.relative_to(sdk)).as_posix())
    else:
        with tarfile.open(dest, "w:gz", dereference=False) as tar:
            tar.add(sdk, arcname="sdrnative")
    print(f"built {dest} ({dest.stat().st_size} bytes)")


def macos_runtime(sdk):
    libdir = sdk / "lib"
    real = [p for p in libdir.glob("libsdrnative*.dylib") if p.is_file() and not p.is_symlink()]
    if len(real) != 1:
        raise SystemExit(f"expected one real SDRNative dylib; got {real}")
    native = real[0]
    brew = Path(run("brew", "--prefix", "libusb"))
    dependency_file = require(brew / "lib" / "libusb-1.0.0.dylib")
    bundled = libdir / "libusb-1.0.0.dylib"
    shutil.copy2(dependency_file, bundled)
    deps = run("otool", "-L", str(native)).splitlines()[1:]
    usb_paths = [line.strip().split()[0] for line in deps if "libusb" in line]
    if len(usb_paths) != 1:
        raise SystemExit(f"expected one libusb dependency: {deps}")
    run("install_name_tool", "-change", usb_paths[0], "@loader_path/libusb-1.0.0.dylib", str(native))
    run("install_name_tool", "-id", "@loader_path/libusb-1.0.0.dylib", str(bundled))
    for library in (native, bundled):
        run("codesign", "--force", "--sign", "-", str(library))
    output = run("otool", "-L", str(native))
    if "@loader_path/libusb-1.0.0.dylib" not in output:
        raise SystemExit(f"libusb install name rewrite failed:\n{output}")
    # Prevent accidental linkage to Homebrew libraries other than libusb itself.
    extra = [line.strip().split()[0] for line in run("otool", "-L", str(bundled)).splitlines()[1:]
             if line.strip() and line.strip().split()[0].startswith(("/opt/homebrew/", "/usr/local/"))]
    if extra:
        raise SystemExit(f"libusb has unbundled Homebrew dependencies: {extra}")


def linux_runtime(sdk):
    libdir = sdk / "lib"
    native = require(libdir / "libsdrnative.so.0.1.0")
    found = []
    for path in ("/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/lib64"):
        p = Path(path) / "libusb-1.0.so.0"
        if p.is_file():
            found.append(p)
    if not found:
        raise SystemExit("cannot locate libusb-1.0.so.0")
    bundled = libdir / "libusb-1.0.so.0"
    shutil.copy2(found[0], bundled)
    rpaths = run("readelf", "-d", str(native))
    if "$ORIGIN" not in rpaths:
        raise SystemExit(f"installed SDRNative library lacks $ORIGIN RUNPATH/RPATH:\n{rpaths}")
    output = run("ldd", str(native))
    path = str(bundled.resolve())
    if not any(path in line for line in output.splitlines() if "libusb-1.0.so.0" in line):
        raise SystemExit(f"libusb did not resolve to the bundled library:\n{output}")


def windows_runtime(sdk):
    require(sdk / "bin" / "sdrnative.dll")
    require(sdk / "lib" / "sdrnative.lib")
    src = require(Path("C:/vcpkg/installed/x64-windows/bin/libusb-1.0.dll"))
    shutil.copy2(src, sdk / "bin" / "libusb-1.0.dll")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", type=Path, required=True, help="directory containing installed sdrnative/")
    parser.add_argument("--source", type=Path, required=True, help="SDRNative v0.1.0 source checkout")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--target", choices=sorted(TARGETS), required=True)
    args = parser.parse_args()
    sdk = (args.stage / "sdrnative").resolve()
    if not sdk.is_dir():
        raise SystemExit(f"missing native SDK directory: {sdk}")
    require(sdk / "include" / "sdrnative.h")
    require(sdk / "lib" / "cmake" / "SDRNative" / "SDRNativeConfig.cmake")
    for filename in ("LICENSE", "NOTICE"):
        shutil.copy2(require(args.source / filename), sdk / filename)
    lic = sdk / "licenses"
    lic.mkdir(parents=True, exist_ok=True)
    shutil.copy2(require(args.source / "licenses" / "libusb-LGPL-2.1.txt"), lic / "libusb-LGPL-2.1.txt")
    if args.target.startswith("macos"):
        macos_runtime(sdk)
    elif args.target.startswith("linux"):
        linux_runtime(sdk)
    else:
        windows_runtime(sdk)
    (sdk / "README.txt").write_text(
        "SDRNative 0.1.0 native C SDK\n\n"
        "Headers: include/sdrnative.h\n"
        "CMake:  find_package(SDRNative 0.1 CONFIG REQUIRED)\n"
        "        target_link_libraries(your_target PRIVATE SDRNative::sdrnative)\n\n"
        "Configure consumers with -DCMAKE_PREFIX_PATH=/path/to/sdrnative.\n"
        "Windows: sdrnative.dll and libusb-1.0.dll are in bin/.\n"
        "macOS/Linux: shared libraries and libusb are in lib/.\n"
        "Linux x64 binaries target an Ubuntu 22.04-compatible glibc environment; system libudev may be required.\n"
        "macOS Intel targets macOS 14+; Apple Silicon targets macOS 15+.\n"
        "Your executable must be able to locate the SDRNative shared library at runtime.\n"
        "This SDK has not been physically validated against a connected receiver on every platform.\n"
        "Source: https://github.com/rubendavidroy/SDRNative\n",
        encoding="utf-8")
    args.out.mkdir(parents=True, exist_ok=True)
    ext = "zip" if args.target.startswith("windows") else "tar.gz"
    dest = args.out / f"sdrnative-{VERSION}-{args.target}.{ext}"
    make_archive(sdk, dest, args.target)


if __name__ == "__main__":
    main()
