# SDRNative
# libusb package lookup
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PC_LIBUSB1 QUIET libusb-1.0)
endif()

find_path(LIBUSB1_INCLUDE_DIR
    NAMES libusb.h
    HINTS
        ${PC_LIBUSB1_INCLUDEDIR}
        ${PC_LIBUSB1_INCLUDE_DIRS}
        /opt/homebrew/include/libusb-1.0
        /usr/local/include/libusb-1.0
        /usr/include/libusb-1.0
    PATH_SUFFIXES libusb-1.0
)

find_library(LIBUSB1_LIBRARY
    NAMES usb-1.0 libusb-1.0 usb
    HINTS
        ${PC_LIBUSB1_LIBDIR}
        ${PC_LIBUSB1_LIBRARY_DIRS}
        /opt/homebrew/lib
        /usr/local/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibUSB1
    REQUIRED_VARS LIBUSB1_LIBRARY LIBUSB1_INCLUDE_DIR
)

if(LibUSB1_FOUND AND NOT TARGET LibUSB1::LibUSB1)
    add_library(LibUSB1::LibUSB1 UNKNOWN IMPORTED)
    set_target_properties(LibUSB1::LibUSB1 PROPERTIES
        IMPORTED_LOCATION "${LIBUSB1_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB1_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(LIBUSB1_INCLUDE_DIR LIBUSB1_LIBRARY)