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

// src/util/prompt.cpp
#include "luogu-extract/util/prompt.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include "luogu-extract/util/compat.h"

namespace
{
const char *kColorReset = "\033[0m";
const char *kColorYellow = "\033[1;33m";
const char *kColorAlert = "\033[1;41;37m";

// 第 5 档最后一次确认要求原样输入的短语（唯一的强化授权手段）
const char *kStrongPhrase = "I know what I am doing";

prompt::Io &current_io()
{
    static prompt::Io io = prompt::default_io();
    return io;
}

void out_line(const std::string &text)
{
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
} // namespace

prompt::Io prompt::default_io()
{
    Io io;
    io.is_tty = []() { return luogu::compat::stdin_is_tty(); };
    io.read_line = [](std::string &line) -> bool {
        std::string value;
        if (luogu::compat::read_line(stdin, value) < 0)
            return false;
        line = value;
        return true;
    };
    io.wait_ms = [](long ms, const std::function<bool(char)> &on_key) {
        return luogu::compat::sleep_interruptible_ms(ms, on_key);
    };
    return io;
}

void prompt::set_io(const Io &io) { current_io() = io; }
void prompt::restore_io() { current_io() = default_io(); }

bool prompt::interactive()
{
    const Io &io = current_io();
    return io.is_tty ? io.is_tty() : false;
}

void prompt::print_line(const std::string &text) { out_line(text); }

void prompt::print_warning(const std::string &text)
{
    out_line(std::string(kColorYellow) + text + kColorReset);
}

void prompt::print_alert(const std::string &text)
{
    out_line(std::string(kColorAlert) + text + kColorReset);
}

bool prompt::confirm(const std::string &prompt_text)
{
    std::fputs(prompt_text.c_str(), stdout);
    std::fflush(stdout);
    std::string line;
    if (!current_io().read_line || !current_io().read_line(line))
    {
        // 读不到输入：失败闭合（视为拒绝）
        out_line("");
        return false;
    }
    // 只认首个非空白字符为 y/Y（直接回车视为拒绝）
    for (char c : line)
    {
        if (c == ' ' || c == '\t' || c == '\r')
            continue;
        return c == 'y' || c == 'Y';
    }
    return false;
}

bool prompt::confirm_phrase(const std::string &prompt_text, const std::string &phrase)
{
    std::fputs(prompt_text.c_str(), stdout);
    std::fflush(stdout);
    std::string line;
    if (!current_io().read_line || !current_io().read_line(line))
    {
        out_line("");
        return false;
    }
    // 去掉首尾空白后必须与短语完全一致
    const size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;
    const size_t last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1) == phrase;
}

std::string prompt::format_duration(long long seconds)
{
    if (seconds < 0)
        seconds = 0;
    if (seconds < 60)
        return std::to_string(seconds) + " 秒";
    if (seconds < 3600)
    {
        const long long m = seconds / 60;
        const long long s = seconds % 60;
        if (s == 0)
            return std::to_string(m) + " 分钟";
        return std::to_string(m) + " 分 " + std::to_string(s) + " 秒";
    }
    const long long h = seconds / 3600;
    const long long m = (seconds % 3600) / 60;
    if (m == 0)
        return std::to_string(h) + " 小时";
    return std::to_string(h) + " 小时 " + std::to_string(m) + " 分";
}

prompt::RiskInfo prompt::plan_risk(long long total_requests, bool yes)
{
    RiskInfo info;
    if (total_requests <= 3)
        info.level = 1;
    else if (total_requests <= 5)
        info.level = 2;
    else if (total_requests <= 10)
        info.level = 3;
    else if (total_requests <= 20)
        info.level = 4;
    else
        info.level = 5;

    static const int kBaseConfirmations[] = {0, 1, 2, 2, 3};
    info.confirmations = kBaseConfirmations[info.level - 1];
    if (yes && info.confirmations > 0)
        --info.confirmations;
    info.total_requests = total_requests;
    return info;
}

