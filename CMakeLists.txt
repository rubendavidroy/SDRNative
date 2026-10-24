# SDRNative
# Build configuration
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.20)
project(sdrnative VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SDRNATIVE_BUILD_TESTS "Build mock backed tests" ON)
option(SDRNATIVE_BUILD_EXAMPLES "Build C examples" ON)
option(SDRNATIVE_REQUIRE_LIBUSB "Fail configure when libusb is unavailable" OFF)
option(SDRNATIVE_PYTHON_WHEEL "Build the native library for a Python wheel" OFF)

if(SDRNATIVE_PYTHON_WHEEL)
    set(SDRNATIVE_REQUIRE_LIBUSB ON)
    set(SDRNATIVE_BUILD_TESTS OFF)
    set(SDRNATIVE_BUILD_EXAMPLES OFF)
    set(BUILD_SHARED_LIBS ON)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(LibUSB1 QUIET)
find_package(Threads REQUIRED)
if(SDRNATIVE_REQUIRE_LIBUSB AND NOT LibUSB1_FOUND)
    message(FATAL_ERROR "libusb-1.0 is required but was not found")
endif()

set(SDRNATIVE_CORE_SOURCES
    src/abi.c
    src/async_stream.c
    src/device.c
    src/error.c
    src/platform.c
    src/threading.c
    src/rtl28xx.c
    src/rtl2832.c
    src/rtl2832_iq.c
    src/r820t.c
)

add_library(sdrnative
    ${SDRNATIVE_CORE_SOURCES}
    src/usb_libusb.c
)
add_library(SDRNative::sdrnative ALIAS sdrnative)

target_include_directories(sdrnative
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(sdrnative PRIVATE SDRNATIVE_BUILD)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(sdrnative PUBLIC SDRNATIVE_SHARED)
endif()

target_link_libraries(sdrnative PRIVATE Threads::Threads)

if(LibUSB1_FOUND)
    target_compile_definitions(sdrnative PRIVATE SDRNATIVE_HAVE_LIBUSB=1)
    target_link_libraries(sdrnative PRIVATE LibUSB1::LibUSB1)
    message(STATUS "libusb backend enabled")
else()
    message(WARNING "libusb not found; USB hardware support disabled")
endif()

if(NOT WIN32)
    target_link_libraries(sdrnative PRIVATE m)
endif()

if(MSVC)
    target_compile_options(sdrnative PRIVATE /W4)
else()
    target_compile_options(sdrnative PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

set_target_properties(sdrnative PROPERTIES
    OUTPUT_NAME sdrnative
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    POSITION_INDEPENDENT_CODE ON
)

if(BUILD_SHARED_LIBS AND NOT MSVC)
    set_target_properties(sdrnative PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
endif()

if(SDRNATIVE_PYTHON_WHEEL)
    set_target_properties(sdrnative PROPERTIES VERSION "" SOVERSION "" NO_SONAME ON)
endif()

if(SDRNATIVE_BUILD_EXAMPLES)
    add_executable(sdrn_probe examples/c/probe.c)
    target_link_libraries(sdrn_probe PRIVATE SDRNative::sdrnative)

    add_executable(sdrn_dvbt_capture examples/c/dvbt.c)
    target_link_libraries(sdrn_dvbt_capture PRIVATE SDRNative::sdrnative)

    add_executable(sdrn_iq_capture examples/c/iq.c)
    target_link_libraries(sdrn_iq_capture PRIVATE SDRNative::sdrnative)
endif()

if(SDRNATIVE_BUILD_TESTS)
    enable_testing()

    function(sdrn_add_mock_test target source)
        add_executable(${target}
            ${source}
            tests/mock_usb.c
            ${SDRNATIVE_CORE_SOURCES}
        )
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        target_link_libraries(${target} PRIVATE Threads::Threads)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4)
        else()
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wshadow
            )
            target_link_libraries(${target} PRIVATE m)
        endif()
    endfunction()

    sdrn_add_mock_test(sdrnative_device_test tests/device.c)
    add_test(NAME sdrnative_device COMMAND sdrnative_device_test)


    add_executable(sdrnative_abi_test tests/abi.c)
    target_link_libraries(sdrnative_abi_test PRIVATE SDRNative::sdrnative)
    if(MSVC)
        target_compile_options(sdrnative_abi_test PRIVATE /W4)
    else()
        target_compile_options(sdrnative_abi_test PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
    endif()
    add_test(NAME sdrnative_abi COMMAND sdrnative_abi_test)

    sdrn_add_mock_test(sdrnative_async_test tests/async.c)
    add_test(NAME sdrnative_async COMMAND sdrnative_async_test)


    sdrn_add_mock_test(sdrnative_async_errors_test tests/async_errors.c)
    add_test(NAME sdrnative_async_errors COMMAND sdrnative_async_errors_test)

endif()

if(SDRNATIVE_PYTHON_WHEEL)
    install(TARGETS sdrnative
        RUNTIME DESTINATION sdrnative
        LIBRARY DESTINATION sdrnative
        ARCHIVE DESTINATION sdrnative
    )
else()
install(TARGETS sdrnative
    EXPORT SDRNativeTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/sdrnative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT SDRNativeTargets
    FILE SDRNativeTargets.cmake
    NAMESPACE SDRNative::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SDRNative
)

set(SDRNATIVE_CONFIG_NEEDS_LIBUSB OFF)
if(LibUSB1_FOUND AND NOT BUILD_SHARED_LIBS)
    set(SDRNATIVE_CONFIG_NEEDS_LIBUSB ON)
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SDRNativeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SDRNativeConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SDRNative"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SDRNativeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SDRNativeConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SDRNativeConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SDRNative"
)
if(SDRNATIVE_CONFIG_NEEDS_LIBUSB)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindLibUSB1.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/SDRNative"
    )
endif()
endif()