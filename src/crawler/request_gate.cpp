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

// src/crawler/request_gate.cpp
#include "luogu-extract/crawler/request_gate.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <curl/curl.h>
#include "luogu-extract/util/compat.h"
#include "luogu-extract/util/cookie.h"
#include "luogu-extract/util/prompt.h"

namespace
{
// 重定向到文件/管道时不写 ANSI 转义序列（这些提示走 stderr）
const char *kColorReset = luogu::compat::stderr_is_tty() ? "\033[0m" : "";
const char *kColorRed = luogu::compat::stderr_is_tty() ? "\033[1;31m" : "";

std::string to_lower(std::string s)
{
    for (auto &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string &s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// 单侧封顶：自动递增后的延时不超过 60 秒，但用户显式配置的更大值仍然有效
const long kScaleCapMs = 60000;

// 连续超时/连接失败达到该次数时按疑似限流处理
const int kConsecutiveFailuresAsRateLimit = 5;

// Retry-After 的硬上限（秒）：该值来自服务器，恶意或异常的响应可以给得很大，
// 不限幅就会把本次运行的等待时间拉到数天（用户显式配置的 --rate-limit-wait
// 不受此上限约束，只有响应头里的 Retry-After 受限）
const long kMaxRetryAfterSec = 3600;

// ---- 运行时状态 ----
// 原站与保存站各一条通道，各自维护延时系数、限流等待与放弃状态；
// auto 模式下两条通道并行工作，因此所有共享状态都要加锁。

struct ChannelState
{
    crawler::GateConfig config;
    long long planned_requests = 0;
    double planned_scale = 1.0;
    bool boosted = false;          // 限流后是否已额外放大
    int rate_limit_rounds = 0;     // 已触发的等待轮数（交互式等待模式）
    int consecutive_failures = 0;  // 连续超时/连接失败次数
    int strikes = 0;               // 连续被限流次数（auto 模式，达到 3 次即放弃）
    bool abandoned = false;        // 本通道已放弃
    std::chrono::steady_clock::time_point blocked_until{}; // 限流等待截止时间
    std::string limit_reason;      // 最近一次限流原因
    std::mt19937 rng;              // 每通道独立的抖动发生器
};

struct GateState
{
    ChannelState channels[crawler::kChannelCount];

    luogu::cookie::Jar jar;
    std::string cookie_file_hint;
    unsigned long long jar_version = 0;  // jar 每变化一次 +1（句柄据此决定是否重装）

    bool user_stopped = false;
    bool rate_limited_out = false;
    std::string rate_limit_reason;

    std::mutex mutex;   // 保护上面的所有状态（两条通道并行时会并发访问）
    std::mutex print;   // 串行化终端输出

    GateState()
    {
        // 每条通道的抖动发生器各用一个随机种子（静态对象构造时执行一次；
        // 这里用构造函数而不是 std::call_once，避免 lambda 捕获静态变量）
        std::random_device rd;
        for (int i = 0; i < crawler::kChannelCount; ++i)
            channels[i].rng.seed(rd());
    }
};

GateState &state()
{
    static GateState s;
    return s;
}

ChannelState &channel_state(crawler::Channel ch)
{
    const int index = static_cast<int>(ch);
    return state().channels[index < 0 || index >= crawler::kChannelCount ? 0 : index];
}

// 统一加锁访问（返回副本，避免调用方在锁外继续持有引用）。
// 因此取共享状态的接口（如 gate_channel_limit_reason）必须返回值而不是引用，
// 否则出锁后另一条线程改写该字段就会与调用方的读竞争
template <typename Fn>
auto with_channel(crawler::Channel ch, Fn &&fn) -> decltype(fn(std::declval<ChannelState &>()))
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return fn(channel_state(ch));
}

void print_error(const std::string &message)
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.print);
    std::fflush(stdout);
    std::fprintf(stderr, "%s错误：%s %s\n", kColorRed, kColorReset, message.c_str());
}

// 线程安全的一行输出
void print_line(const std::string &message)
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.print);
    std::fflush(stdout);
    std::printf("%s\n", message.c_str());
    std::fflush(stdout);
}

double effective_scale(ChannelState &st)
{
    double scale = st.config.auto_scale ? st.planned_scale : 1.0;
    if (st.config.auto_scale && st.boosted)
        scale *= 1.5;
    return scale;
}

// 生效区间：基础区间乘以系数，单侧封顶 60 秒（用户显式配置的更大值仍有效）
std::pair<long, long> effective_delay_ms_of(ChannelState &st)
{
    const double scale = effective_scale(st);
    const long cap = std::max<long>(kScaleCapMs, st.config.delay.max_ms);
    long lo = static_cast<long>(static_cast<double>(st.config.delay.min_ms) * scale);
    long hi = static_cast<long>(static_cast<double>(st.config.delay.max_ms) * scale);
    lo = std::min(lo, cap);
    hi = std::min(hi, cap);
    if (lo > hi)
        lo = hi;
    return {lo, hi};
}

