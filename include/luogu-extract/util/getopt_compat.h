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

// include/luogu-extract/util/getopt_compat.h
// 平台无关的 getopt / getopt_long，按 glibc 语义移植（Windows 无 POSIX <getopt.h>）；非 Windows 默认用系统头，
// 定义 LUOGU_FORCE_COMPAT_GETOPT 可强制本实现（测试用）。要点：
// - optstring 前导 ':' 不打印错误（未知 '?'、缺参数 ':'），'+' 遇首个裸参数即停，'-' 把裸参数作为选项 1 返回；
// - 短选项簇、--name[=value]、长选项无歧义前缀缩写（歧义时精确匹配优先）、必选参数会吞掉下一个 token；
// - GNU 式重排：返回 -1 后 argv[optind..argc) 全是裸参数（"--" 之后亦然），出错时 argv[optind-1] 指向出错 token。
#ifndef LUOGU_EXTRACT_UTIL_GETOPT_COMPAT_H
#define LUOGU_EXTRACT_UTIL_GETOPT_COMPAT_H

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

// 与 glibc/POSIX 相同的 C 链接（非 Windows 强制用本实现时会与系统头文件声明同名符号）
extern "C"
{

inline char *optarg = nullptr;
inline int optind = 1;
inline int opterr = 1;
inline int optopt = '?';

namespace detail
{
    struct PermuteState
    {
        int first_nonopt = 1;   // 已跳过的裸参数区起始
        int last_nonopt = 1;    // 已跳过的裸参数区末尾
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

    // 移植 glibc 的 exchange：交换相邻块 [first_nonopt, last_nonopt)（已跳过的裸参数）与
    // [last_nonopt, optind)（期间的选项），保持块内顺序并更新记录
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

    // 等价 glibc：opterr 非 0 且 optstring 不以 ':' 开头才打印错误
    inline bool should_print_errors(const char *optstring)
    {
        return opterr != 0 && optstring[0] != ':';
    }

    // 长选项处理（移植 glibc process_long_option，long_only 恒为 false）
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

        // 1. 精确匹配优先于前缀缩写（即使存在多个前缀匹配项）
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

        // 2. 无精确匹配时找前缀缩写；多个候选且 (has_arg, flag, val) 不同即歧义
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
                    std::fprintf(stderr, "%s: 长选项 '--%.*s' 存在歧义，可能是：%s\n",
                                 argv[0], static_cast<int>(name_len), name,
                                 ambig_list.c_str());
                }
                st.nextchar = nullptr;
                ++optind;
                optopt = 0;
                return '?';
            }
        }

        if (pfound == nullptr)
        {
            if (print_errors)
                std::fprintf(stderr, "%s: 无法识别的长选项 '--%.*s'\n",
                             argv[0], static_cast<int>(name_len), name);
            st.nextchar = nullptr;
            ++optind;
            optopt = 0;
            return '?';
        }

        ++optind;
        st.nextchar = nullptr;
        if (name[name_len] == '=')
        {
            if (pfound->has_arg)
            {
                optarg = const_cast<char *>(name + name_len + 1);
            }
            else
            {
                if (print_errors)
                    std::fprintf(stderr, "%s: 长选项 '--%s' 不接受参数值\n",
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
                    std::fprintf(stderr, "%s: 长选项 '--%s' 需要参数值\n",
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
            st.ordering = 2;
        else if (optstring[0] == '+')
            st.ordering = 1;
        else
            st.ordering = 0;
    }
    // 后续调用时 optstring 前缀符号已在初始化时消费，跳过
    if (optstring[0] == '-' || optstring[0] == '+')
        ++optstring;
    const bool colon_mode = (optstring[0] == ':');

    if (st.nextchar == nullptr || *st.nextchar == '\0')
    {
        // 用户可能手动回退过 optind：把记录区间收敛到有效范围
        if (st.last_nonopt > optind)
            st.last_nonopt = optind;
        if (st.first_nonopt > optind)
            st.first_nonopt = optind;

        if (st.ordering == 0)
        {
            if (st.first_nonopt != st.last_nonopt && st.last_nonopt != optind)
                exchange(av, st.first_nonopt, st.last_nonopt, optind);
            else if (st.last_nonopt != optind)
                st.first_nonopt = optind;

            while (optind < argc && is_nonoption(av[optind]))
                ++optind;
            st.last_nonopt = optind;
        }

        // "--"：同 glibc，交换到裸参数区之前，其后全为裸参数
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

        // 长选项
        if (longopts && av[optind][1] == '-')
        {
            st.nextchar = av[optind] + 2;
            return process_long_option(argc, av, optstring, longopts,
                                       longindex, st.nextchar);
        }

        st.nextchar = av[optind] + 1;
    }

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
                std::fprintf(stderr, "%s: 无效的选项 -- '%c'\n", av[0], c);
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
                if (*st.nextchar != '\0')
                {
                    optarg = const_cast<char *>(st.nextchar);
                    ++optind;
                }
                else if (optind >= argc)
                {
                    if (should_print_errors(optstring))
                        std::fprintf(stderr, "%s: 选项缺少参数值 -- '%c'\n",
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

// 重新开始一次参数解析（同一进程内多次调用 getopt_long 前必须复位）。本实现除 optind 外还存有重排状态
// （first_nonopt / last_nonopt / nextchar），不一并复位的话第二次解析会把 argv[0] 当成裸参数。
inline void getopt_reset()
{
    optind = 1;
    optarg = nullptr;
    optopt = '?';
    detail::permute_state() = detail::PermuteState();
}

#endif // LUOGU_EXTRACT_UTIL_GETOPT_COMPAT_H
