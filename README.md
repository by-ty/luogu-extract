# luogu-extract

一个用 C++17 编写的命令行工具，用于抓取 [洛谷](https://www.luogu.com.cn/) 的题目列表与标签，并按条件筛选题目，导出为 **Markdown** 或 **LaTeX** 文档（便于离线阅读、打印成题册）。

> **luogu-extract**（[github.com/by-ty/luogu-extract](https://github.com/by-ty/luogu-extract)）是 [luogu-export](https://github.com/sacharei/luogu-export)（MIT 协议）的派生项目，在原项目基础上进行了大量修改与增强（详见「[许可证](#许可证)」与「[鸣谢](#鸣谢)」）。

## 功能特性

- **更新缓存**（`-U, --update`）：从洛谷 CDN 下载全量题目列表（`problemset-open/latest.ndjson.gz`，gzip 解压后为 `latest.ndjson`），并从官方标签接口下载标签对照表（`tags.json`）。可与 `-M` / `-L` 一同使用：同时给出时先更新缓存再下载题目。
- **清除缓存**（以下参数只能彼此组合使用，不能与其他参数同时使用）：
  - `-C, --clean-all`：清空整个 luogu-extract 缓存文件夹（题目列表、标签、图片与字体缓存一并删除）；
  - `-CIMG, --clean-images`：清除 `luogu-extract/images/` 下的图片缓存；
  - `-CP, --clean-problems`：清除题面缓存（`latest.ndjson` 与 `latest.ndjson.gz`）；
  - `-CS, --clean-solutions`：清除题解列表缓存（`solutions.ndjson`）；
  - `-CA, --clean-articles`：清除题解正文缓存（`articles/`）。
- **按条件筛选题目**：
  - 按**标签**筛选（多个标签取「且」，即题目必须同时包含所有标签）；
  - 按**难度**筛选（支持单个数字或闭区间 `1-4`，多个取「或」）；
  - 按**题目类型**筛选（`B` 基础题 / `P` 普通题）；
  - 按**题号**筛选（`--pid`，多个取「或」；先在题目列表缓存中校验题号是否存在）；
  - 按**题号范围**筛选（`--pid-range <题号>-<题号>`（闭区间）；先校验两端点存在于缓存且属于同一题库，可与标签/难度/类型条件组合）；
  - 按**题面语言**筛选（`zh-CN` / `en`，英文缺失时自动回退中文）。
- **导出 Markdown**（`-M`）：每题一个章节，包含时空限制、题目背景、题目描述、输入/输出格式、样例、说明/提示；难度与标签的显示由下方「题目信息显示」开关控制；一级标题可用 `--set-cover-title` 自定义。
- **导出 LaTeX**（`-L`）：生成可直接用 `xelatex` 编译的完整 `.tex` 文档（含文档类、宏包、目录、页眉、标签样式等），并内置大量针对洛谷题面公式/格式「坑」的自动修复。
- **版本信息**（`-V, --version`）：输出项目简介、版本号、版权声明与项目仓库链接。
- **题目信息显示**（`-M` 与 `-L` 均生效，默认显示来源类标签、隐藏算法标签与难度）：
  - `--no-show-source-tags`：不显示来源、时间、区域、特殊题目标签（默认显示）；
  - `--show-algorithm-tags`：显示算法标签（默认不显示）；
  - `--show-difficulty-tags`：显示难度（默认不显示）。
- **LaTeX 排版定制**（均仅对 `-L` 生效）：
  - `--no-toc-links`：目录条目不带跳转到对应题目页的超链接（默认带超链接）；
  - `--toc-backlinks`：每页页眉的页码变成跳回目录页的超链接（默认无超链接）；
  - `--show-contents-difficulty-tags`：目录中的题目标题按题目难度着色；
  - `--paginate`：题目与题解（文章）之间分页——一道题目/文章结束后另起一页写下一篇（默认连续排版）；只在题目、文章之间插入换页命令，目录条目与 PDF 书签不受影响；
  - `--set-font-cover-page` / `--set-font-body-zh-CN` / `--set-font-body-en-US` / `--set-font-body-codes` / `--set-font-title-zh-CN` / `--set-font-title-en-US`：分别设置封面标题、正文中文、正文及题目大标题西文（不含公式）、代码块与正文黑体部分西文、标题中文（大标题、小节、目录与页眉）、标题西文（小节、目录与页眉；大标题西文随正文西文）的字体，参数既可填**系统已安装的字体名称**，也可填**字体文件地址**；
  - `--no-bilibili-link`：题面中的 B 站视频补全为完整网址后输出为普通文本而非超链接（默认超链接）；
  - `--set-cover-title`：自定义封面标题（`-M` 下对应一级标题）。
- **LaTeX 默认字体方案**：`-L` 生成文档时使用 ctex 宏包的 `fontset=` 机制选择整套中文字体——Windows 用 `fontset=windows`，macOS 用 `fontset=mac`；Linux 在程序运行阶段解析 `/etc/os-release`，Ubuntu / Kubuntu 等 Ubuntu 系列发行版用 `fontset=ubuntu`，其他发行版用 `fontset=fandol`。正文与题目大标题默认使用 fontset 预设的正文字体，小节标题的中文默认使用预设黑体、西文（不含公式）则与代码块使用同一字体。
- **代码字体回退链**：代码块西文，以及正文中黑体部分的西文，按 **Consolas → Menlo → DejaVu Sans Mono** 的顺序回退（`--set-font-body-codes` 可统一指定该字体）；中文字体仍由 ctex fontset 的黑体方案控制，不受影响。三种等宽字体都不存在时保留 fontspec 默认等宽字体（小标题西文则沿用正文字体）。
- **标签字体跟随正文**：题目标签使用正文中西文字体，`--set-font-body-zh-CN` / `--set-font-body-en-US` 会同步作用于标签文字。
- **跨平台兼容**：兼容 **Windows、macOS、Linux** 的主流现代版本：
  - Windows 下输出/缓存路径按 UTF-8（宽字符）处理，支持中文文件名（如 `--output 题册.tex`）与含中文用户名的缓存目录；
  - Windows 传统控制台自动启用 ANSI 转义解析，彩色与进度输出不乱码；
  - Windows（MSVC / MinGW-w64）构建自动使用内置的 `getopt` 兼容实现（语义与 GNU getopt 一致，含长选项缩写与参数重排），macOS / Linux 使用系统 `getopt`。
- **图片下载**：并行下载题面中的图片到本地缓存；导出 LaTeX 时图片引用会替换为缓存文件路径。加 `-RD, --new-download` 后不使用之前缓存的图片，而是重新下载图片。
- **下载题解**（需提供 Cookies，存在风险）：按题目抓取题解的**列表**与**正文**。
- **标签 ID 对照表**（`--tags`）：按官方分类打印标签名称与数字 ID。

## 依赖与构建

- 编译标准：C++17
- 构建工具：CMake（>= 3.23）
- 编译器：GCC / Clang / MSVC / MinGW 均可
- 外部库：
  - [libcurl](https://curl.se/)（网络请求 / 下载）
  - [libxml2](https://gitlab.gnome.org/GNOME/libxml2)（HTML 解析）
  - [nlohmann/json](https://github.com/nlohmann/json)（JSON 解析）
  - [zlib](https://zlib.net/)（gzip 解压）

```bash
cmake CMakeLists.txt
make
```

构建产物为可执行文件 `luogu-extract`。

### 示例

```bash
# 1. 首次使用先更新题目列表与标签缓存
luogu-extract -U

# 2. 导出全部题目为 Markdown（默认输出 problems.md）
luogu-extract -M

# 3. 按标签与难度筛选后导出
luogu-extract -M --tag 模拟 贪心 --difficulty 3-5

# 4. 按类型和语言筛选，导出为 LaTeX（默认输出 problems.tex）
luogu-extract -L --type P --lang zh-CN --output 题册.tex

# 5. 查看所有标签及其数字 ID
luogu-extract --tags

# 6. 定制 LaTeX 排版：目录不带超链接、页码可跳回目录、
#    封面标题改为「算法竞赛题册」并指定字体（系统字体名称或字体文件均可）
luogu-extract -L --no-toc-links --toc-backlinks \
    --set-cover-title "算法竞赛题册" \
    --set-font-cover-page "Noto Serif CJK SC" \
    --set-font-body-zh-CN "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc" \
    --set-font-body-en-US "TeX Gyre Pagella" \
    --set-font-body-codes "JetBrains Mono" \
    --set-font-title-zh-CN "Noto Sans CJK SC" \
    --set-font-title-en-US "TeX Gyre Heros" \
    --no-bilibili-link

# 7. Markdown 导出时自定义一级标题
luogu-extract -M --set-cover-title "洛谷竞赛题册（全量）"

# 8. 按题号导出指定题目（可重复 --pid 或空格分隔；题号必须存在于缓存）
luogu-extract -L --pid P1001 P1002 --pid P2000 --output 指定题目.tex

# 9. 按题号范围导出（闭区间；可与标签/难度/类型组合）
luogu-extract -L --pid-range P1000-P1999 --tag "动态规划 DP" --difficulty 3-5 \
    --output 区间题册.tex

# 10. 显示题目难度、算法标签，并让目录中的题目标题按难度着色
luogu-extract -L --show-difficulty-tags --show-algorithm-tags \
    --show-contents-difficulty-tags --output 题册.tex

# 11. 只保留算法标签（隐藏来源、时间、区域、特殊题目标签）
luogu-extract -L --no-show-source-tags --show-algorithm-tags --output 题册.tex

# 12. Markdown 导出同样支持这三个显示开关（与 -L 语义一致）
luogu-extract -M --show-difficulty-tags --show-algorithm-tags --output 题册.md
luogu-extract -M --no-show-source-tags --output 仅题面.md

# 13. 先更新缓存再导出（-U 与下载题目的参数一起使用时，先更新再下载，
#     与参数位置无关；下面两条命令效果相同）
luogu-extract -U -L --tag 模拟 --output 题册.tex
luogu-extract -L --tag 模拟 --output 题册.tex -U

# 14. 重新下载题面图片（不使用之前缓存的图片；下载失败时保留原有缓存）
luogu-extract -L -RD --pid P1001 P1002 --output 指定题目.tex

# 15. 清除缓存：清除类参数只可彼此组合使用（-C 覆盖全部，其余可叠加）
luogu-extract -C                    # 清空整个 luogu-extract 缓存文件夹
luogu-extract -CIMG                 # 只清除 images/ 下的图片缓存
luogu-extract -CP                   # 只清除题面缓存（latest.ndjson 与 latest.ndjson.gz）
luogu-extract -CS                   # 只清除题解列表缓存（solutions.ndjson）
luogu-extract -CA                   # 只清除题解正文缓存（articles/）
luogu-extract -CIMG -CP -CS -CA      # 一次清除图片、题面、题解列表与题解正文缓存

# 16. 下载题解：最简写法（12 道题，每题 1 篇，题解统一放在文档最后；
#     双向按钮与目录条目默认开启。来源默认 auto：缓存优先，未命中的在
#     原站与保存站之间轮流分配、并行抓取）
luogu-extract -L --cookie cookies.txt --tag 动态规划 --with-solutions --output 题册.tex

# 17. 题解紧跟题目之后
luogu-extract -L --cookie cookies.txt --pid-range P1000-P1099 \
    --with-solutions --article-placement per-problem --output 题册.tex

# 18. 抓取某题全部题解（会触发第 5 档警告，需 3 次确认）
luogu-extract -L --cookie cookies.txt --pid P1001 \
    --with-solutions --max-articles all --output 单题题册.tex

# 19. 离线囤货：只抓缓存不导出；延时放宽到 8~15 秒
luogu-extract --cookie cookies.txt --pid-range P1000-P1099 \
    --articles-only-download --request-delay 8-15

# 20. 强制刷新题解正文后重新导出，但复用已缓存的题解列表
luogu-extract -L --cookie cookies.txt --tag 贪心 --with-solutions \
    --refresh-articles --output 题册.tex

# 21. 关闭跳转按钮与目录中的题解条目
luogu-extract -L --cookie cookies.txt --tag 贪心 --with-solutions \
    --no-problem-to-article-link --no-article-to-problem-link \
    --no-article-toc --output 题册.tex

# 22. 只用保存站（第三方镜像）获取题解正文；该来源不发送任何 Cookie
luogu-extract -L --cookie cookies.txt --tag 贪心 --with-solutions \
    --article-source save --output 题册.tex

# 23. 分页排版：一道题目/文章结束后另起一页写下一篇
#     （题解同样分页，目录与 PDF 书签不受影响）
luogu-extract -L --tag 动态规划 --paginate --output 分页题册.tex

# 24. 题解分页且紧跟各自题目
luogu-extract -L --cookie cookies.txt --pid-range P1000-P1099 \
    --with-solutions --article-placement per-problem \
    --paginate --output 分页题册.tex
```

### 参数说明

| 选项 | 含义 |
| --- | --- |
| `-U, --update` | 更新题目列表缓存（`latest.ndjson`）与标签缓存（`tags.json`）。可与 `-M` / `-L` 一同使用：同时给出时先更新缓存再下载题目；更新失败时不继续执行后续操作 |
| `-M, --markdown` | 筛选并导出 Markdown（默认输出 `problems.md`） |
| `-L, --latex` | 筛选并导出 LaTeX（默认输出 `problems.tex`） |
| `-RD, --new-download` | 仅 `-L` 有效：下载题目时不使用之前缓存的图片，而是重新下载图片 |
| `-C, --clean-all` | 清空 luogu-extract 缓存文件夹（含题目列表、标签、图片与字体缓存）。清除类参数（`-C` / `-CIMG` / `-CP` / `-CS` / `-CA`）只能彼此组合使用，不能与其他参数同时使用 |
| `-CIMG, --clean-images` | 清除 `<缓存目录>/images/` 下的图片缓存。清除类参数只能彼此组合使用 |
| `-CP, --clean-problems` | 清除题面缓存（`latest.ndjson` 与 `latest.ndjson.gz`）。清除类参数只能彼此组合使用 |
| `-CS, --clean-solutions` | 清除题解列表缓存（`<缓存目录>/solutions.ndjson`）。清除类参数只能彼此组合使用 |
| `-CA, --clean-articles` | 清除文章缓存（`<缓存目录>/articles/`）。清除类参数只能彼此组合使用 |
| `--tags` | 按官方分类打印标签 ID 对照表（可与 `-h` 组合） |
| `--tag <name\|ID>...` | 按标签筛选；多个值可用空格分隔或重复 `--tag`，题目须包含全部标签；引号整体恰好等于已知标签名（如 `"NOIP 普及组"`）时按一个标签处理 |
| `--difficulty <spec>` | 按难度（ $0\sim 8$ ）筛选；支持区间写法（如 `1-4`），多组值可用空格分隔或重复 `--difficulty` |
| `--type <B\|P>` | 按题目类型筛选（可重复，空表示全部类型） |
| `--pid <pid>...` | 按题号精确筛选；多个值可用空格分隔或重复 `--pid`。不能与 `--tag`、`--difficulty`、`--type` 同时使用 |
| `--pid-range <a>-<b>` | 按题号闭区间筛选；多组值可用空格分隔或重复 `--pid-range`。一组范围两端必须为同一题库（如都为 `P` 题库或都为 `B` 题库，多组范围间可不为同一题库）。可与 `--tag`、`--difficulty`、`--type` 同时使用 |
| `--lang <zh-CN\|en>` | 题面语言（默认 `zh-CN`；`en` 缺失时回退中文） |
| `--output <file>` | 输出文件路径（默认 `problems.md` / `problems.tex`） |
| `--no-show-source-tags` | `-M` / `-L` 均有效：不显示来源、时间、区域、特殊题目标签（默认显示） |
| `--show-algorithm-tags` | `-M` / `-L` 均有效：显示算法标签（默认不显示） |
| `--show-difficulty-tags` | `-M` / `-L` 均有效：显示难度（默认不显示） |
| `--no-toc-links` | 仅 `-L` 有效：目录条目不带跳转到对应题目页的超链接（默认带超链接） |
| `--toc-backlinks` | 仅 `-L` 有效：每页页眉处的页码为跳回目录页的超链接（默认无超链接） |
| `--show-contents-difficulty-tags` | 仅 `-L` 有效：目录中的题目标题按难度着色 |
| `--paginate` | 仅 `-L` 有效：题目与题解（文章）之间分页，每道题、每篇文章都从新的一页开始（默认连续排版） |
| `--set-font-cover-page <font>` | 仅 `-L` 有效：设置封面标题字体；`<font>` 为系统已安装的字体名称或字体文件地址 |
| `--set-font-body-zh-CN <font>` | 仅 `-L` 有效：设置题面正文中文字符的字体（名称或字体文件地址） |
| `--set-font-body-en-US <font>` | 仅 `-L` 有效：设置题面正文及题目大标题中的西文字符字体（名称或字体文件地址；不作用于公式） |
| `--set-font-body-codes <font>` | 仅 `-L` 有效：设置代码块西文，以及正文黑体部分西文的字体（名称或字体文件地址；默认按 `Consolas` → `Menlo` → `DejaVu Sans Mono` 回退） |
| `--set-font-title-zh-CN <font>` | 仅 `-L` 有效：设置题目大标题、小节标题、目录页标题与每页页眉标题中的中文字体（名称或字体文件地址） |
| `--set-font-title-en-US <font>` | 仅 `-L` 有效：设置小节标题、目录页标题与每页页眉标题中的西文字体；题目大标题西文跟随 `--set-font-body-en-US`（名称或字体文件地址） |
| `--no-bilibili-link` | 仅 `-L` 有效：题面中的 B 站视频补全为完整网址后输出为普通文本而非超链接（默认超链接） |
| `--set-cover-title <title>` | 设置封面标题（`-L`，默认 `luogu extract`）或 Markdown 一级标题（`-M`，默认 `洛谷题目导出`） |
| `--with-solutions` | 启用题解抓取与导出；需与 `-M` 或 `-L` 同用，题解与题面导出到同一个文件 |
| `--cookie <file>` | 需提供 Netscape 格式的 `cookies.txt`（含登录态）。题解列表接口需要登录态，该参数是启用题解功能的前提 |
| `--cookie-string <k=v; ...>` | 直接传入 Cookie 串（与 `--cookie` 二选一） |
| `--article-source <auto\|official\|save>` | 题解**正文**来源，默认 `auto`：缓存优先（两个来源都有时优先原站），未命中的在[洛谷原站](https://www.luogu.com.cn/)与[洛谷保存站](https://www.luogu.me/)之间**轮流分配、并行抓取**；`official` 只用原站；`save` 只用保存站。题解列表恒取洛谷原站 |
| `--max-articles <n\|all>` | 每题抓取篇数，默认 `1`，按列表顺序取最靠前的 `n` 篇；`all` 表示该题**全部**题解 |
| `--request-delay <mean\|min-max>` | 请求的平均间隔秒数，默认 `5`（实际为均值 ±30% 均匀抖动，即 3.5~6.5 秒）；也支持显式区间（如 `8-15`）与小数（如 `2.5`）；单次间隔上限 300 秒 |
| `--no-delay-auto-scale` | 关闭「随抓取量自动递增延时」与限流后的额外放大 |
| `--solution-ttl <days>` | 题解**列表**缓存有效期天数：默认**无限**；`0` 表示每次都发 ETag 条件请求（`304` 时只刷新时间戳） |
| `--article-ttl <days>` | 题解**正文**缓存有效期天数：语义同上，默认**无限**；`0` 表示每次都发 ETag 条件请求（`304` 时只刷新时间戳） |
| `--rate-limit-wait <seconds>` | 检测到限流后的等待时长，默认 `120`；`0` 表示检测到限流直接停止。等待期间可按 `S` 立即停止、按 `C` 确认后立即继续 |
| `--allow-partial` | 允许导出正文不完整的题解（默认跳过并在结束时汇总） |
| `--refresh-solutions` | 强制重新获取题解列表（忽略有效期与 ETag） |
| `--refresh-articles` | 强制重新获取题解正文（忽略有效期与 ETag） |
| `--articles-only` | 只导出题解，不导出题面（需与 `-M` 或 `-L` 同用） |
| `--articles-only-download` | 只抓取并缓存题解，不导出任何文件（不需要 `-M` / `-L`） |
| `--article-placement <document-end\|per-problem>` | 题解在文档中的位置，默认 `document-end`（统一置于文档最后）；`per-problem` 表示紧跟对应题目之后 |
| `--no-problem-to-article-link` | 关闭题目到题解的跳转（`-L` 为题目标题右侧的「查看题解」按钮，`-M` 为 Markdown 中的跳转链接） |
| `--no-article-to-problem-link` | 关闭题解到题目的跳转（`-L` 为题解标题右侧的「返回题目」按钮，`-M` 为 Markdown 中的跳转链接） |
| `--no-article-toc` | 仅 `-L` 有效：题解标题不进目录（默认进目录并注明所属题目） |
| `--no-article-meta` | 不显示题解的来源与原文链接 |
| `-y, --yes` | 把爬取风险的确认次数减少 1 次（减到 0 为止）；不能把第 5 档变为无需确认 |
| `-h, --help` | 显示帮助（文本与上表内容一致） |
| `-V, --version` | 显示项目简介、版本号、版权声明与项目仓库链接；版本号在编译期由 CMake 从 `CMakeLists.txt` 中的 `project(luogu-extract VERSION ...)` 取得；不能与其他参数同时使用 |

> `-M` 与 `-L` 不能同时使用；需要两种格式时请分两次执行。

关于字体参数 `<font>` 的写法：

- **系统已安装的字体名称**：直接填字体名，如 `"Noto Sans CJK SC"`、`"SimSun"`；
- **字体文件地址**：填字体文件的路径（支持相对路径与绝对路径），如 `fonts/source-han-serif.ttc`、`/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc`；程序会校验该文件是否存在，存在时在生成的 `.tex` 中引用其绝对路径；
- 含路径分隔符或以 `.ttf`/`.otf`/`.ttc` 等常见字体扩展名结尾的值一律按**字体文件地址**处理，文件不存在时会报错并拒绝执行；其余值按**字体名称**处理；
- `--set-font-*` 系列参数的优先级最高：只要传入对应参数，生成的 `.tex` 就会用参数指定的字体覆盖 ctex fontset 中的默认值；未传入的参数一律使用上述 ctex fontset / 代码字体回退链的默认方案；
- 不传 `--set-font-body-codes` 时，代码块西文与正文黑体部分的西文按 `Consolas` → `Menlo` → `DejaVu Sans Mono` 回退；三种字体都不可用时保留 fontspec 默认等宽字体；
- 不传 `--set-font-body-zh-CN` / `--set-font-title-zh-CN` 等参数时，正文与标题直接使用 ctex fontset 预设的中西文字体，不再额外指定 `SimHei` 或等宽标题字体。

## 题解下载（`--with-solutions`）

### 声明

- 题解**著作权归原作者**，导出物**仅供个人离线阅读**，请勿再分发或用于商业用途；
- 抓取频率与请求总量由用户自行判断，若因抓取过快或过多导致被限流或封禁，**后果自负**；
- 洛谷保存站（[luogu.me](https://www.luogu.me/)）为**第三方站点**；
- 请遵守洛谷用户协议及相关法律法规。

程序**不实现**账号密码/验证码登录，**不绕过**任何访问控制，也**不实现**任何规避风控的手段（不使用代理池、不伪造指纹、不绕验证码）。检测到限流时只会**等待**或**停止**。

### 准备：导出 cookies.txt

获取题解**列表**接口需要登录态（未带登录 Cookie 时返回 401），因此必须先准备 Netscape 格式的 `cookies.txt`：

1. 登录洛谷；
2. 用浏览器扩展（如 `Get cookies.txt LOCALLY`）导出**Netscape 格式**的 cookies，或按该格式手写：

   ```
   # Netscape HTTP Cookie File
   #HttpOnly_.luogu.com.cn	TRUE	/	TRUE	<过期时间戳>	_uid	<值>
   #HttpOnly_.luogu.com.cn	TRUE	/	TRUE	<过期时间戳>	__client_id	<值>
   ```

3. 用 `--cookie cookies.txt` 指定。建议把文件权限设为 `600`（程序检测到同组/其他用户可读时会提示，但不会修改你的文件）。

安全约定：Cookie **只**会附加到 `luogu.com.cn` / `luogu.org` 及其子域；`auto` 模式下的保存站请求与 `--article-source save` **不带 Cookie**；Cookie 不会打印、不会写入日志与缓存；重定向到其它域名时由 libcurl 的 Cookie 引擎按域匹配，不会外泄。

### 正文来源：`auto`（默认）

`--article-source` 默认为 `auto`，规则如下：

| 情况 | 行为 |
| --- | --- |
| 缓存里已有该篇题解 | **直接用缓存**，不发请求；两个来源都有时**优先原站** |
| 缓存里没有 | 在**原站**与**保存站**之间**轮流分配**，两条通道**并行抓取** |
| 某个站点被限流（或封禁） | 当前这一篇**临时转给另一个站点**；该站点按自己的限流等待计时，到期后继续重试本站点剩余任务 |
| 某站点没有这篇题解或该请求失败 | **换另一个站点再试一次**（只改派一次）：保存站是第三方镜像，可能没有收录某篇题解；原站的文章也可能已被删除而镜像仍有副本 |
| 某站点**连续被限流 3 次** | **放弃该站点**，剩余任务全部转给另一个站点（某次成功即清零连续计数） |
| **两个站点都连续被限流 3 次** | **终止下载**（退出码 3，已抓缓存保留） |
| 计划阶段获取题解列表 | 列表恒取原站，闸门内部仍会交互式等待/重试（列表没有备用站点） |

两个站点**分别计算延时**（`--request-delay` 与自动递增系数各算一份）、分别统计限流状态；保存站请求始终**不携带 Cookie**。需要固定单一来源时用 `--article-source official` 或 `save`。

### 爬取风险分级提示

程序在计划阶段（只读缓存、统计本次实际待抓篇数）后按规模给出 5 档提示，**以「实际待抓的正文篇数 N + 需要重新获取的列表数 P」定档**：

| 档 | 计划请求数（N + P） | 提示等级 | 基础确认次数 | 加 `--yes` 后 |
| --- | --- | --- | --- | --- |
| 1 | ≤ 3 | 简单提示 | 0（提示后直接继续） | 0 |
| 2 | 4 ~ 5 | 警告 + 确认 | 1 | 0 |
| 3 | 6 ~ 10 | 警告 + 确认 | 2 | 1 |
| 4 | 11 ~ 20 | 严厉警告 + 确认 | 2 | 1 |
| 5 | ≥ 21 | 醒目警告 + 明确不推荐 + 确认 | 3 | 2 |

- 计数在**计划阶段**完成：先只读缓存，再把缓存缺失/过期的题解列表抓回来（列表请求本来就属于 N + P 里的 P，同样受 `--request-delay` 控制），因此确认前显示的篇数是**精确值**；
- `-y, --yes` **只把确认次数减少 1 次**（减到 0 为止），不能把第 5 档变成无需确认；
- 第 5 档的**最后一次**确认要求原样输入 `I know what I am doing`；
- 确认一律 **`[y/N]`**：必须显式输入 `y`（直接回车视为拒绝），任一次拒绝立即退出（**退出码 0**）；
- **失败闭合**：标准输入不是交互终端（如管道、重定向）且仍需确认时**拒绝执行**，绝不默认继续；

### 延时、自动递增与耗时估算

- `--request-delay 5` ⇒ 每次等待 `[3.5, 6.5]` 秒的随机值（均值 5 秒）；`--request-delay 8-15` 为显式闭区间；支持小数；单次上限 300 秒；
- 题解请求**串行**，延时覆盖**题解列表接口**与**题解正文**；
- 延时按本次网络请求数 Np **一次性算定系数**（≤20→1.0；≤99→1.5；≤299→2.0；>299→3.0），单侧封顶 60 秒；`--no-delay-auto-scale` 关闭该机制；
- 运行中出现限流后，本次运行剩余请求的系数**额外 ×1.5**；
- 计划阶段会打印「预计网络请求次数 / 平均间隔 / 预计耗时」，便于当场决定是否缩小范围。

### 限流检测与暂停重试

以下信号会被判定为限流：HTTP `429`、HTTP `403`（疑似风控）、响应头含 `Retry-After`、响应体含风控特征串（「操作过于频繁」「请稍后再试」「验证码」「访问受限」等）、连续 5 次请求超时或连接失败。

命中的处理流程：

```
检测到限流 → 打印已暂停与原因 → 可中断等待（默认 120 秒，--rate-limit-wait 可改）
          → 等待结束重试当前请求 → 仍被拒绝则直接停止（不再等待、不再重试）
```

- 等待期间：按 **`S`** 立即停止（等同主动取消，缓存保留，**退出码 0**）；按 **`C`** 立即继续（需二次确认「提前继续可能加重风控……后果自负」）；无 TTY 时退化为纯倒计时并打印剩余时间；
- 一次运行内**最多触发 2 轮**等待，第 3 次仍被限流即停止（**退出码 3**，与主动取消的 `0` 区分，便于脚本判断）；
- `--rate-limit-wait 0` 表示检测到限流直接停止。

### 缓存、增量与断点续传

题解缓存由两部分组成：

| 文件 | 说明 |
| --- | --- |
| `solutions.ndjson` | 题解列表：一行一个题目，记录该题的题解列表、分页信息与抓取元信息（`pid` / `items` / `etag` / `fetched_at` 等） |
| `articles/<文章编号>.<来源>.json` | 单篇题解正文（洛谷 Markdown 原文；按来源分别入键，正文不再按题目分目录） |

- 默认**优先读缓存**：列表的有效期由 `--solution-ttl`、正文的有效期由 `--article-ttl` 控制，两者默认都是**无限**——只要缓存里有就一律直接用，命中即**零请求**；只有显式指定天数后过期内容才会重取；
- 指定了有效期且已过期时带 `If-None-Match` 发条件请求，命中 `304` 时只刷新时间戳；`--solution-ttl 0` / `--article-ttl 0` 表示每次都发条件请求；
- `--refresh-solutions` / `--refresh-articles` 分别强制重抓列表 / 正文；
- 上次中断后重跑会跳过已缓存篇目（**断点续传**）；部分失败只缓存成功篇目，下次自动补抓；
- 切换 `--article-source` 后另一来源视为未命中，两个来源分别缓存；
- 缓存一律「同目录临时文件 + `fsync` + 原子替换」写入：写入失败或进程被杀时原缓存保持不变，最多残留 `.tmp.*`（启动时清理超过 1 小时的残留）；
- 结束时打印题解缓存占用；`-CS, --clean-solutions` 只清除题解列表缓存，`-CA, --clean-articles` 只清除题解正文缓存。

### 输出形态

**位置模式**（`--article-placement`，默认 `document-end`）：

| 模式 | LaTeX 结构 | Markdown 结构 |
| --- | --- | --- |
| `document-end`（默认） | 题面各题依次 `\section`；另起一页后每题一组 `\section*{<PID> <题目名>}` → 该题各篇 `\subsection*{题解：…}`（同题题解连续排列，**不再单独生成与目录同级的一级标题**） | 文末 `# 题解` → `## <PID> <题目名>` → `### 题解：…` |
| `per-problem` | 每题 `\section` → 题面 → 紧随该题的各篇 `\subsection*{题解：…}` | `# <PID> <题目名>` → 题面 → `### 题解：…` |

- 两种模式下题解与题面都在**同一个文件**，不产生额外文件；
- 题解标题统一格式 `题解：<题解标题>`，过长时截断为 60 字符加 `…`，避免目录与书签被超长标题撑爆。

### 错误与汇总

- **需要登录态但无 Cookie（401 / 跳登录）**：中止并提示「需要登录态；请登录洛谷后导出 cookies.txt，并用 `--cookie <file>` 指定」；
- **Cookie 过期**：中止并提示重新导出；
- **429 / 403 / 风控页**：走上文的暂停流程；
- **单篇 404 / 无权限 / 付费**：跳过并汇总「N 篇不可访问（已删除或无权限）」；
- **正文不完整（`contentFull=false`）**：默认跳过并汇总「N 篇正文不完整（可用 `--allow-partial` 导出）」，加 `--allow-partial` 后导出并在文档中标注；
- **列表接口结构异常**：中止并提示排查；
- **该题确无题解**：正常继续，打印「<PID> 暂无题解」，不生成按钮与小节；
- **保存站不可用**：中止并提示「保存站请求失败；可用 `--article-source official` 改用原站」；
- **用户拒绝确认 / 主动停止**：打印「已按你的选择取消」，**退出码 0**；
- **因限流中止**：汇总「成功 X 篇，因限流中止」，**退出码 3**；
- **磁盘写入失败**：中止并提示，原缓存保持不变。

## 缓存机制

缓存目录按以下顺序确定：

1. 环境变量 `XDG_CACHE_HOME` 存在时 → `$XDG_CACHE_HOME/luogu-extract`；
2. 否则使用 `$HOME/.cache/luogu-extract`；
3. Windows 下若前两项均未设置，使用 `%LOCALAPPDATA%\luogu-extract`；
4. 否则使用系统临时目录下的 `luogu-extract`。

缓存目录中的文件：

| 文件 | 说明 |
| --- | --- |
| `latest.ndjson` | 全量题目列表（每行一个题目的 JSON），由 `-U` 下载并解压得到 |
| `tags.json` | 标签对照表：`{"<数字ID>": {"name": "<名称>", "type": <分类>}}` |
| `images/` | 图片缓存目录：文件名 = 完整 URL 的双种子 FNV-1a 128 位哈希（32 位十六进制）+ 白名单扩展名 |
| `fonts/` | 字体缓存目录：无扩展名的字体文件按格式识别后复制到此并补全扩展名 |
| `solutions.ndjson` | 题解列表缓存：一行一个题目（`{"pid": ..., "items": [...], "etag": ..., "fetched_at": ...}`），写入时替换该题目那一行 |
| `articles/` | 题解正文缓存目录：`<文章编号>.<来源>.json` |

图片文件名仅包含哈希值与扩展名：对完整 URL 分别以官方偏移基数（`0xcbf29ce484222325`）与官方素数（`0x100000001b3`）为种子计算两路 64 位 FNV-1a，拼成 128 位后输出 32 位十六进制作为文件名主体（不含 URL 原文，避免不同图床的同名图片互相覆盖）；扩展名取自 URL 路径并做白名单清洗，非法/超长扩展名丢弃。已存在的文件会跳过。下载时按 CPU 核心数并行，洛谷图床（`luogu.com.cn`）的图片会串行下载并保持 0.5~3 秒随机间隔，避免请求过快。加 `-RD, --new-download` 时已有图片不再跳过：新图片先下载到同目录的临时文件，校验通过后再原子替换缓存中的同名图片，下载失败或内容无效时只删除临时文件，原有缓存保持不变。

缓存清除（以下参数只能彼此组合使用）：

| 参数 | 清除范围 |
| --- | --- |
| `-C, --clean-all` | 缓存目录 `luogu-extract` 本身（相当于删除整个缓存文件夹） |
| `-CIMG, --clean-images` | `luogu-extract/images/` 目录及其中的全部图片 |
| `-CP, --clean-problems` | `luogu-extract/latest.ndjson` 与 `luogu-extract/latest.ndjson.gz`（以及更新中断时可能残留的 `latest.ndjson.tmp.*` 临时文件） |
| `-CS, --clean-solutions` | `luogu-extract/solutions.ndjson`（全部题解列表缓存） |
| `-CA, --clean-articles` | `luogu-extract/articles/` 目录及其中的全部题解正文缓存 |

缓存目录或文件不存在时按「已清空」处理并正常退出；删除失败（如权限不足）时输出错误信息并以非零状态码退出。

## 导出格式说明

### Markdown（`-M`）

| 项目 | 说明 |
| --- | --- |
| 文件头 | 题目总数与筛选条件（含「不显示难度 / 不显示算法类标签 / 不显示来源类标签」等显示设置说明） |
| 题目分节 | 每道题以 `---` 分隔，以 `# <题号> <标题>` 作为章节标题 |
| 单题内容 | 标签、时空限制，以及题目背景、题目描述、输入格式、输出格式、输入输出样例、说明/提示 |
| 难度 | 默认不显示；加 `--show-difficulty-tags` 后在标题下方显示「难度：<难度>」 |
| 标签 | 默认显示算法标签以外的标签（来源、时间、区域、特殊等，保持缓存中的原顺序）；加 `--show-algorithm-tags` 后连同算法标签一起显示；加 `--no-show-source-tags` 后不显示算法标签以外的标签；两类标签都不显示时没有「标签」一栏 |
| 一级标题 | 可用 `--set-cover-title` 自定义（默认 `洛谷题目导出`） |

### LaTeX（`-L`）

| 项目 | 说明 |
| --- | --- |
| 文档结构 | 完整可编译的 `.tex` 文档：封面、目录、页眉页码，章节不编号 |
| 封面 | 标题可用 `--set-cover-title` 自定义（默认 `luogu extract`）；作者「luogu-extract」带有指向项目仓库的超链接 |
| 数学公式 | 由 `unicode-math` + `Latin Modern Math` 统一排版 |
| 表格 | 自动转换洛谷的合并语法（单元格恰为 `^` 时向上合并、恰为 `<` 时向左合并）；普通表格的表头自动加粗，表头中的公式同样加粗；支持 Tuack 样式表格 |
| 折叠框 | `:::info` / `:::success` / `:::warning` / `:::error` 渲染为彩色盒子：标题条底色与框线用对应折叠框颜色；未指定标题时用默认标题；支持嵌套和跨页 |
| 未闭合块 | 一段内容结束时若仍有未闭合的块（代码围栏、折叠框、引言、居中/居右标记），按嵌套次序从内到外自动闭合，内容照常导出、不会丢失 |
| 图片 | 只引用缓存中已有的图片，缺失的图片会被跳过而不影响编译；过大的图片自动缩小到版心内，小图片保持原始大小；xelatex 无法加载的格式（GIF/WebP/SVG/BMP/ICO 等）会被跳过；加 `-RD, --new-download` 后导出前重新下载题面引用的图片 |
| 视频 | B 站视频补全为 `https://www.bilibili.com/video/...` 的完整网址后输出链接；加 `--no-bilibili-link` 后输出为普通文本 |
| 难度 | 默认不显示；加 `--show-difficulty-tags` 后在时间限制、内存限制下方显示「难度：<难度>」，`<难度>` 的字体颜色与洛谷网页一致 |
| 标签 | 默认显示来源、时间、区域、特殊题目标签；加 `--show-algorithm-tags` 后算法标签显示在最前；加 `--no-show-source-tags` 后不显示来源、时间、区域、特殊题目标签；两类标签都不显示时没有「标签」一栏 |
| 目录 | 目录条目默认可跳转到对应题目；加 `--no-toc-links` 后不带超链接；加 `--show-contents-difficulty-tags` 后目录中的题目标题按难度着色 |
| 页眉页码 | 加 `--toc-backlinks` 后，每页页眉的页码可跳回目录页 |
| 默认字体 | 按系统自动选择中文字体方案（Windows / macOS / Linux 各有对应方案） |
| 自定义字体 | `--set-font-*` 可分别设置封面标题、正文中文、正文西文、代码、标题中文、标题西文的字体，填系统已安装的字体名称或字体文件地址均可 |
| 标签字体 | 跟随正文中西文字体，随 `--set-font-body-zh-CN` / `--set-font-body-en-US` 一起变化 |

> [!IMPORTANT]
> LaTex 导出后请使用 `latexmk --xelatex <输出文件名>.tex` 编译。

## 参数错误处理

程序会在执行前校验参数，出现以下填用错误时拒绝执行并输出中文错误信息（含出错参数与正确调用方式）：

- 字体类参数（`--set-font-*`）后未接字体名称或字体文件地址；
- 字体类参数被识别为字体文件地址，但对应文件不存在；
- 选择了 `-M`（Markdown）导出，却使用了仅 `-L`（LaTeX）支持的设置参数；
- 清除缓存的参数（`-C` / `-CIMG` / `-CP` / `-CS` / `-CA`）只能彼此组合使用，与非清除类参数（含 `-h`、`--tags`、`-U`、`-M`、`-L` 等）或多余的位置参数同时出现即拒绝执行；
- `-V` / `--version` 与其他参数（含 `-h`、`--tags`、`-M`、`-L` 等）或多余的位置参数同时使用（该参数必须单独使用）；
- 出现了程序没有的未知参数（提示使用 `-h, --help` 查看帮助）；
- 题解相关参数（`--cookie`、`--cookie-string`、`--article-source`、`--solution-*`、`--max-articles`、`--refresh-*`、`--no-article-*`、`--yes` 等）在未启用 `--with-solutions`（或 `--articles-only` / `--articles-only-download`）时，报「缺少 `--with-solutions`」；
- `--with-solutions` / `--articles-only` 未与 `-M` 或 `-L` 同用（`--articles-only-download` 例外，它不需要导出模式）；
- 启用题解功能但未提供 `--cookie` / `--cookie-string`（题解列表接口需要登录态），或两者同时给出；
- `--request-delay` 非正数、区间不满足 `0 < min ≤ max`、或单次间隔超过 300 秒；
- `--solution-ttl` / `--article-ttl` 小于 0、`--rate-limit-wait < 0`、`--max-articles` 既不是正整数也不是 `all`；
- `--article-placement` 不是 `document-end` / `per-problem`，或 `--article-source` 不是 `official` / `save`；
- `--articles-only` 与 `--articles-only-download` 同时给出；
- `-CS` / `--clean-solutions`、`-CA` / `--clean-articles` 与其他参数同时使用；
- 非交互终端（管道、重定向）下仍需风险确认时**拒绝执行**并说明原因。

## 待添加功能
- [ ] 简易命令行交互程序，通过交互设置下载参数；
- [ ] 按题单下载题目功能。

## 许可证

本项目（**luogu-extract**）以 **GNU Lesser General Public License v3.0 或任何更新版本**（SPDX: `LGPL-3.0-or-later`）授权发布，版权 © 2026 by-ty，完整许可文本见 [LICENSE](LICENSE)。

本项目派生自以 MIT 协议发布的 [luogu-export](https://github.com/sacharei/luogu-export)（Copyright © 2026 sacharei）。按 MIT 协议要求，原版权与许可声明完整保留于 LICENSE 文件「Original MIT License」一节。

## 鸣谢

- 本项目派生自 [sacharei/luogu-export](https://github.com/sacharei/luogu-export)（MIT 协议，Copyright © 2026 sacharei），感谢原作者的贡献；
- 本项目部分代码由 [DeepSeek](https://www.deepseek.com/) 辅助生成。