// 在 [lo, hi] 闭区间内均匀取一个毫秒值（std::mt19937 +
// std::uniform_int_distribution，标准库实现，跨平台结果一致）
long random_delay_ms(ChannelState &st, long lo, long hi)
{
    if (hi <= lo)
        return lo;
    std::uniform_int_distribution<long> dist(lo, hi);
    return dist(st.rng);
}

bool looks_like_html(const std::string &body)
{
    const std::string head = to_lower(body.substr(0, 512));
    return head.find("<!doctype html") != std::string::npos ||
           head.find("<html") != std::string::npos;
}

// 响应体中的风控特征串。只在「非 200」或「200 且是 HTML」时才检查，
// 避免题解正文里恰好出现「请稍后再试」这类短语时被误判为限流。
bool body_has_rate_limit_mark(const std::string &body)
{
    static const char *kMarks[] = {
        "操作过于频繁", "请求过于频繁", "请稍后再试", "访问受限",
        "验证码", "Too Many Requests", "rate limit", "Rate Limit",
    };
    for (const char *mark : kMarks)
        if (body.find(mark) != std::string::npos)
            return true;
    return false;
}

// 极简 JSON 取值：从形如 {"status":401,...} 的错误模板中取出 status
bool json_status_of(const std::string &body, long &status)
{
    const size_t pos = body.find("\"status\"");
    if (pos == std::string::npos || pos > 256)
        return false;
    size_t i = body.find(':', pos);
    if (i == std::string::npos)
        return false;
    ++i;
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t'))
        ++i;
    size_t j = i;
    while (j < body.size() && std::isdigit(static_cast<unsigned char>(body[j])))
        ++j;
    if (j == i)
        return false;
    status = std::strtol(body.substr(i, j - i).c_str(), nullptr, 10);
    return true;
}

bool body_is_login_error(const std::string &body)
{
    return body.find("UserUnloginException") != std::string::npos ||
           body.find("\"needLogin\":1") != std::string::npos;
}

// ---- curl 回调 ----

struct ResponseBuffer
{
    std::string data;
    size_t max_bytes = 4 * 1024 * 1024;
    bool overflow = false;
};

size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    const size_t total = size * nmemb;
    auto *buf = static_cast<ResponseBuffer *>(userp);
    if (total > buf->max_bytes || buf->data.size() > buf->max_bytes - total)
    {
        buf->overflow = true;
        return 0; // 中止传输
    }
    buf->data.append(static_cast<char *>(contents), total);
    return total;
}

struct HeaderSink
{
    std::string etag;
    std::string retry_after;
    long status = 0;
};

size_t header_cb(char *buffer, size_t size, size_t nitems, void *userp)
{
    const size_t total = size * nitems;
    auto *sink = static_cast<HeaderSink *>(userp);
    const std::string line(buffer, total);

    // 状态行："HTTP/2 200"
    if (line.compare(0, 5, "HTTP/") == 0)
    {
        const size_t sp = line.find(' ');
        if (sp != std::string::npos)
        {
            const long code = std::strtol(line.c_str() + sp + 1, nullptr, 10);
            if (code > 0)
                sink->status = code;
        }
        return total;
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos)
        return total;
    const std::string name = to_lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (name == "etag")
        sink->etag = value;
    else if (name == "retry-after")
        sink->retry_after = value;
    return total;
}

// Retry-After 支持秒数与 HTTP-date 两种形式；解析失败返回 0。
// 秒数上限 kMaxRetryAfterSec：这个值完全由服务器控制，不限幅（"Retry-After:
// 999999999" 或超出 long 范围的数字）会让本次运行等待数天，甚至因收窄转换
// 溢出变成负数而被当成 --rate-limit-wait 0。负数与非数字仍然返回 0。
long parse_retry_after(const std::string &value)
{
    const std::string v = trim(value);
    if (v.empty())
        return 0;
    char *end = nullptr;
    const long seconds = std::strtol(v.c_str(), &end, 10);
    if (end && *end == '\0' && seconds > 0)
        return std::min(seconds, kMaxRetryAfterSec);
    return 0;
}
} // namespace

// ---------------------------------------------------------------------------

const crawler::Endpoints &crawler::endpoints()
{
    static Endpoints ep = []() {
        Endpoints e;
        // 环境变量覆盖仅用于本地 mock 测试（帮助文本中不列出）
        const std::string official =
            luogu::compat::getenv_utf8("LUOGU_EXTRACT_BASE_OFFICIAL");
        if (!official.empty())
            e.official_base = official;
        const std::string save =
            luogu::compat::getenv_utf8("LUOGU_EXTRACT_BASE_SAVE");
        if (!save.empty())
            e.save_base = save;
        while (!e.official_base.empty() && e.official_base.back() == '/')
            e.official_base.pop_back();
        while (!e.save_base.empty() && e.save_base.back() == '/')
            e.save_base.pop_back();
        return e;
    }();
    return ep;
}

