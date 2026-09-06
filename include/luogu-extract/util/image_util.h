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

// include/luogu-export/util/image_util.h
#ifndef LUOGU_EXPORT_IMAGE_UTIL_H
#define LUOGU_EXPORT_IMAGE_UTIL_H

#include <string>
#include <vector>

namespace image_util
{
    // 从 markdown / HTML 文本中提取图片链接（去重，保持出现顺序）。
    // 支持 markdown 语法 ![alt](url) 以及 HTML 的 <img src="url">。
    std::vector<std::string> extract_urls(const std::string &markdown);
}

#endif // LUOGU_EXPORT_IMAGE_UTIL_H
