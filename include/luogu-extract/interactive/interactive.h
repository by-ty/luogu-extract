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

// include/luogu-extract/interactive/interactive.h
// 简易命令行交互程序：不带任何参数运行 ./luogu-extract 时进入。
#ifndef LUOGU_EXTRACT_INTERACTIVE_INTERACTIVE_H
#define LUOGU_EXTRACT_INTERACTIVE_INTERACTIVE_H

namespace interactive
{
    // 依次询问是否更新缓存、导出类型、导出内容与各项设置，最后把设置翻译成
    // 等价的命令行参数交给 app::run 执行（因此下载过程中的提示、警告与确认
    // 与非交互模式完全一致）。过程中输入 q 可随时结束程序。返回进程退出码。
    int run();
} // namespace interactive

#endif // LUOGU_EXTRACT_INTERACTIVE_INTERACTIVE_H