bool crawler::parse_delay_spec(const std::string &spec, DelaySpec &out, std::string &error)
{
    error.clear();
    const std::string value = trim(spec);
    if (value.empty())
    {
        error = "参数 '--request-delay' 后缺少间隔秒数；正确用法："
                "--request-delay <平均秒数>（如 5）或 --request-delay <最小>-<最大>"
                "（如 8-15）";
        return false;
    }

    auto parse_seconds = [](const std::string &text, double &seconds) -> bool {
        if (text.empty())
            return false;
        char *end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || (end && *end != '\0'))
            return false;
        if (v <= 0.0)
            return false;
        seconds = v;
        return true;
    };

    const size_t dash = value.find('-');
    double lo = 0.0, hi = 0.0;
    if (dash == std::string::npos)
    {
        double mean = 0.0;
        if (!parse_seconds(value, mean))
        {
            error = "参数 '--request-delay' 的值 '" + spec +
                    "' 不是合法的秒数；正确用法：--request-delay <平均秒数>"
                    "（正数，可带小数，如 5 或 2.5）";
            return false;
        }
        // 均值 ±30% 均匀抖动
        lo = mean * 0.7;
        hi = mean * 1.3;
    }
    else
    {
        const std::string a = trim(value.substr(0, dash));
        const std::string b = trim(value.substr(dash + 1));
        if (b.find('-') != std::string::npos || !parse_seconds(a, lo) ||
            !parse_seconds(b, hi))
        {
            error = "参数 '--request-delay' 的值 '" + spec +
                    "' 不是合法的区间；正确用法：--request-delay <最小>-<最大>"
                    "（如 8-15，两端均为正数且最小不超过最大）";
            return false;
        }
        if (lo > hi)
        {
            error = "参数 '--request-delay' 的区间左端点不能大于右端点（'" + spec +
                    "'）；正确用法：--request-delay <最小>-<最大>（如 8-15）";
            return false;
        }
    }

    const double kMaxSeconds = 300.0;
    if (hi > kMaxSeconds)
    {
        error = "参数 '--request-delay' 的间隔上限为 300 秒，'" + spec +
                "' 超过上限；正确用法：--request-delay 5 或 "
                "--request-delay 8-15（单次间隔不超过 300 秒）";
        return false;
    }

    out.min_ms = static_cast<long>(lo * 1000.0 + 0.5);
    out.max_ms = static_cast<long>(hi * 1000.0 + 0.5);
    if (out.min_ms < 1)
        out.min_ms = 1;
    if (out.max_ms < out.min_ms)
        out.max_ms = out.min_ms;
    return true;
}

double crawler::auto_scale_for(long long planned_requests)
{
    if (planned_requests <= 20)
        return 1.0;
    if (planned_requests <= 99)
        return 1.5;
    if (planned_requests <= 299)
        return 2.0;
    return 3.0;
}

void crawler::gate_configure_channel(Channel ch, const GateConfig &cfg,
                                      long long planned_requests)
{
    with_channel(ch, [&](ChannelState &st) {
        st.config = cfg;
        st.planned_requests = planned_requests;
        st.planned_scale = auto_scale_for(planned_requests);
        st.boosted = false;
        st.rate_limit_rounds = 0;
        st.consecutive_failures = 0;
        st.strikes = 0;
        st.abandoned = false;
        st.blocked_until = std::chrono::steady_clock::time_point{};
        st.limit_reason.clear();
        return 0;
    });
}

void crawler::gate_configure(const GateConfig &cfg, long long planned_requests)
{
    gate_configure_channel(Channel::Official, cfg, planned_requests);
}

void crawler::gate_set_planned_requests(Channel ch, long long planned_requests)
{
    with_channel(ch, [&](ChannelState &st) {
        st.planned_requests = planned_requests;
        st.planned_scale = auto_scale_for(planned_requests);
        return 0;
    });
}

void crawler::gate_set_planned_requests(long long planned_requests)
{
    gate_set_planned_requests(Channel::Official, planned_requests);
}

std::pair<long, long> crawler::gate_effective_delay_ms(Channel ch)
{
    return with_channel(ch, [](ChannelState &st) { return effective_delay_ms_of(st); });
}

double crawler::gate_effective_delay_seconds(Channel ch)
{
    const auto range = gate_effective_delay_ms(ch);
    return (static_cast<double>(range.first) + static_cast<double>(range.second)) /
           2000.0;
}

double crawler::gate_scale(Channel ch)
{
    return with_channel(ch, [](ChannelState &st) { return effective_scale(st); });
}

bool crawler::gate_scale_boosted()
{
    return with_channel(Channel::Official,
                        [](ChannelState &st) { return st.boosted; });
}

// ---- 限流记账（auto 模式的两站点调度器使用）----

crawler::ChannelLimitInfo crawler::gate_note_rate_limit(Channel ch,
                                                        const std::string &reason)
{
    return with_channel(ch, [&](ChannelState &st) {
        ChannelLimitInfo info;
        info.reason = reason;
        st.limit_reason = reason;
        ++st.strikes;
        if (st.strikes >= 3)
        {
            st.abandoned = true;
            info.abandoned = true;
            info.wait_ms = 0;
        }
        else
        {
            info.wait_ms = static_cast<long>(st.config.rate_limit_wait_sec) * 1000;
            // 运行中出现限流后，本通道剩余请求的延时额外 ×1.5
            if (st.config.auto_scale && !st.boosted)
                st.boosted = true;
            if (info.wait_ms > 0)
                st.blocked_until = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(info.wait_ms);
            else
                st.blocked_until = std::chrono::steady_clock::time_point{};
        }
        return info;
    });
}

