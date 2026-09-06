// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 by-ty
//
// This file is part of luogu-export-next
// (https://github.com/by-ty/luogu-export-next), a fork of luogu-export
// (https://github.com/sacharei/luogu-export) which is licensed under the
// MIT License (Copyright (c) 2026 sacharei); see the "Original MIT License"
// section in the LICENSE file.
//
// luogu-export-next is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published
// by the Free Software Foundation, either version 3 of the License, or (at
// your option) any later version. See the LICENSE file or
// https://www.gnu.org/licenses/lgpl-3.0.html for the full license text.
//
// luogu-export-next is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
// License for more details.

// LaTeX 字体配置的独立模块：集中处理 ctex fontset 选择、用户 --set-font-*
// 参数渲染与等宽西文字体回退链，避免 export_latex 的排版代码继续膨胀。
#ifndef LUOGU_EXPORT_EXPORT_LATEX_FONTS_H
#define LUOGU_EXPORT_EXPORT_LATEX_FONTS_H

#include <cstdio>
#include <string>

#include "luogu-export/export/latex.h"

namespace latex
{
// 根据编译平台 / 运行平台选择 ctex 的 fontset 名：
// Windows -> windows，macOS -> mac；Linux 运行时解析 /etc/os-release，
// Ubuntu 系列 -> ubuntu，其余发行版 -> fandol。
std::string ctex_fontset_name();

// 返回写 \usepackage[...]{ctex} 时使用的选项串（含方括号）。
std::string ctex_package_options();

// 把 main 规范化后的字体参数渲染为 fontspec/xeCJK 可接受的参数。
// 字体名称直接作为名称参数；字体文件地址拆成 Path/Extension/文件名。
std::string font_argument(const std::string &spec);

// 输出默认字体、用户指定的 --set-font-* 字体、标签徽章字体等全部
// 字体设置命令。调用前需已加载 ctex / fontspec / xeCJK 与 xcolor。
void write_font_setup(FILE *out, const Options &opt);
} // namespace latex

#endif // LUOGU_EXPORT_EXPORT_LATEX_FONTS_H