prompt::ConfirmResult prompt::confirm_risk(const RiskInfo &info)
{
    // ---- 预计 0 次网络请求（全部命中缓存）：一行精简提示，不做任何确认 ----
    if (info.total_requests <= 0)
    {
        print_warning("[提示] " + std::to_string(info.problems) + " 道题的 " +
                      std::to_string(info.cached_articles) +
                      " 篇题解全部命中缓存，本次无需网络请求。");
        return ConfirmResult::Proceed;
    }

    // ---- 规模与耗时：三要素与估算，先给出便于用户当场决定 ----
    std::string scale_line = "即将抓取：";
    scale_line += std::to_string(info.problems) + " 道题 × 每题 ";
    scale_line += info.per_problem_all ? std::string("all（全部）")
                                       : std::to_string(info.per_problem) + " 篇";
    scale_line += " = " + std::to_string(info.articles + info.cached_articles) +
                  " 篇题解";
    if (info.cached_articles > 0)
        scale_line += "（" + std::to_string(info.cached_articles) +
                      " 篇已命中缓存，实际需抓 " + std::to_string(info.articles) +
                      " 篇）";
    if (info.list_requests > 0)
        scale_line += "；另有 " + std::to_string(info.list_requests) +
                      " 个题解列表需要获取";
    print_line(scale_line);

    if (info.total_requests > 0)
    {
        char rate[64];
        std::snprintf(rate, sizeof(rate), "%.1f", info.seconds_per_request);
        print_line("预计网络请求 " + std::to_string(info.total_requests) +
                   " 次，平均 " + rate + " 秒/次 ⇒ 约 " +
                   format_duration(info.eta_seconds) + "（不含图片下载）");
    }

    const std::string advice =
        "题解版权归原作者，抓取频率与后果由你自行承担（后果自负）。\n"
        "建议：先用 --max-solutions 1 与少量题号试跑，确认可用后再扩大范围。";
    const std::string shrink =
        "可用 --max-solutions 1、--pid 或 --pid-range 缩小范围；中断后重跑会自动续传";

    // ---- 第 1 档（含缓存全命中的降级）：提示后直接继续 ----
    if (info.level <= 1)
    {
        print_warning("[提示] 本次抓取量很小（" +
                      std::to_string(info.total_requests) +
                      " 次网络请求），无需风险确认。" + shrink);
        return ConfirmResult::Proceed;
    }

    // ---- --yes 把确认次数减到 0：仍然重新打印风险要点后继续 ----
    if (info.confirmations <= 0)
    {
        print_warning("[警告] 本次将抓取 " + std::to_string(info.articles) +
                      " 篇题解（共 " + std::to_string(info.total_requests) +
                      " 次网络请求）；--yes 已将确认次数减少 1 次，"
                      "无需确认，直接继续。");
        print_line(advice);
        return ConfirmResult::Proceed;
    }

    // ---- 无 TTY 且仍需确认：失败闭合，绝不默认继续 ----
    if (!interactive())
    {
        print_alert("[拒绝执行] 当前不是交互终端，风险确认无法进行。");
        print_line("本次共需 " + std::to_string(info.confirmations) +
                   " 次风险确认，但标准输入不是交互终端，无法读取你的确认。");
        print_line("请改用交互终端运行，或用 --yes 减少确认次数后重试"
                   "（--yes 只减少 1 次；第 5 档仍需在交互终端完成确认）。");
        print_line(shrink);
        return ConfirmResult::CannotConfirm;
    }

    const int total_confirm = info.confirmations;
    for (int i = 1; i <= total_confirm; ++i)
    {
        // 每次确认之间重新打印风险要点（不做刷屏式重复）
        if (info.level >= 5)
        {
            print_alert("[醒目警告] 本次将抓取 " + std::to_string(info.articles) +
                        " 篇题解，严重超出常规使用范围，不推荐这么做。");
            print_line("           频繁请求可能导致账号被临时封禁，后果自负。");
        }
        else if (info.level == 4)
        {
            print_alert("[严厉警告] 本次将抓取 " + std::to_string(info.articles) +
                        " 篇题解，请求量较大，请确认这确实是你需要的。");
            print_line("           短时间内大量请求可能触发限流，甚至导致账号被临时封禁。");
        }
        else if (info.level == 3)
        {
            print_warning("[警告] 本次将抓取 " + std::to_string(info.articles) +
                          " 篇题解，会连续发出数百次以内的网络请求。");
        }
        else
        {
            print_warning("[警告] 本次将抓取 " + std::to_string(info.articles) +
                          " 篇题解，请注意请求频率。");
        }
        print_line(advice);

        // 第 5 档的最后一次确认要求原样输入指定短语
        if (info.level >= 5 && i == total_confirm)
        {
            const std::string prompt_text =
                "第三次确认（" + std::to_string(i) + "/" +
                std::to_string(total_confirm) + "）：请输入「" + kStrongPhrase +
                "」以继续（原样输入，回车取消）：";
            if (!confirm_phrase(prompt_text, kStrongPhrase))
            {
                print_line("已按你的选择取消");
                return ConfirmResult::Cancelled;
            }
            continue;
        }

        const std::string prompt_text =
            "第 " + std::to_string(i) + " 次确认（" + std::to_string(i) + "/" +
            std::to_string(total_confirm) + "）：确认继续抓取？[y/N] ";
        if (!confirm(prompt_text))
        {
            print_line("已按你的选择取消");
            return ConfirmResult::Cancelled;
        }
    }

    return ConfirmResult::Proceed;
}