void crawler::gate_note_success(Channel ch)
{
    with_channel(ch, [](ChannelState &st) {
        st.strikes = 0;
        return 0;
    });
}

void crawler::gate_abandon_channel(Channel ch)
{
    with_channel(ch, [](ChannelState &st) {
        st.abandoned = true;
        return 0;
    });
}

bool crawler::gate_channel_abandoned(Channel ch)
{
    return with_channel(ch, [](ChannelState &st) { return st.abandoned; });
}

long long crawler::gate_channel_block_remaining_ms(Channel ch)
{
    return with_channel(ch, [](ChannelState &st) -> long long {
        if (st.abandoned)
            return 0;
        const auto now = std::chrono::steady_clock::now();
        if (st.blocked_until <= now)
            return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(st.blocked_until -
                                                                    now)
            .count();
    });
}

std::string crawler::gate_channel_limit_reason(Channel ch)
{
    // 必须返回值：with_channel 出锁后，另一条线程可能正在改写 st.limit_reason
    return with_channel(ch, [](ChannelState &st) { return st.limit_reason; });
}

void crawler::gate_reset_channel(Channel ch)
{
    with_channel(ch, [](ChannelState &st) {
        st.strikes = 0;
        st.abandoned = false;
        st.blocked_until = std::chrono::steady_clock::time_point{};
        st.limit_reason.clear();
        return 0;
    });
}

bool crawler::gate_all_channels_abandoned()
{
    for (int i = 0; i < kChannelCount; ++i)
        if (!gate_channel_abandoned(static_cast<Channel>(i)))
            return false;
    return true;
}

void crawler::gate_print_delay_notice(long long planned_requests)
{
    const GateConfig cfg = with_channel(Channel::Official,
                                        [](ChannelState &st) { return st.config; });
    const double scale = gate_scale(Channel::Official);
    const auto range = gate_effective_delay_ms(Channel::Official);
    auto fmt = [](long ms) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(ms) / 1000.0);
        return std::string(buf);
    };

    if (!cfg.auto_scale || planned_requests <= 20)
    {
        if (!cfg.auto_scale)
            std::printf("本次计划 %lld 次网络请求，延时为 %.1f~%.1f 秒"
                        "（已用 --no-delay-auto-scale 关闭自动递增）\n",
                        planned_requests,
                        static_cast<double>(cfg.delay.min_ms) / 1000.0,
                        static_cast<double>(cfg.delay.max_ms) / 1000.0);
        else
            std::printf("本次计划 %lld 次网络请求，延时 %.1f~%.1f 秒\n",
                        planned_requests, static_cast<double>(range.first) / 1000.0,
                        static_cast<double>(range.second) / 1000.0);
        return;
    }
    std::printf("本次计划 %lld 次网络请求，延时自动提升为 %s~%s 秒"
                "（系数 %.1f，封顶 60 秒）\n",
                planned_requests, fmt(range.first).c_str(), fmt(range.second).c_str(),
                scale);
}

// ---- 凭据通道 ----

namespace
{
// 清空两个请求句柄的 Cookie 引擎（定义见下方「长连接句柄」一节，
// gate_clear_cookies 在它之前，故先声明）
void clear_cookie_engines();
} // namespace

bool crawler::gate_load_cookies(const std::filesystem::path &file, std::string &error,
                                std::string *warnings)
{
    GateState &s = state();
    luogu::cookie::Jar jar;
    if (!luogu::cookie::load_netscape_file(file, jar, error, warnings))
        return false;
    const std::string hint = luogu::compat::path_to_utf8(file);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.jar = std::move(jar);
    s.cookie_file_hint = hint;
    ++s.jar_version;  // 新 jar 需要重新装进句柄引擎
    return true;
}

bool crawler::gate_set_cookie_string(const std::string &text, std::string &error)
{
    GateState &s = state();
    luogu::cookie::Jar jar;
    if (!luogu::cookie::load_cookie_string(text, jar, error))
        return false;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.jar = std::move(jar);
    s.cookie_file_hint = "--cookie-string";
    ++s.jar_version;  // 新 jar 需要重新装进句柄引擎
    return true;
}

size_t crawler::gate_cookie_count()
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.jar.size();
}

bool crawler::gate_has_cookies()
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return !s.jar.empty();
}

void crawler::gate_clear_cookies()
{
    GateState &s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.jar.cookies.clear();
        s.cookie_file_hint.clear();
        ++s.jar_version;  // jar 已变化：句柄下次请求按新版本重新安装
    }
    // jar 只是「待安装的凭据」：已经装进 libcurl 引擎的 Cookie 必须显式清掉，
    // 否则后续请求仍会带着旧凭据（auto 模式下两条通道会并发读写 jar，
    // 因此先出 s.mutex 再取句柄锁，两把锁不交叉持有）
    clear_cookie_engines();
}

