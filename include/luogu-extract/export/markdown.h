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

// include/luogu-extract/export/markdown.h
#ifndef LUOGU_EXTRACT_MARKDOWN_H
#define LUOGU_EXTRACT_MARKDOWN_H

#include <filesystem>
#include <string>
#include "luogu-extract/export/common.h"

namespace markdown
{
    // 读取缓存题目列表，按条件筛选后合并成一个 markdown 文件。
    // cover_title：一级标题（空串表示默认「洛谷题目导出」）；display：与 -L 共用的
    // 题目信息显示开关；solutions：题解包（nullptr 表示不导出，与题面同文件）；
    // articles：--article 的文章（nullptr 表示没有），统一置于文档最后，级别与题解相同，
    // 只是没有题目跳转链接与「（所属题目）」说明
    bool export_markdown(const luogu::ExportFilter &filter,
                         const std::filesystem::path &output_path,
                         std::string &error,
                         const std::string &cover_title = "",
                         const luogu::DisplayOptions &display = {},
                         const luogu::SolutionBundle *solutions = nullptr,
                         const luogu::SolutionExportOptions &solution_export = {},
                         const luogu::ProblemSelection *preselected = nullptr,
                         const luogu::ArticleBundle *articles = nullptr);
}

#endif // LUOGU_EXTRACT_MARKDOWN_H