bool prompt::wait_delay(long ms)
{
    if (ms <= 0)
        return true;
    bool stop = false;
    current_io().wait_ms(ms, [&](char key) -> bool {
        if (key == 's' || key == 'S')
        {
            stop = true;
            return true;
        }
        // 按 C 跳过剩余等待（请求间隔很短，不需要二次确认）
        return key == 'c' || key == 'C';
    });
    if (stop)
        print_line("已按你的选择停止抓取（已抓取的缓存全部保留）。");
    return !stop;
}

prompt::WaitOutcome prompt::wait_with_keys(long seconds, const std::string &reason)
{
    if (seconds <= 0)
        return WaitOutcome::Timeout;

    const bool tty = interactive();
    print_warning("[限流] 已暂停爬取，等待 " + format_duration(seconds) +
                  " 后自动重试");
    if (!reason.empty())
        print_line("       原因：" + reason);
    if (tty)
        print_line("       等待期间可按 S 立即停止，按 C 立即继续（有风险）");
    else
        print_line("       当前不是交互终端，无法中途干预，将等待满 "
                   + format_duration(seconds) + " 后自动重试");

    long long remaining_ms = seconds * 1000;
    int unknown_keys = 0;
    const long long report_interval_ms = tty ? 30000 : 10000;
    long long next_report_ms = remaining_ms - report_interval_ms;

    while (remaining_ms > 0)
    {
        const long chunk = static_cast<long>(std::min<long long>(200, remaining_ms));
        bool stop = false;
        bool resume = false;
        current_io().wait_ms(chunk, [&](char key) -> bool {
            if (key == 's' || key == 'S')
            {
                stop = true;
                return true;
            }
            if (key == 'c' || key == 'C')
            {
                resume = true;
                return true;
            }
            // 其它按键：不识别则重新提示，最多 3 次
            if (unknown_keys < 3)
            {
                ++unknown_keys;
                print_line("       未识别的按键：请按 S 立即停止，或按 C 立即继续");
            }
            return false;
        });
        remaining_ms -= chunk;

        if (stop)
        {
            print_line("已按你的选择停止抓取（已抓取的缓存全部保留）。");
            return WaitOutcome::Stop;
        }
        if (resume)
        {
            print_line("");
            const bool go = confirm("提前继续可能加重风控甚至导致账号被临时封禁，"
                                    "后果自负。确认继续？[y/N] ");
            if (go)
                return WaitOutcome::Continue;
            print_line("已取消提前继续，回到等待倒计时。");
        }
        if (remaining_ms <= next_report_ms && remaining_ms > 0)
        {
            print_line("       仍在等待，剩余 " + format_duration((remaining_ms + 999) / 1000));
            next_report_ms = remaining_ms - report_interval_ms;
        }
    }
    return WaitOutcome::Timeout;
}
