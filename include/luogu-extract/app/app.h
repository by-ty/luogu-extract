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

// include/luogu-extract/app/app.h
// 程序的「一次执行」入口：解析参数并完成 -U / -M / -L / -C / --tags 等动作。
// main() 在带参数时直接调用它；不带参数时的简易命令行交互程序
// （src/interactive/interactive.cpp）也把用户设置翻译成等价的参数后调用它，
// 因此两种运行方式的行为（含下载过程中的提示、警告与确认）完全一致。
#ifndef LUOGU_EXTRACT_APP_APP_H
#define LUOGU_EXTRACT_APP_APP_H

#include <string>
#include <vector>

namespace app
{
    // 以给定的 UTF-8 参数表执行一次程序：args[0] 为程序名，其余为命令行参数
    // （与 argv 完全等价，可含长选项的 "--name=value" 写法）。
    // 返回进程退出码。同一进程内可多次调用（每次调用都会复位参数解析状态）。
    int run(const std::vector<std::string> &args_utf8);
} // namespace app

#endif // LUOGU_EXTRACT_APP_APP_H
