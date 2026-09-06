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

// include/luogu-export/util/getopt_compat.h
// 平台无关的 getopt / getopt_long 实现：Windows（MSVC/MinGW-w64）通常不提供
// POSIX <getopt.h>，本头文件按 glibc 的实现语义移植了本程序用到的行为：
//   - optstring 以 ':' 开头时不打印错误：未知选项返回 '?'，缺少参数返回 ':'
//   - optstring 以 '+' 开头时遇第一个裸参数即停止；以 '-' 开头时裸参数
//     作为选项 1 返回（optarg 指向参数）
//   - 支持短选项簇（如 -UML）、长选项 --name、--name=value、--name value
//   - 支持长选项的无歧义前缀缩写（与 GNU getopt 一致）；歧义时优先精确匹配
//   - 出错时 argv[optind - 1] 指向出错的 token
//   - 必选参数会吞掉下一个 token，即使它以 '-' 开头（与 GNU 一致）
//   - 实现 GNU 式参数重排（permutation，直接移植 glibc 的 exchange 算法）：
//     选项与裸参数可以混排，裸参数保持原有相对顺序被挪到 argv 末尾；
//     getopt 返回 -1 后 argv[optind..argc) 即全部裸参数
//   - "--" 之后的内容全部视为裸参数；"--" 自身被交换到裸参数区之前
// 非 Windows 平台默认用系统 <getopt.h>；定义 LUOGU_FORCE_COMPAT_GETOPT
// 可强制使用本实现（用于测试）。
#ifndef LUOGU_EXPORT_UTIL_GETOPT_COMPAT_H
#define LUOGU_EXPORT_UTIL_GETOPT_COMPAT_H

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#define no_argument 0
#define required_argument 1
#define optional_argument 2

