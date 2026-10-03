# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 by-ty
#
# This file is part of luogu-extract
# (https://github.com/by-ty/luogu-extract), a fork of luogu-export
# (https://github.com/sacharei/luogu-export) which is licensed under the
# MIT License (Copyright (c) 2026 sacharei); see the "Original MIT License"
# section in the LICENSE file.
#
# luogu-extract is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published
# by the Free Software Foundation, either version 3 of the License, or (at
# your option) any later version. See the LICENSE file or
# https://www.gnu.org/licenses/lgpl-3.0.html for the full license text.
#
# luogu-extract is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
# License for more details.

cmake_minimum_required(VERSION 3.23)

project(luogu-extract VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LUOGU_EXTRACT_STATIC "Statically link third-party dependencies" OFF)
option(LUOGU_EXTRACT_FULLY_STATIC "Also statically link the C/C++ runtime (Linux)" OFF)

if(LUOGU_EXTRACT_STATIC)
    if(MSVC)
        # vcpkg 的 *-windows-static triplet 用 /MT 构建依赖，主程序必须一致
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(WIN32)
        # 静态 libcurl 时必须定义，否则头文件按 dllimport 声明导致链接失败
        add_compile_definitions(CURL_STATICLIB)
    endif()
    if(UNIX AND NOT APPLE)
        # 只让 find_library 选 .a：-static 下链接到 .so 会直接报错
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    endif()
endif()

find_package(CURL REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(ZLIB REQUIRED)

file(GLOB_RECURSE LUOGU_EXTRACT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/luogu-extract/util/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/luogu-extract/version_config.h"
    @ONLY)

add_executable(luogu-extract ${LUOGU_EXTRACT_SOURCES})

target_compile_features(luogu-extract PRIVATE cxx_std_17)

target_include_directories(luogu-extract PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_options(luogu-extract PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>)

target_link_libraries(luogu-extract PRIVATE
    CURL::libcurl
    LibXml2::LibXml2
    nlohmann_json::nlohmann_json
    ZLIB::ZLIB)

if(LUOGU_EXTRACT_STATIC AND UNIX AND NOT APPLE)
    if(LUOGU_EXTRACT_FULLY_STATIC)
        target_link_options(luogu-extract PRIVATE -static)
    else()
        target_link_options(luogu-extract PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS luogu-extract RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
