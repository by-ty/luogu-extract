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

// include/luogu-export/export/markdown.h
#ifndef LUOGU_EXPORT_MARKDOWN_H
#define LUOGU_EXPORT_MARKDOWN_H

#include <filesystem>
#include <string>
#include "luogu-export/export/common.h"

namespace markdown
{
    // 读取缓存的题目列表（latest.ndjson），按条件筛选后合并成一个 markdown 文件
    // @param filter      筛选条件（与 -L 共用）
    // @param output_path 输出文件路径
    // @param error       失败时返回的错误信息
    // @param cover_title 一级标题文字（--set-cover-title；空串表示默认“洛谷题目导出”）
    // @return 成功返回 true
    bool export_markdown(const luogu::ExportFilter &filter,
                         const std::filesystem::path &output_path,
                         std::string &error,
                         const std::string &cover_title = "");
}

#endif // LUOGU_EXPORT_MARKDOWN_H
