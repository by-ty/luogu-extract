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
// 交互提示：题解抓取的爬取风险分级确认、限流暂停期间的用户选择。
//
// 所有需要用户确认的场合一律「失败闭合」：读不到输入（无 TTY、EOF）时
// 拒绝执行，绝不默认继续。输入源通过 Io 注入，测试可以在没有真实 TTY 的
// 情况下用脚本喂入 y / n / s / c 序列。
#ifndef LUOGU_EXTRACT_UTIL_PROMPT_H
#define LUOGU_EXTRACT_UTIL_PROMPT_H

#include <functional>
#include <string>

namespace prompt
{
    // 终端输入/等待的注入点（默认实现走 stdin 与 luogu::compat）
    struct Io
    {
        // 标准输入是否为交互式终端
        std::function<bool()> is_tty;
        // 读取一行（不含换行符）；返回 false 表示 EOF/读取失败
        std::function<bool(std::string &)> read_line;
        // 等待 ms 毫秒，期间可用按键中断；返回 true 表示被 on_key 中断
        std::function<bool(long, const std::function<bool(char)> &)> wait_ms;
    };

    // 默认 Io（真终端）。测试可调用 set_io 覆盖，restore_io 还原。
    Io default_io();
    void set_io(const Io &io);
    void restore_io();

    // 是否存在可交互的终端（失败闭合判定的依据）
    bool interactive();

    // 打印一行普通提示
    void print_line(const std::string &text);
    // 打印一行黄色警告（第 1 档风险提示等）
    void print_warning(const std::string &text);
    // 打印一行醒目的红色警告（第 4、5 档）
    void print_alert(const std::string &text);

    // 单次 [y/N] 确认：必须显式输入 y/Y 才返回 true；
    // 直接回车、其它输入、EOF 一律返回 false。
    // prompt_text 已含「… [y/N] 」时不再追加提示。
    bool confirm(const std::string &prompt_text);

    // 要求原样输入指定短语（第 5 档的最后一次确认）。
    // 必须完全一致（忽略首尾空白）才返回 true。
    bool confirm_phrase(const std::string &prompt_text, const std::string &phrase);

    // ---- 风险分级确认 ----

    // 计划阶段的计数与估算（用于分级与提示文案）
    struct RiskInfo
    {
        int level = 1;                 // 1~5
        int confirmations = 0;         // 本档需要的确认次数（已扣除 --yes）
        long long problems = 0;        // 选中题目数
        int per_problem = 1;           // 每题篇数上限（all 时为 all_limit）
        bool per_problem_all = false;  // --max-solutions all
        long long articles = 0;        // 本次实际待抓正文篇数 N（不含已命中缓存）
        long long cached_articles = 0; // 已命中缓存的篇数
        long long list_requests = 0;   // 需要重新获取列表的题目数 P
        long long total_requests = 0;  // N + P
        double seconds_per_request = 0;// 生效延时均值（秒）
        long long eta_seconds = 0;     // 预计耗时（不含图片下载）
    };

    // 按本次实际网络请求数（N + P）定档并算出确认次数（设计 §5.2 表）：
    //   档位 1（≤3）→0 次；2（4~5）→1 次；3（6~10）→2 次；
    //   4（11~20）→2 次；5（≥21）→3 次
    // --yes 只把确认次数减少 1 次，减到 0 为止（不能把第 5 档变为无需确认）。
    // 返回的 RiskInfo 只填好 level 与 confirmations，其余字段由调用方补齐。
    RiskInfo plan_risk(long long total_requests, bool yes);

    enum class ConfirmResult
    {
        Proceed,        // 可以开始抓取
        Cancelled,      // 用户拒绝（退出码 0）
        CannotConfirm,  // 无 TTY 且仍需确认（拒绝执行）
    };

    // 打印风险提示并按档位要求确认。返回 Proceed 时才继续抓取。
    ConfirmResult confirm_risk(const RiskInfo &info);

    // ---- 限流暂停期间的可中断等待 ----

    enum class WaitOutcome
    {
        Timeout,   // 等待结束（自动重试）
        Continue,  // 用户按 C 并二次确认后要求立即继续
        Stop,      // 用户按 S 要求立即停止
    };

    // 可中断等待 seconds 秒：
    // - 有 TTY 时期间可按 S（立即停止）/ C（确认后立即继续）；
    // - 无 TTY 时退化为纯倒计时（每 10 秒打印一次剩余时间）；
    // - 不识别其它按键，最多提示 3 次后走满倒计时。
    WaitOutcome wait_with_keys(long seconds, const std::string &reason);

    // 请求间隔的短等待（与 wait_with_keys 共用同一套可中断实现）：
    // 期间按 S 立即停止、按 C 跳过剩余等待。返回 false 表示用户要求停止。
    bool wait_delay(long ms);

    // 把秒数格式化为「1 分 40 秒」/「12 秒」/「约 90 分钟」
    std::string format_duration(long long seconds);

} // namespace prompt

#endif // LUOGU_EXTRACT_UTIL_PROMPT_H
