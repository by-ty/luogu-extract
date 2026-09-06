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

// src/util/image_util.cpp
#include <regex>
#include <unordered_set>
#include "luogu-extract/util/image_util.h"

namespace
{

// ![alt](url) 或 ![](url)，可带 "title"
const std::regex kMarkdownImage(R"(!\[[^\]]*\]\(\s*([^\s)]+)[^)]*\))", std::regex::icase);

// <img ... src="url" ...>
const std::regex kHtmlImage(R"(<img[^>]*\bsrc\s*=\s*["']([^"']+)["'])", std::regex::icase);

} // namespace

std::vector<std::string> image_util::extract_urls(const std::string &markdown)
{
    std::vector<std::string> urls;
    std::unordered_set<std::string> seen;
    auto add = [&](const std::string &url) {
        if (url.empty() || seen.count(url))
            return;
        seen.insert(url);
        urls.push_back(url);
    };

    for (std::sregex_iterator it(markdown.begin(), markdown.end(), kMarkdownImage), end;
         it != end; ++it)
        add((*it)[1].str());

    for (std::sregex_iterator it(markdown.begin(), markdown.end(), kHtmlImage), end;
         it != end; ++it)
        add((*it)[1].str());

    return urls;
}