std::string crawler::gate_cookie_file_hint()
{
    // 返回值拷贝：cookie_file_hint 会被 gate_load_cookies 在锁内改写，
    // 返回引用会让调用方在锁外读到正在被改写的 std::string（与 limit_reason 同理）
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.cookie_file_hint;
}

// ---- 请求执行 ----

namespace
{
// 限流暂停流程（设计 §8.2）：最多触发 max_rate_limit_rounds 轮等待，
// 每轮等待结束后重试当前请求；仍被拒绝则直接停止（不再等待、不再重试）。
// 返回 true 表示「已等待完毕，可以重试当前请求」。
bool handle_rate_limit(crawler::Channel ch, const std::string &reason)
{
    GateState &s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.rate_limit_reason = reason;
    }
    const int wait_sec = with_channel(ch, [](ChannelState &st) {
        return st.config.rate_limit_wait_sec;
    });
    const int rounds_used = with_channel(ch, [](ChannelState &st) {
        return st.rate_limit_rounds;
    });
    const int max_rounds = with_channel(ch, [](ChannelState &st) {
        return st.config.max_rate_limit_rounds;
    });

    if (wait_sec <= 0)
    {
        print_error("检测到限流（" + reason +
                    "），且 --rate-limit-wait 为 0：不再等待，直接停止本次抓取。"
                    "已抓取的缓存全部保留，可稍后重跑续传。");
        with_channel(ch, [](ChannelState &st) {
            st.abandoned = true;
            return 0;
        });
        std::lock_guard<std::mutex> lock(state().mutex);
        state().rate_limited_out = true;
        return false;
    }
    if (rounds_used >= max_rounds)
    {
        print_error("本次运行已触发 " + std::to_string(rounds_used) +
                    " 轮限流等待（上限 " + std::to_string(max_rounds) +
                    " 轮），仍被限流，已停止本次抓取。"
                    "已抓取的缓存全部保留，可稍后重跑续传。");
        with_channel(ch, [](ChannelState &st) {
            st.abandoned = true;
            return 0;
        });
        std::lock_guard<std::mutex> lock(state().mutex);
        state().rate_limited_out = true;
        return false;
    }

    with_channel(ch, [](ChannelState &st) {
        ++st.rate_limit_rounds;
        return 0;
    });
    const prompt::WaitOutcome outcome = prompt::wait_with_keys(wait_sec, reason);
    if (outcome == prompt::WaitOutcome::Stop)
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().user_stopped = true;
        return false;
    }

    // 运行中出现限流后，本次运行剩余请求的延时额外 ×1.5
    // （局部放大，不回写缓存与配置；--no-delay-auto-scale 时不做任何放大）
    const bool boosted_now = with_channel(ch, [](ChannelState &st) {
        if (st.config.auto_scale && !st.boosted)
        {
            st.boosted = true;
            return true;
        }
        return false;
    });
    if (boosted_now)
    {
        const auto range = crawler::gate_effective_delay_ms(ch);
        print_line("已按限流处理：本次运行剩余请求的延时额外 ×1.5，调整为 " +
                   std::to_string(static_cast<double>(range.first) / 1000.0) + "~" +
                   std::to_string(static_cast<double>(range.second) / 1000.0) +
                   " 秒");
    }
    return true;
}

// 是否携带 Cookie：既要调用方要求（只有洛谷原站请求会要求），
// 又要目标域名在白名单内（luogu.com.cn / luogu.org 及其子域，
// 或端点被环境变量覆盖时的测试主机）
bool cookie_allowed_for(const std::string &url)
{
    const std::string host = luogu::cookie::host_of_url(url);
    const std::string extra_host =
        luogu::cookie::host_of_url(crawler::endpoints().official_base);
    return luogu::cookie::host_allowed(host, extra_host);
}

// 长连接句柄：Cookie 引擎绑在句柄上，C3VK 之类的 CDN 挑战 Cookie
// 因此在多次请求之间复用，避免每次都要多花一次 302 往返。
//
// 两个句柄分别用于「带凭据」与「不带凭据」：
// - 带凭据句柄按域名白名单装入用户 jar 里的凭据；
// - 不带凭据句柄从不装入任何用户凭据，保证保存站（第三方镜像）
//   请求在任何情况下都不携带洛谷凭据（即使端点被指向同一主机）。
//   注意两个句柄都启用了 Cookie 引擎（CURLOPT_COOKIEFILE ""），差别只在
//   是否装入用户 jar：镜像自己下发的 Cookie 仍会在该句柄内正常往返。
//
// 句柄按「本次请求是否允许携带凭据」选择，而不是按调用方通道选择：
// 白名单判定是逐请求做的（见 cookie_allowed_for），若把句柄与通道硬绑定，
// 「保存站端点被指向洛谷主机」时就会把凭据发给第三方镜像。
//
// 每个句柄各带一把互斥量：惰性初始化、重置、设置选项、perform、getinfo
// 整体串行。句柄上的 WRITEDATA / HEADERDATA 指向本次请求栈上的缓冲，
// 同一句柄被两条线程并发使用会让响应写进另一条线程的缓冲（错乱 / 串味）。
// 当前 auto 模式下原站线程走「带凭据」句柄、保存站线程走「不带凭据」句柄，
// 本不会撞在一起；互斥量把这条不变量变成结构性保证，限流等待放在锁外。
struct HandleSlot
{
    CURL *handle = nullptr;
    bool jar_installed = false;          // 用户 jar 是否已装进本句柄的引擎
    unsigned long long jar_version = 0;  // 已装入的 jar 版本（jar_installed 为真时有效）
    std::mutex mutex;
};