struct option
{
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

// 与 glibc/POSIX 相同的 C 链接：非 Windows 平台强制测试本实现时，
// 系统头文件（如 unistd.h）也会声明这些符号，C 链接可以保证兼容。
extern "C"
{

inline char *optarg = nullptr;
inline int optind = 1;
inline int opterr = 1;
inline int optopt = '?';

namespace detail
{
    // 参数重排状态（等价于 glibc 的 __first_nonopt / __last_nonopt / __ordering）
    struct PermuteState
    {
        int first_nonopt = 1;   // 已跳过的裸参数区起始
        int last_nonopt = 1;    // 已跳过的裸参数区末尾（== 下一个选项位置）
        int ordering = 0;       // 0 = PERMUTE, 1 = REQUIRE_ORDER, 2 = RETURN_IN_ORDER
        const char *nextchar = nullptr; // 当前短选项簇内的解析位置
        bool initialized = false;
    };
    inline PermuteState &permute_state()
    {
        static PermuteState state;
        return state;
    }

    // 判断 argv[i] 是否为裸参数（不以 '-' 开头，或就是单独的 "-"）
    inline bool is_nonoption(const char *arg)
    {
        return arg[0] != '-' || arg[1] == '\0';
    }

    // 直接移植 glibc 的 exchange：交换 argv 中两个相邻块
    //   [first_nonopt, last_nonopt)（已跳过的裸参数）与
    //   [last_nonopt, optind)（期间处理过的选项），
    // 保持每块内部顺序不变，并更新 first_nonopt / last_nonopt。
    inline void exchange(char **argv, int first_nonopt, int last_nonopt, int optind_now)
    {
        int bottom = first_nonopt;
        int middle = last_nonopt;
        int top = optind_now;
        while (top > middle && middle > bottom)
        {
            if (top - middle > middle - bottom)
            {
                const int len = middle - bottom;
                for (int i = 0; i < len; ++i)
                    std::swap(argv[bottom + i], argv[top - len + i]);
                top -= len;
            }
            else
            {
                const int len = top - middle;
                for (int i = 0; i < len; ++i)
                    std::swap(argv[bottom + i], argv[middle + i]);
                bottom += len;
            }
        }
        PermuteState &st = permute_state();
        st.first_nonopt += (optind_now - st.last_nonopt);
        st.last_nonopt = optind_now;
    }

    // 打印错误信息（等价于 glibc 的行为：opterr 非 0 且 optstring 不以 ':' 开头）
    inline bool should_print_errors(const char *optstring)
    {
        return opterr != 0 && optstring[0] != ':';
    }

    // 处理长选项（移植 glibc process_long_option，long_only 恒为 false）。
    // 返回 getopt_long 应返回的值。
    inline int process_long_option(int argc, char *const argv[],
                                   const char *optstring,
                                   const struct option *longopts,
                                   int *longindex,
                                   const char *name /* 不含前导 -- */)
    {
        PermuteState &st = permute_state();
        const bool print_errors = should_print_errors(optstring);

        // 名称在 '=' 处截断
        size_t name_len = 0;
        while (name[name_len] && name[name_len] != '=')
            ++name_len;

        const struct option *pfound = nullptr;
        int option_index = -1;

        // 1. 先找精确匹配（GNU 语义：精确匹配优先于前缀缩写，
        //    即使存在多个前缀匹配项）
        for (int i = 0; longopts && longopts[i].name; ++i)
        {
            if (std::strlen(longopts[i].name) == name_len &&
                std::strncmp(longopts[i].name, name, name_len) == 0)
            {
                pfound = &longopts[i];
                option_index = i;
                break;
            }
        }

        // 2. 无精确匹配时找前缀缩写；多个候选且 (has_arg, flag, val) 不同则歧义
        if (pfound == nullptr)
        {
            std::string ambig_list;
            for (int i = 0; longopts && longopts[i].name; ++i)
            {
                if (std::strncmp(longopts[i].name, name, name_len) != 0)
                    continue;
                if (pfound == nullptr)
                {
                    pfound = &longopts[i];
                    option_index = i;
                }
                else if (pfound->has_arg != longopts[i].has_arg ||
                         pfound->flag != longopts[i].flag ||
                         pfound->val != longopts[i].val)
                {
                    // 记录歧义候选：无论是否打印错误，歧义都成立
                    if (print_errors)
                    {
                        if (ambig_list.empty())
                            ambig_list = std::string("'--") + pfound->name + "'";
                        ambig_list += " '--" + std::string(longopts[i].name) + "'";
                    }
                    else if (ambig_list.empty())
                    {
                        ambig_list = "'" + std::string(pfound->name) + "'";
                    }
                }
            }
            if (!ambig_list.empty())
            {
                if (print_errors)
                {
                    std::fprintf(stderr, "%s: option '--%.*s' is ambiguous; possibilities: %s\n",
                                 argv[0], static_cast<int>(name_len), name,
                                 ambig_list.c_str());
                }
                st.nextchar = nullptr;
                ++optind;
                optopt = 0;
                return '?';
            }
        }

        // 3. 未匹配到任何长选项
        if (pfound == nullptr)
        {
            if (print_errors)
                std::fprintf(stderr, "%s: unrecognized option '--%.*s'\n",
                             argv[0], static_cast<int>(name_len), name);
            st.nextchar = nullptr;
            ++optind;
            optopt = 0;
            return '?';
        }

        // 4. 已匹配：消费该 token
        ++optind;
        st.nextchar = nullptr;
        if (name[name_len] == '=') // 带 "=value"
        {
            if (pfound->has_arg)
            {
                optarg = const_cast<char *>(name + name_len + 1);
            }
            else
            {
                if (print_errors)
                    std::fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                                 argv[0], pfound->name);
                optopt = pfound->val;
                return '?';
            }
        }
        else if (pfound->has_arg == required_argument)
        {
            if (optind < argc)
            {
                optarg = argv[optind];
                ++optind;
            }
            else
            {
                if (print_errors)
                    std::fprintf(stderr, "%s: option '--%s' requires an argument\n",
                                 argv[0], pfound->name);
                optopt = pfound->val;
                return optstring[0] == ':' ? ':' : '?';
            }
        }
        // optional_argument 且未带 "="：optarg 保持 nullptr

        if (longindex)
            *longindex = option_index;
        if (pfound->flag)
        {
            *pfound->flag = pfound->val;
            return 0;
        }
        return pfound->val;
    }
} // namespace detail

inline int getopt_long(int argc, char *const argv[], const char *optstring,
                       const struct option *longopts, int *longindex) noexcept
{
    using namespace detail;
    PermuteState &st = permute_state();
    char **av = const_cast<char **>(argv);

    if (longindex)
        *longindex = -1;
    optarg = nullptr;

    // 首次调用时按 optstring 前缀确定重排策略
    if (!st.initialized)
    {
        st.initialized = true;
        if (optind == 0)
            optind = 1;
        st.first_nonopt = st.last_nonopt = optind;
        if (optstring[0] == '-')
            st.ordering = 2; // RETURN_IN_ORDER
        else if (optstring[0] == '+')
            st.ordering = 1; // REQUIRE_ORDER
        else
            st.ordering = 0; // PERMUTE
    }
    // 后续调用时 optstring 前缀符号已在初始化时消费，跳过
    if (optstring[0] == '-' || optstring[0] == '+')
        ++optstring;
    const bool colon_mode = (optstring[0] == ':');

    // ---- 需要前进到下一个 token ----
    if (st.nextchar == nullptr || *st.nextchar == '\0')
    {
        // 用户可能手动回退过 optind：把记录区间收敛到有效范围
        if (st.last_nonopt > optind)
            st.last_nonopt = optind;
        if (st.first_nonopt > optind)
            st.first_nonopt = optind;

        if (st.ordering == 0) // PERMUTE
        {
            if (st.first_nonopt != st.last_nonopt && st.last_nonopt != optind)
                exchange(av, st.first_nonopt, st.last_nonopt, optind);
            else if (st.last_nonopt != optind)
                st.first_nonopt = optind;

            while (optind < argc && is_nonoption(av[optind]))
                ++optind;
            st.last_nonopt = optind;
        }

        // "--"：提前结束选项解析；与 glibc 相同，把它当作一个"选项"与
        // 已跳过的裸参数交换，使其位于裸参数区之前，其后全部视为裸参数
        if (optind != argc && std::strcmp(av[optind], "--") == 0)
        {
            ++optind;
            if (st.first_nonopt != st.last_nonopt && st.last_nonopt != optind)
                exchange(av, st.first_nonopt, st.last_nonopt, optind);
            else if (st.first_nonopt == st.last_nonopt)
                st.first_nonopt = optind;
            st.last_nonopt = argc;
            optind = argc;
        }

        // 全部处理完毕：optind 回退到第一个裸参数，返回 -1
        if (optind >= argc)
        {
            if (st.first_nonopt != st.last_nonopt)
                optind = st.first_nonopt;
            return -1;
        }

        // 裸参数：REQUIRE_ORDER 停止；RETURN_IN_ORDER 作为选项 1 返回
        if (is_nonoption(av[optind]))
        {
            if (st.ordering == 1)
                return -1;
            optarg = av[optind];
            ++optind;
            return 1;
        }

        // ---- 长选项 ----
        if (longopts && av[optind][1] == '-')
        {
            st.nextchar = av[optind] + 2;
            return process_long_option(argc, av, optstring, longopts,
                                       longindex, st.nextchar);
        }

        // ---- 短选项：跳过前导 '-' ----
        st.nextchar = av[optind] + 1;
    }

    // ---- 处理当前短选项簇中的一个字符 ----
    {
        const char c = *st.nextchar;
        ++st.nextchar;
        const char *temp = std::strchr(optstring, c);

        // 处理到 token 末尾时前进 optind
        if (*st.nextchar == '\0')
            ++optind;

        if (temp == nullptr || c == ':' || c == ';')
        {
            if (should_print_errors(optstring))
                std::fprintf(stderr, "%s: invalid option -- '%c'\n", av[0], c);
            optopt = c;
            return '?';
        }

        if (temp[1] == ':')
        {
            if (temp[2] == ':')
            {
                // 可选参数：簇内剩余部分作为参数，否则参数为 nullptr
                if (*st.nextchar != '\0')
                {
                    optarg = const_cast<char *>(st.nextchar);
                    ++optind;
                }
                else
                {
                    optarg = nullptr;
                }
                st.nextchar = nullptr;
            }
            else
            {
                // 必选参数
                if (*st.nextchar != '\0')
                {
                    optarg = const_cast<char *>(st.nextchar);
                    ++optind;
                }
                else if (optind >= argc)
                {
                    if (should_print_errors(optstring))
                        std::fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                                     av[0], c);
                    optopt = c;
                    return colon_mode ? ':' : '?';
                }
                else
                {
                    optarg = av[optind];
                    ++optind;
                }
                st.nextchar = nullptr;
            }
        }
        return static_cast<unsigned char>(c);
    }
}

inline int getopt(int argc, char *const argv[], const char *optstring) noexcept
{
    return getopt_long(argc, argv, optstring, nullptr, nullptr);
}

} // extern "C"

#endif // LUOGU_EXPORT_UTIL_GETOPT_COMPAT_H
