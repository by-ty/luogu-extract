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
// 命令行参数值的解析与校验：main.cpp 的参数解析与简易命令行交互程序
// （src/interactive/interactive.cpp）共用这一份实现，保证「同样的值」在两处
// 得到完全相同的判定与中文报错文案。
#ifndef LUOGU_EXTRACT_UTIL_CLI_PARSE_H
#define LUOGU_EXTRACT_UTIL_CLI_PARSE_H

#include <string>
#include <utility>
#include <vector>

namespace cliparse
{
    // ASCII 大小写转换（不影响多字节的 UTF-8 字节）
    std::string to_lower_ascii(const std::string &s);
    std::string to_upper_ascii(const std::string &s);

    // 按空白拆成多个 token（空 token 忽略）
    std::vector<std::string> split_whitespace(const std::string &s);

    // 解析非负/正整数参数：允许首尾空白，超出 [min,max] 或含非数字字符时返回 false
    bool parse_positive_int(const std::string &value, long min_value,
                            long max_value, long &out);

    // 解析难度规格："N"（单个数字）或 "A-B"（闭区间），展开后追加到 difficulties
    bool parse_difficulty_spec(const std::string &spec, std::vector<int> &difficulties);

    // 解析一组 --pid-range 规格 "<题号>-<题号>"（两端点均包含）。
    // 成功时把规范化（大写）的两端点写入 out 并返回 true；
    // 失败时返回 false，并把中文错误信息（含相关要求）写入 err。
    bool parse_pid_range_arg(const std::string &spec,
                             std::pair<std::string, std::string> &out,
                             std::string &err);

    // --local 的默认输出：与输入文件同目录的 <原文件名>.tex（去掉原扩展名）
    std::string default_local_output(const std::string &input);

    // 该值是否会被当作「字体文件地址」处理：含路径分隔符、以常见字体扩展名
    // 结尾，或当前目录下存在同名文件；其余值按系统已安装的字体名称处理。
    bool font_value_is_file_address(const std::string &value);

    // 校验字体参数（--set-font-*）的值，并区分两种写法：
    // - 系统已安装的字体名称：直接透传给 fontspec；
    // - 字体文件地址：必须真实存在，否则报错拒绝执行；存在时转成绝对路径并把
    //   '\' 归一化为 '/'，便于写入 LaTeX 代码。
    // 返回错误信息（空串表示合法）；合法时把规范化结果写入 font_out。
    std::string validate_font_option(const std::string &option_name,
                                     const std::string &value,
                                     std::string &font_out);
} // namespace cliparse

#endif // LUOGU_EXTRACT_UTIL_CLI_PARSE_H