HandleSlot &handle_slot(bool with_cookies)
{
    // 函数内静态对象只初始化一次（C++11 起无初始化竞争），这里只承载句柄本身，
    // 句柄的创建与使用全部在 slot.mutex 保护下
    static HandleSlot with_cookie_slot;
    static HandleSlot without_cookie_slot;
    return with_cookies ? with_cookie_slot : without_cookie_slot;
}

// 用户 jar 的一次快照：版本号 + 域名白名单过滤后的待安装 Netscape 行。
// 在 s.mutex 保护下取好副本，安装时就不再访问共享状态（两把锁不交叉持有）。
struct CookieSnapshot
{
    unsigned long long version = 0;
    std::vector<std::string> lines;
};

CookieSnapshot cookie_snapshot()
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    CookieSnapshot snapshot;
    snapshot.version = s.jar_version;
    if (s.jar.empty())
        return snapshot;
    const std::string extra_host =
        luogu::cookie::host_of_url(crawler::endpoints().official_base);
    for (const auto &c : s.jar.cookies)
    {
        if (!luogu::cookie::host_allowed(c.domain, extra_host))
            continue; // 域名白名单硬校验：非洛谷域名一律不装
        snapshot.lines.push_back(luogu::cookie::netscape_line(c));
    }
    return snapshot;
}

// 取用句柄并保证 Cookie 引擎状态正确（调用方必须已持有 slot.mutex）。
// 只在 jar 版本变化时安装一次用户 jar：每次请求都重装会把服务器刚下发的
// 同名 Cookie（挑战 Cookie / 轮换过的会话 Cookie）覆盖回文件里的旧值。
// 调用方必须持有 slot.mutex。
// 已知限制（潜在竞态，当前调用图不可达）：jar 快照在进入句柄锁之前取得，
// 若另一条线程恰好在这中间调用 gate_clear_cookies()（清 jar + 清引擎并让
// 版本号 +1），本函数随后仍会把旧 jar 装回并记录旧版本号，而那批 Cookie
// 不会被后续的「空 jar」安装移除。因此 gate_clear_cookies() 只允许在没有
// 并发请求时调用（当前唯一调用点是 app::run 入口、抓取线程启动之前）。
CURL *acquire_handle(HandleSlot &slot, const CookieSnapshot *jar)
{
    if (!slot.handle)
    {
        slot.handle = curl_easy_init();
        if (!slot.handle)
            return nullptr;
        // 空文件名 = 只启用内存中的 Cookie 引擎，不读写磁盘
        curl_easy_setopt(slot.handle, CURLOPT_COOKIEFILE, "");
        curl_easy_setopt(slot.handle, CURLOPT_COOKIESESSION, 0L);
    }
    else
    {
        // 保留 Cookie（curl_easy_reset 只重置选项，不清空 Cookie 缓存，
        // 引擎也保持启用），因此挑战 Cookie 不会因为复用句柄而丢
        curl_easy_reset(slot.handle);
    }
    if (jar && (!slot.jar_installed || slot.jar_version != jar->version))
    {
        for (const std::string &line : jar->lines)
            curl_easy_setopt(slot.handle, CURLOPT_COOKIELIST, line.c_str());
        slot.jar_installed = true;
        slot.jar_version = jar->version;
    }
    return slot.handle;
}

// 清空两个句柄 Cookie 引擎里的全部 Cookie（"ALL" 只清 Cookie，引擎保持启用，
// 之后仍能接收与保存服务器下发的 Cookie）。
// 调用方不得持有 s.mutex：本函数要取句柄锁，请求路径的加锁顺序是
// 「先取 jar 快照（s.mutex）→ 再取句柄锁」，两把锁不交叉持有。
void clear_cookie_engines()
{
    for (int i = 0; i < 2; ++i)
    {
        HandleSlot &slot = handle_slot(i == 0);
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (!slot.handle)
            continue; // 句柄还没创建过，引擎本来就是空的
        curl_easy_setopt(slot.handle, CURLOPT_COOKIELIST, "ALL");
        slot.jar_installed = false; // 引擎已清空，下次请求按当前版本重新安装
    }
}
} // namespace

bool crawler::gate_wait(RequestClass cls, Channel ch)
{
    if (cls != RequestClass::SolutionList && cls != RequestClass::SolutionArticle)
        return true; // 图片/其它请求不受本闸门控制

    const auto range = gate_effective_delay_ms(ch);
    const long ms = with_channel(
        ch, [&](ChannelState &st) { return random_delay_ms(st, range.first, range.second); });
    if (ms <= 0)
        return true;
    return prompt::wait_delay(ms);
}

