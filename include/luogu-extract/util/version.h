// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 by-ty
//
// This file is part of luogu-extract
// (https://github.com/by-ty/luogu-extract), a fork of luogu-export
// (https://github.com/sacharei/luogu-export) which is licensed under the
// MIT License (Copyright (c) 2026 sacharei); see the "Original MIT License"
// section in the LICENSE file.
//
// luogu-extract is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version. See the LICENSE file or
// https://www.gnu.org/licenses/lgpl-3.0.html for the full license text.
//
// luogu-extract is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
// License for more details.

// include/luogu-extract/util/version.h
//
// 项目版本与仓库信息。版本号在编译期确定：
// CMake 配置阶段按 CMakeLists.txt 中 project(luogu-extract VERSION ...) 的
// 版本号生成 <构建目录>/generated/luogu-extract/version_config.h，
// 本头文件优先引用该生成文件；非 CMake 构建（例如手工调用编译器）时
// 自动退回下面的后备值，保证代码始终可以编译。
#pragma once

#if defined(__has_include)
#if __has_include("luogu-extract/version_config.h")
#include "luogu-extract/version_config.h"
#endif
#endif

// 未经过 CMake 配置（没有生成 version_config.h）时的后备版本号
#ifndef LUOGU_EXTRACT_VERSION
#define LUOGU_EXTRACT_VERSION "unknown"
#endif
#ifndef LUOGU_EXTRACT_VERSION_MAJOR
#define LUOGU_EXTRACT_VERSION_MAJOR 0
#endif
#ifndef LUOGU_EXTRACT_VERSION_MINOR
#define LUOGU_EXTRACT_VERSION_MINOR 0
#endif
#ifndef LUOGU_EXTRACT_VERSION_PATCH
#define LUOGU_EXTRACT_VERSION_PATCH 0
#endif

// 项目名称、项目仓库地址与版权声明（--version 输出与 LaTeX 封面使用）
#define LUOGU_EXTRACT_PROJECT_NAME "luogu-extract"
#define LUOGU_EXTRACT_REPOSITORY_URL "https://github.com/by-ty/luogu-extract"
#define LUOGU_EXTRACT_COPYRIGHT "Copyright (C) 2026 by-ty"
#define LUOGU_EXTRACT_LICENSE_NAME "GNU LGPL-3.0-or-later"
