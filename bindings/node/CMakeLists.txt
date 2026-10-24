# SDRNative
# Node binding build configuration
#
# Copyright (c) 2026 Ruben Roy
# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.20)
project(sdrnative_node LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_program(NODE_EXECUTABLE node REQUIRED)
execute_process(
    COMMAND "${NODE_EXECUTABLE}" -p "require('path').resolve(require('path').dirname(process.execPath), '..')"
    OUTPUT_VARIABLE NODE_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
)
find_path(NODE_API_INCLUDE_DIR node_api.h
    HINTS
        "${NODE_PREFIX}/include/node"
        /usr/include/node
        /usr/local/include/node
        /opt/homebrew/include/node
    REQUIRED
)

option(SDRNATIVE_NODE_REQUIRE_LIBUSB "Require libusb for the Node binding" ON)
set(SDRNATIVE_REQUIRE_LIBUSB ${SDRNATIVE_NODE_REQUIRE_LIBUSB} CACHE BOOL "" FORCE)
set(SDRNATIVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SDRNATIVE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/sdrnative-core")

add_library(sdrnative_node MODULE src/addon.cc)
target_include_directories(sdrnative_node PRIVATE
    "${NODE_API_INCLUDE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../../include"
)
target_link_libraries(sdrnative_node PRIVATE SDRNative::sdrnative)

# Windows addon links libusb and MSVC runtime statically
# node.exe and system DLLs only remain runtime dependencies
if(MSVC)
    set_property(TARGET sdrnative_node PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(sdrnative_node PRIVATE kernel32 user32)
endif()

# POSIX resolves Node API symbols from loaded runtime
# Windows links node.lib matching installed node release
if(WIN32)
    set(NODE_LIBRARY "" CACHE FILEPATH "Path to node.lib matching target Node runtime")
    if(NOT NODE_LIBRARY OR NOT EXISTS "${NODE_LIBRARY}")
        message(FATAL_ERROR
            "node.lib not found for the target Node.js version. "
            "Set NODE_LIBRARY to its path.")
    endif()
    target_link_libraries(sdrnative_node PRIVATE "${NODE_LIBRARY}")
endif()

target_compile_definitions(sdrnative_node PRIVATE NAPI_VERSION=8)

if(MSVC)
    target_compile_options(sdrnative_node PRIVATE /W4 /EHsc)
else()
    target_compile_options(sdrnative_node PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(APPLE)
    target_link_options(sdrnative_node PRIVATE "-undefined" "dynamic_lookup")
endif()

set_target_properties(sdrnative_node PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/build/Release"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/build/Release"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_LIST_DIR}/build/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_LIST_DIR}/build/Release"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_LIST_DIR}/build/Release"
)