crawler::RequestResult crawler::http_get(const RequestOptions &opt, RequestClass cls,
                                         Channel ch)
{
    GateState &s = state();
    RequestResult result;

    const bool gated = (cls == RequestClass::SolutionList ||
                        cls == RequestClass::SolutionArticle);
    const bool interactive_retry = with_channel(ch, [](ChannelState &st) {
        return st.config.interactive_retry;
    });

    // 每次尝试前先过闸门；重试（限流后）同样重新等待
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        if (gated && !gate_wait(cls, ch))
        {
            result.status = RequestStatus::Stopped;
            result.error = "已按你的选择停止抓取";
            result.attempts = attempt - 1;
            return result;
        }

        // 是否携带凭据逐请求判定：既要调用方要求（只有洛谷原站请求会要求），
        // 又要目标域名在白名单内（见 cookie_allowed_for）；句柄按这个结果选择
        const bool use_cookies = opt.send_cookie && cookie_allowed_for(opt.url);
        HandleSlot &slot = handle_slot(use_cookies);
        // 先取 jar 快照与超时值（都要拿 s.mutex），再进句柄锁：两把锁不交叉持有
        const CookieSnapshot jar = use_cookies ? cookie_snapshot() : CookieSnapshot{};
        const long timeout_sec =
            opt.timeout_sec > 0
                ? opt.timeout_sec
                : with_channel(ch,
                               [](ChannelState &st) { return st.config.timeout_sec; });

        ResponseBuffer buffer;
        buffer.max_bytes = opt.max_bytes;
        HeaderSink headers;

        struct curl_slist *header_list = nullptr;
        if (opt.json_content_only)
            header_list = curl_slist_append(header_list, "x-lentille-request: content-only");
        if (!opt.if_none_match.empty())
            header_list = curl_slist_append(
                header_list, ("If-None-Match: " + opt.if_none_match).c_str());

        CURLcode code = CURLE_FAILED_INIT;
        long http_code = 0;
        {
            // 句柄互斥量覆盖「重置 + 安装 jar + 设置选项 + perform + getinfo」：
            // WRITEDATA / HEADERDATA 指向本线程栈上的缓冲，同一句柄绝不能被
            // 另一条线程并发改写；限流等待在锁外进行，不占着句柄睡觉
            std::lock_guard<std::mutex> handle_lock(slot.mutex);
            CURL *curl = acquire_handle(slot, use_cookies ? &jar : nullptr);
            if (!curl)
            {
                if (header_list)
                    curl_slist_free_all(header_list);
                result.status = RequestStatus::NetworkError;
                result.error = "初始化 libcurl 失败";
                return result;
            }

            curl_easy_setopt(curl, CURLOPT_URL, opt.url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
            // 始终校验 TLS 证书，不提供关闭开关
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "luogu-extract/0.1");
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
#if LIBCURL_VERSION_NUM >= 0x075500
            // 只允许 http/https 跳转（新接口），避免被重定向到 file:// 等协议
            curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
            curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                             CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
            if (header_list)
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

            code = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        }
        if (header_list)
            curl_slist_free_all(header_list);
        // 注意：句柄不清理，Cookie 引擎的状态要跨请求保留

        result.attempts = attempt;
        result.http_code = headers.status > 0 ? headers.status : http_code;
        result.etag = headers.etag;
        result.retry_after = headers.retry_after;
        result.body = std::move(buffer.data);

        if (buffer.overflow && code == CURLE_WRITE_ERROR)
        {
            result.status = RequestStatus::TooLarge;
            result.error = "响应内容超过体积上限";
            return result;
        }
        if (code != CURLE_OK)
        {
            result.status = RequestStatus::NetworkError;
            result.error = std::string("网络请求失败：") + curl_easy_strerror(code);

            // 连续超时/连接失败按疑似限流处理
            const int failures = with_channel(ch, [](ChannelState &st) {
                return ++st.consecutive_failures;
            });
            if (failures >= kConsecutiveFailuresAsRateLimit && gated && attempt < 3)
            {
                if (!interactive_retry)
                {
                    // auto 模式：把「疑似限流」交给调度器处理
                    result.status = RequestStatus::RateLimited;
                    result.error = "连续 " + std::to_string(failures) +
                                   " 次请求超时或连接失败（疑似限流）";
                    return result;
                }
                if (!handle_rate_limit(ch, "连续 " + std::to_string(failures) +
                                               " 次请求超时或连接失败"))
                {
                    result.status = RequestStatus::Stopped;
                    return result;
                }
                continue; // 等待后重试
            }
            return result;
        }
        with_channel(ch, [](ChannelState &st) {
            st.consecutive_failures = 0;
            return 0;
        });

        // ---- 状态码与限流判定 ----
        const long code_http = result.http_code;
        if (code_http == 304)
        {
            result.status = RequestStatus::NotModified;
            return result;
        }
        if (code_http == 401)
        {
            result.status = RequestStatus::NeedLogin;
            result.error = "服务器返回 401（需要登录态）";
            return result;
        }
        if (code_http == 200)
        {
            // 200 也可能是风控页（HTML）或未登录错误模板（JSON status=401）
            long json_status = 0;
            if (json_status_of(result.body, json_status) && json_status == 401)
            {
                result.status = RequestStatus::NeedLogin;
                result.error = "服务器返回未登录错误模板（401）";
                return result;
            }
            if (body_is_login_error(result.body))
            {
                result.status = RequestStatus::NeedLogin;
                result.error = "服务器返回未登录错误模板";
                return result;
            }
            if (looks_like_html(result.body) && body_has_rate_limit_mark(result.body))
            {
                // 风控页面：按限流处理
            }
            else if (result.body.empty())
            {
                result.status = RequestStatus::HttpError;
                result.error = "服务器返回空响应";
                return result;
            }
            else
            {
                result.status = RequestStatus::Ok;
                return result;
            }
        }

        const bool rate_limited =
            code_http == 429 || code_http == 403 ||
            (code_http == 200 && looks_like_html(result.body) &&
             body_has_rate_limit_mark(result.body)) ||
            (!result.retry_after.empty());

        if (rate_limited && gated)
        {
            std::string reason;
            if (code_http == 429)
                reason = "HTTP 429（请求过于频繁）";
            else if (code_http == 403)
                reason = "HTTP 403（疑似风控）";
            else if (!result.retry_after.empty())
                reason = "响应头 Retry-After: " + result.retry_after;
            else
                reason = "响应体包含风控提示（HTTP 200 + HTML）";

            // 等待时长取 max(Retry-After, 配置值)；
            // 但 --rate-limit-wait 0 表示「检测到限流直接停止」，
            // 是用户的显式选择，Retry-After 不能把它变成等待。
            // parse_retry_after 已把秒数限幅到 kMaxRetryAfterSec（3600）以内，
            // 因此这里的收窄转换不会溢出，也不会把等待拉长到数天
            const long retry_after = parse_retry_after(result.retry_after);
            with_channel(ch, [&](ChannelState &st) {
                if (st.config.rate_limit_wait_sec > 0 &&
                    retry_after > st.config.rate_limit_wait_sec)
                    st.config.rate_limit_wait_sec = static_cast<int>(retry_after);
                return 0;
            });

            if (!interactive_retry)
            {
                // auto 模式：立即返回，由调度器决定「临时转给另一站点」还是
                // 「放弃该站点」
                result.status = RequestStatus::RateLimited;
                result.error = reason;
                return result;
            }

            if (!handle_rate_limit(ch, reason))
            {
                bool stopped = false;
                {
                    std::lock_guard<std::mutex> lock(s.mutex);
                    stopped = s.user_stopped;
                }
                if (stopped)
                {
                    result.status = RequestStatus::Stopped;
                    result.error = "已按你的选择停止抓取";
                }
                else
                {
                    result.status = RequestStatus::RateLimited;
                    result.error = "仍被限流，已停止本次抓取";
                }
                return result;
            }
            continue; // 等待结束，重试当前请求
        }

        if (code_http == 403)
        {
            result.status = RequestStatus::Forbidden;
            result.error = "HTTP 403（服务器拒绝访问）";
            return result;
        }
        if (code_http == 404)
        {
            result.status = RequestStatus::NotFound;
            result.error = "HTTP 404（内容不存在或已删除）";
            return result;
        }
        result.status = RequestStatus::HttpError;
        result.error = "HTTP 状态码 " + std::to_string(code_http);
        return result;
    }

    // 三次尝试都用完（两次等待后仍被限流）
    result.status = RequestStatus::RateLimited;
    result.error = "仍被限流，已停止本次抓取";
    return result;
}

