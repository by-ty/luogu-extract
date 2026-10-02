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

// include/luogu-extract/util/cli_parse.h
// 命令行参数值的解析与校验：main.cpp 与交互程序共用，保证判定与报错文案一致。
#ifndef LUOGU_EXTRACT_UTIL_CLI_PARSE_H
#define LUOGU_EXTRACT_UTIL_CLI_PARSE_H

#include <string>
#include <utility>
#include <vector>

namespace cliparse
{
    // ASCII 大小写转换（不改动多字节 UTF-8 字节）
    std::string to_lower_ascii(const std::string &s);
    std::string to_upper_ascii(const std::string &s);

    // 按空白拆分（忽略空 token）
    std::vector<std::string> split_whitespace(const std::string &s);

    // 解析整数：允许首尾空白；超出 [min,max] 或含非数字字符返回 false
    bool parse_positive_int(const std::string &value, long min_value,
                            long max_value, long &out);

    // 难度规格 "N" 或 "A-B"（闭区间），展开后追加到 difficulties
    bool parse_difficulty_spec(const std::string &spec, std::vector<int> &difficulties);

    // --pid-range "<题号>-<题号>"（两端含）：规范化（大写）写入 out；失败写中文 err 并返回 false
    bool parse_pid_range_arg(const std::string &spec,
                             std::pair<std::string, std::string> &out,
                             std::string &err);

    // --local 默认输出：同目录的 <原文件名>.tex
    std::string default_local_output(const std::string &input);

    // 是否按字体文件地址处理（含路径分隔符、字体扩展名，或当前目录存在同名文件）
    bool font_value_is_file_address(const std::string &value);

    // 校验 --set-font-*：系统已安装的字体名直接透传；文件地址必须存在，否则报错拒绝执行。
    // 合法时把归一化结果（绝对路径、'\' -> '/'）写入 font_out；返回错误信息（空串=合法）
    std::string validate_font_option(const std::string &option_name,
                                     const std::string &value,
                                     std::string &font_out);
} // namespace cliparse

#endif // LUOGU_EXTRACT_UTIL_CLI_PARSE_H
