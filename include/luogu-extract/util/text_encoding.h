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

// include/luogu-extract/util/text_encoding.h
// 本地文本文件的编码兼容：把文件内容统一转成 UTF-8（--local 转写本地 Markdown）。
//
// 程序内部一律按 UTF-8 处理文本（生成的 .tex 也必须是 UTF-8，xelatex + ctex
// 才能正确排版中文），但用户手上的 Markdown 文件可能是各种编码，因此读入时
// 统一转码：
// - 有 BOM：按 BOM 判定 UTF-8 / UTF-16 LE、BE / UTF-32 LE、BE；
// - 无 BOM：先按严格 UTF-8 校验，通过即按 UTF-8 使用；否则按 GB18030 转码
//   （GB18030 是 GBK / GB2312 的超集，覆盖简体中文的常见「ANSI」文件）；
// - 统一把 CRLF / CR 归一化为 LF，并过滤 C0 控制字符（保留 \t 与 \n），
//   避免残留的 \r 或控制字符破坏生成的 LaTeX。
#ifndef LUOGU_EXTRACT_UTIL_TEXT_ENCODING_H
#define LUOGU_EXTRACT_UTIL_TEXT_ENCODING_H

#include <filesystem>
#include <string>

namespace textenc
{
    /// 读取文本文件并把内容统一转成 UTF-8。
    /// 失败时返回 false，并把中文说明（含文件路径与所尝试的编码）写入 error。
    bool read_text_file_utf8(const std::filesystem::path &path, std::string &out,
                             std::string &error);
} // namespace textenc

#endif // LUOGU_EXTRACT_UTIL_TEXT_ENCODING_H
