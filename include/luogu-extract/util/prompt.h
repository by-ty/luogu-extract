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

// include/luogu-extract/util/prompt.h
// 交互提示：风险分级确认与限流暂停等待。所有确认一律「失败闭合」：无 TTY 或 EOF 即拒绝执行，绝不默认继续；
// 输入源经 Io 注入，便于无 TTY 的测试脚本喂入 y / n / s / c。
#ifndef LUOGU_EXTRACT_UTIL_PROMPT_H
#define LUOGU_EXTRACT_UTIL_PROMPT_H

#include <functional>
#include <string>

namespace prompt
{
    // 终端输入/等待的注入点
    struct Io
    {
        // 标准输入是否为交互式终端
        std::function<bool()> is_tty;
        // 读一行（不含换行）；false = EOF/失败
        std::function<bool(std::string &)> read_line;
        // 等待 ms 毫秒（可被按键中断）；true = 被 on_key 中断
        std::function<bool(long, const std::function<bool(char)> &)> wait_ms;
    };

    // 默认 Io（真终端）；测试可用 set_io 覆盖、restore_io 还原
    Io default_io();
    void set_io(const Io &io);
    void restore_io();

    // 是否存在可交互终端（失败闭合判定的依据）
    bool interactive();

    // 提示行：普通 / 黄色（第 1 档）/ 醒目红色（第 4、5 档）
    void print_line(const std::string &text);
    void print_warning(const std::string &text);
    void print_alert(const std::string &text);

    // [y/N] 确认：只有显式 y/Y 为 true，回车、其它输入与 EOF 一律 false
    bool confirm(const std::string &prompt_text);

    // 要求原样输入指定短语（第 5 档最后确认）；忽略首尾空白、完全一致才 true
    bool confirm_phrase(const std::string &prompt_text, const std::string &phrase);

    // 计划阶段的计数与估算（用于分级与提示文案）
    struct RiskInfo
    {
        int level = 1;                 // 1~5
        int confirmations = 0;         // 本档确认次数（已扣除 --yes）
        long long problems = 0;
        int per_problem = 1;           // 每题篇数上限（all 时为 all_limit）
        bool per_problem_all = false;  // --max-solutions all
        long long articles = 0;        // 待抓正文篇数 N（题解 + 文章，不含已命中缓存）
        long long cached_articles = 0;
        long long solution_to_fetch = 0; // 其中题解正文的篇数
        long long cached_solutions = 0;
        // --article 指定的文章（含已缓存篇目）；与题解共用同一套抓取规则
        long long standalone_articles = 0;
        long long standalone_to_fetch = 0;
        long long cached_standalone = 0;
        long long list_requests = 0;   // 需重新获取列表的题目数 P
        long long total_requests = 0;  // N + P
        double seconds_per_request = 0;// 生效延时均值（秒）
        long long eta_seconds = 0;     // 预计耗时（不含图片下载）
    };

    // 按请求数（N + P）定档：≤3→0 次、4~5→1、6~10→2、11~20→2、≥21→3；--yes 只减 1 次
    // （第 5 档不能免确认）。返回的 RiskInfo 只填 level 与 confirmations
    RiskInfo plan_risk(long long total_requests, bool yes);

    enum class ConfirmResult
    {
        Proceed,        // 可以抓取
        Cancelled,      // 用户拒绝（退出码 0）
        CannotConfirm,  // 无 TTY 且需确认（拒绝执行）
    };

    // 打印风险提示并按档位确认；返回 Proceed 才继续
    ConfirmResult confirm_risk(const RiskInfo &info);

    enum class WaitOutcome
    {
        Timeout,   // 等待结束（自动重试）
        Continue,  // 按 C 并二次确认后立即继续
        Stop,      // 按 S 立即停止
    };

    // 可中断等待 seconds 秒：有 TTY 时按 S 停止 / 按 C 二次确认后继续；无 TTY 时纯倒计时（每 10 秒报剩余）；
    // 其它按键无效，最多提示 3 次
    WaitOutcome wait_with_keys(long seconds, const std::string &reason);

    // 请求间隔的短等待（实现同上）：S 停止、C 跳过剩余；false = 要求停止
    bool wait_delay(long ms);

    // 秒数格式化为「1 分 40 秒」/「12 秒」/「约 90 分钟」
    std::string format_duration(long long seconds);

} // namespace prompt

#endif // LUOGU_EXTRACT_UTIL_PROMPT_H
