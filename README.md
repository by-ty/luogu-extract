# luogu-extract

一个用 C++17 编写的命令行工具，用于抓取 [洛谷](https://www.luogu.com.cn/) 的题目列表与标签，并按条件筛选题目，导出为 **Markdown** 或 **LaTeX** 文档（便于离线阅读、打印成题册）。

> **luogu-extract**（[github.com/by-ty/luogu-extract](https://github.com/by-ty/luogu-extract)）是 [luogu-export](https://github.com/sacharei/luogu-export)（MIT 协议）的派生项目，在原项目基础上进行了大量修改与增强（详见「[许可证](#许可证)」与「[鸣谢](#鸣谢)」）。

## 功能特性

- **更新缓存**：从洛谷 CDN 下载全量题目列表（`problemset-open/latest.ndjson.gz`，gzip 解压后为 `latest.ndjson`），并从官方标签接口下载标签对照表（`tags.json`）。
- **按条件筛选题目**：
  - 按**标签**筛选（多个标签取「且」，即题目必须同时包含所有标签）；
  - 按**难度**筛选（支持单个数字或闭区间 `1-4`，多个取「或」）；
  - 按**题目类型**筛选（`B` 基础题 / `P` 普通题）；
  - 按**题号**筛选（`--pid`，多个取「或」；先在题目列表缓存中校验题号是否存在）；
  - 按**题号范围**筛选（`--pid-range <题号>-<题号>`（闭区间）；先校验两端点存在于缓存且属于同一题库，可与标签/难度/类型条件组合）；
  - 按**题面语言**筛选（`zh-CN` / `en`，英文缺失时自动回退中文）。
- **导出 Markdown**（`-M`）：每题一个章节，包含难度、标签、时空限制、题目背景、题目描述、输入/输出格式、样例、说明/提示；一级标题可用 `--set-cover-title` 自定义。
- **导出 LaTeX**（`-L`）：生成可直接用 `xelatex` 编译的完整 `.tex` 文档（含文档类、宏包、目录、页眉、标签样式等），并内置大量针对洛谷题面公式/格式「坑」的自动修复。
- **版本信息**（`-V, --version`）：输出项目简介、版本号、版权声明与项目仓库链接。
- **LaTeX 排版定制**（均仅对 `-L` 生效）：
  - `--no-toc-links`：目录条目不带跳转到对应题目页的超链接（默认带超链接）；
  - `--toc-backlinks`：每页页眉的页码变成跳回目录页的超链接（默认无超链接）；
  - `--set-font-cover-page` / `--set-font-body-zh-CN` / `--set-font-body-en-US` / `--set-font-body-codes` / `--set-font-title-zh-CN` / `--set-font-title-en-US`：分别设置封面标题、正文中文、正文及题目大标题西文（不含公式）、代码块与正文黑体部分西文、标题中文（大标题、小节、目录与页眉）、标题西文（小节、目录与页眉；大标题西文随正文西文）的字体，参数既可填**系统已安装的字体名称**，也可填**字体文件地址**；
  - `--no-bilibili-link`：bilibili 视频 URL 输出为普通文本而非超链接（默认超链接）；
  - `--set-cover-title`：自定义封面标题（`-M` 下对应一级标题）。
- **LaTeX 默认字体方案**：`-L` 生成文档时使用 ctex 宏包的 `fontset=` 机制选择整套中文字体——Windows 用 `fontset=windows`，macOS 用 `fontset=mac`；Linux 在程序运行阶段解析 `/etc/os-release`，Ubuntu / Kubuntu 等 Ubuntu 系列发行版用 `fontset=ubuntu`，其他发行版用 `fontset=fandol`。正文与题目大标题默认使用 fontset 预设的正文字体，小节标题的中文默认使用预设黑体、西文（不含公式）则与代码块使用同一字体。
- **代码字体回退链**：代码块西文，以及正文中黑体部分的西文，按 **Consolas → Menlo → DejaVu Sans Mono** 的顺序回退（`--set-font-body-codes` 可统一指定该字体）；中文字体仍由 ctex fontset 的黑体方案控制，不受影响。三种等宽字体都不存在时保留 fontspec 默认等宽字体（小标题西文则沿用正文字体）。
- **标签字体跟随正文**：题目标签使用正文中西文字体，`--set-font-body-zh-CN` / `--set-font-body-en-US` 会同步作用于标签文字。
- **跨平台兼容**：兼容 **Windows、macOS、Linux** 的主流现代版本：
  - Windows 下输出/缓存路径按 UTF-8（宽字符）处理，支持中文文件名（如 `--output 题册.tex`）与含中文用户名的缓存目录；
  - Windows 传统控制台自动启用 ANSI 转义解析，彩色与进度输出不乱码；
  - Windows（MSVC / MinGW-w64）构建自动使用内置的 `getopt` 兼容实现（语义与 GNU getopt 一致，含长选项缩写与参数重排），macOS / Linux 使用系统 `getopt`。
- **图片下载**：并行下载题面中的图片到本地缓存；导出 LaTeX 时图片引用会替换为缓存文件路径。
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

## 使用方法

```
用法：luogu-extract [选项]

选项：
  -U, --update          更新题目列表缓存（latest.ndjson）与标签缓存（tags.json）
  -M, --markdown        筛选并导出 Markdown（默认输出 problems.md）
  -L, --latex           筛选并导出 LaTeX（默认输出 problems.tex）
                        （-M 与 -L 不能同时使用）
      --tags            按官方分类打印标签 ID 对照表（可与 -h 组合使用）
      --tag <name|ID>...
                        按标签筛选；多个值可用空格分隔或重复 --tag，题目须包含全部标签；
                        引号整体恰好等于已知标签名（如 "NOIP 普及组"）时按一个标签处理
      --difficulty <spec>
                        按难度（0~8）筛选；支持区间写法（如 1-4），多组值可用空格
                        分隔或重复 --difficulty
      --type <B|P>      按题目类型筛选（可重复，空表示全部类型）
      --pid <pid>...    按题号精确筛选；多个值可用空格分隔或重复 --pid。
                        不能与 --tag、--difficulty、--type 同时使用，
                        且每个题号必须存在于题目列表缓存中
      --pid-range <a>-<b>
                        按题号闭区间筛选；多组值可用空格分隔或重复 --pid-range。
                        一组范围两端必须为同一题库（如都为 P 题库或都为 B 题库，
                        多组范围间可不为同一题库），且两端点均须存在于缓存中。
                        可与 --tag、--difficulty、--type 同时使用
      --lang <zh-CN|en> 题面语言（默认 zh-CN；en 缺失时回退中文）
      --show <NN>       仅 -M 有效：第 1 位=是否显示难度，第 2 位=是否显示标签
                        （默认 11）；隐藏标签仅隐藏「算法」类标签，其他类型始终显示
      --output <file>   输出文件路径（默认 problems.md / problems.tex）

LaTeX 排版选项（仅在使用 -L 时有效）：
      --no-toc-links    目录条目不带跳转到对应题目页的超链接（默认带超链接）
      --toc-backlinks   每页页眉处的页码为跳回目录页的超链接（默认无超链接）
      --set-font-cover-page <font>
                        设置封面标题字体；<font> 为系统已安装的字体名称或字体文件地址
      --set-font-body-zh-CN <font>
                        设置题面正文中文字符的字体（名称或字体文件地址）
      --set-font-body-en-US <font>
                        设置题面正文及题目大标题中的西文字符字体
                        （名称或字体文件地址；不作用于公式）
      --set-font-body-codes <font>
                        设置代码块西文，以及正文黑体部分西文的字体（名称或字体文件地址；
                        默认按 Consolas → Menlo → DejaVu Sans Mono 回退）
      --set-font-title-zh-CN <font>
                        设置题目大标题、小节标题、目录页标题与每页页眉标题中的中文字体
                        （名称或字体文件地址）
      --set-font-title-en-US <font>
                        设置小节标题、目录页标题与每页页眉标题中的西文字体；题目大标题
                        西文跟随 --set-font-body-en-US（名称或字体文件地址）
      --no-bilibili-link
                        bilibili 视频 URL 输出为普通文本而非超链接（默认超链接）
      --set-cover-title <title>
                        设置封面标题（-L，默认 luogu extract）或 Markdown 一级标题
                        （-M，默认 洛谷题目导出）
  -h, --help            显示帮助
  -V, --version         显示项目简介、版本号、版权声明与项目仓库链接
                        （不能与其他参数同时使用）
```

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
```

### 参数说明

| 选项 | 含义 |
| --- | --- |
| `-U, --update` | 更新题目列表缓存（`latest.ndjson`）与标签缓存（`tags.json`） |
| `-M, --markdown` | 筛选并导出 Markdown（默认输出 `problems.md`） |
| `-L, --latex` | 筛选并导出 LaTeX（默认输出 `problems.tex`） |
| `--tags` | 按官方分类打印标签 ID 对照表（可与 `-h` 组合） |
| `--tag <name\|ID>...` | 按标签筛选；多个值可用空格分隔或重复 `--tag`，题目须包含全部标签；引号整体恰好等于已知标签名（如 `"NOIP 普及组"`）时按一个标签处理 |
| `--difficulty <spec>` | 按难度（$0\sim 8$）筛选；支持区间写法（如 `1-4`），多组值可用空格分隔或重复 `--difficulty` |
| `--type <B\|P>` | 按题目类型筛选（可重复，空表示全部类型） |
| `--pid <pid>...` | 按题号精确筛选；多个值可用空格分隔或重复 `--pid`。不能与 `--tag`、`--difficulty`、`--type` 同时使用 |
| `--pid-range <a>-<b>` | 按题号闭区间筛选；多组值可用空格分隔或重复 `--pid-range`。一组范围两端必须为同一题库（如都为 `P` 题库或都为 `B` 题库，多组范围间可不为同一题库）。可与 `--tag`、`--difficulty`、`--type` 同时使用 |
| `--lang <zh-CN\|en>` | 题面语言（默认 `zh-CN`；`en` 缺失时回退中文） |
| `--show <NN>` | 仅 `-M` 有效：第 $1$ 位=是否显示难度，第 $2$ 位=是否显示标签（默认 `11`）；隐藏标签仅隐藏「算法」类标签，其他类型始终显示 |
| `--output <file>` | 输出文件路径（默认 `problems.md` / `problems.tex`） |
| `--no-toc-links` | 仅 `-L` 有效：目录条目不带跳转到对应题目页的超链接（默认带超链接） |
| `--toc-backlinks` | 仅 `-L` 有效：每页页眉处的页码为跳回目录页的超链接（默认无超链接） |
| `--set-font-cover-page <font>` | 仅 `-L` 有效：设置封面标题字体；`<font>` 为系统已安装的字体名称或字体文件地址 |
| `--set-font-body-zh-CN <font>` | 仅 `-L` 有效：设置题面正文中文字符的字体（名称或字体文件地址） |
| `--set-font-body-en-US <font>` | 仅 `-L` 有效：设置题面正文及题目大标题中的西文字符字体（名称或字体文件地址；不作用于公式） |
| `--set-font-body-codes <font>` | 仅 `-L` 有效：设置代码块西文，以及正文黑体部分西文的字体（名称或字体文件地址；默认按 `Consolas` → `Menlo` → `DejaVu Sans Mono` 回退） |
| `--set-font-title-zh-CN <font>` | 仅 `-L` 有效：设置题目大标题、小节标题、目录页标题与每页页眉标题中的中文字体（名称或字体文件地址） |
| `--set-font-title-en-US <font>` | 仅 `-L` 有效：设置小节标题、目录页标题与每页页眉标题中的西文字体；题目大标题西文跟随 `--set-font-body-en-US`（名称或字体文件地址） |
| `--no-bilibili-link` | 仅 `-L` 有效：bilibili 视频 URL 输出为普通文本而非超链接（默认超链接） |
| `--set-cover-title <title>` | 设置封面标题（`-L`，默认 `luogu extract`）或 Markdown 一级标题（`-M`，默认 `洛谷题目导出`） |
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

图片文件名仅包含哈希值与扩展名：对完整 URL 分别以官方偏移基数（`0xcbf29ce484222325`）与官方素数（`0x100000001b3`）为种子计算两路 64 位 FNV-1a，拼成 128 位后输出 32 位十六进制作为文件名主体（不含 URL 原文，避免不同图床的同名图片互相覆盖）；扩展名取自 URL 路径并做白名单清洗，非法/超长扩展名丢弃。已存在的文件会跳过。下载时按 CPU 核心数并行，洛谷图床（`luogu.com.cn`）的图片会串行下载并保持 0.5~3 秒随机间隔，避免请求过快。

## 导出格式说明

### Markdown（`-M`）

| 项目 | 说明 |
| --- | --- |
| 文件头 | 题目总数与筛选条件 |
| 题目分节 | 每道题以 `---` 分隔，以 `# <题号> <标题>` 作为章节标题 |
| 单题内容 | 难度、标签、时空限制，以及题目背景、题目描述、输入格式、输出格式、输入输出样例、说明/提示 |
| 一级标题 | 可用 `--set-cover-title` 自定义（默认 `洛谷题目导出`） |

### LaTeX（`-L`）

| 项目 | 说明 |
| --- | --- |
| 文档结构 | 完整可编译的 `.tex` 文档：封面、目录、页眉页码，章节不编号 |
| 封面 | 标题可用 `--set-cover-title` 自定义（默认 `luogu extract`）；作者「luogu-extract」带有指向项目仓库的超链接 |
| 数学公式 | 由 `unicode-math` + `Latin Modern Math` 统一排版 |
| 表格 | 自动转换洛谷的合并语法（单元格恰为 `^` 时向上合并、恰为 `<` 时向左合并）；表头自动加粗，表头中的公式同样加粗 |
| 图片 | 只引用缓存中已有的图片，缺失的图片会被跳过而不影响编译；过大的图片自动缩小到版心内，小图片保持原始大小；xelatex 无法加载的格式（GIF/WebP/SVG/BMP/ICO 等）会被跳过 |
| 视频 | 只输出链接；加 `--no-bilibili-link` 后输出为普通文本 |
| 目录 | 目录条目默认可跳转到对应题目；加 `--no-toc-links` 后不带超链接 |
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
- `-V` / `--version` 与其他参数（含 `-h`、`--tags`、`-M`、`-L` 等）或多余的位置参数同时使用（该参数必须单独使用）；
- 出现了程序没有的未知参数（提示使用 `-h, --help` 查看帮助）。

## 待添加功能
- [ ] 导出 LaTex 文档时可选是否带题目难度和标签；
- [ ] 导出相应题解；
- [ ] 简易命令行交互程序，通过交互设置下载参数。

## 许可证

本项目（**luogu-extract**）以 **GNU Lesser General Public License v3.0 或任何更新版本**（SPDX: `LGPL-3.0-or-later`）授权发布，版权 © 2026 by-ty，完整许可文本见 [LICENSE](LICENSE)。

本项目派生自以 MIT 协议发布的 [luogu-export](https://github.com/sacharei/luogu-export)（Copyright © 2026 sacharei）。按 MIT 协议要求，原版权与许可声明完整保留于 LICENSE 文件「Original MIT License」一节。

## 鸣谢

- 本项目派生自 [sacharei/luogu-export](https://github.com/sacharei/luogu-export)（MIT 协议，Copyright © 2026 sacharei），感谢原作者的贡献；
- 本项目部分代码由 [DeepSeek](https://www.deepseek.com/) 辅助生成。