bool crawler::gate_user_stopped()
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.user_stopped;
}

void crawler::gate_request_stop(const std::string &reason)
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.user_stopped = true;
    if (s.rate_limit_reason.empty() && !reason.empty())
        s.rate_limit_reason = reason;
}

bool crawler::gate_rate_limited_out()
{
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.rate_limited_out;
}

std::string crawler::gate_rate_limit_reason()
{
    // 限流原因写进 GateState::rate_limit_reason（gate_note_rate_limit），
    // 不是某个通道的 limit_reason：此前读的是 Official 通道的字段，永远为空。
    // 与 gate_channel_limit_reason 一样返回拷贝，避免调用方持有锁内引用。
    GateState &s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.rate_limit_reason;
}

void crawler::gate_reset_state()
{
    GateState &s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.user_stopped = false;
        s.rate_limited_out = false;
        s.rate_limit_reason.clear();
    }
    for (int i = 0; i < kChannelCount; ++i)
    {
        const Channel ch = static_cast<Channel>(i);
        // 与 gate_reset_channel 一致：连续限流计数、放弃标志、等待截止时间、原因
        gate_reset_channel(ch);
        // 限流相关的运行期计数与放大一并清零：同一进程内多次 app::run
        // （交互模式）时从头计数、恢复基础延时
        with_channel(ch, [](ChannelState &st) {
            st.rate_limit_rounds = 0;
            st.consecutive_failures = 0;
            st.boosted = false;
            return 0;
        });
    }
}
