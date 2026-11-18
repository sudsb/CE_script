# CE 脚本编辑器

一个面向 **Cheat Engine** 的轻量级 Notepad 风格编辑器，同时支持 Lua、Auto Assembler 与内嵌 C 代码。纯 Python + Tkinter，无第三方运行时依赖。

## 功能

- **语法高亮**：Lua / Auto Assembler / C 分段识别，`[ENABLE]` 区块、指令、寄存器、跳转、数字、字符串、注释分别着色。
- **自动补全**：输入 `Ctrl+Space` 或直接键入前缀，弹出按当前语言排序的 API 补全；参数签名实时显示。
- **API 参考面板**：600+ 条 CE 与 Lua API，按分类分组，支持搜索、分类筛选、双击或回车插入。
- **实时校验**：括号配对、未闭合字符串/注释即时提示，状态栏显示位置与语法状态。
- **格式化**：Lua 自动空格、缩进、对齐；AA 指令对齐；XML 智能重排。支持格式化整篇文档。
- **查找替换**：高亮全部匹配、区分大小写、逐个/全部替换、`Enter`/`Shift+Enter` 跳转。
- **编辑辅助**：自动配对括号与引号、回车自动缩进、Tab/Shift+Tab 多行增减缩进、`Ctrl+/` 注释、`Ctrl+D` 复制行。
- **CE 表格互转**：从 `.CT`/`.xml` 提取 `AssemblerScript`/`LuaScript`，或将脚本导出为可导入的单条目 CE 表格。
- **主题与动画**：浅色/深色主题、行号、字号缩放，补全弹层淡入、参考面板滑动、按钮悬停渐变。
- **无边框界面**：不显示任何按钮与系统蓝色标题栏；拖动顶部标题条移动窗口、双击最大化、右下角拖拽缩放，自定义标题条显示文档名。
- **全局右键菜单**：编辑区、行号栏、滚动条、状态栏、输入框、参考树、终端与日志窗口都支持右键（macOS 支持 Ctrl+点击，另可用 `Shift+F10` / 菜单键）；文件、编辑、视图、工具与退出操作全部集中于此。
- **底部终端与日志**：`Ctrl+\` 打开终端、`Ctrl+0` 打开日志，面板高度与左右分栏均可拖拽调整；日志显示打开/保存文件路径、主题切换、命令与异常等运行信息。
- **磨砂玻璃补全**：半透明分层与柔和阴影；弹层自动避开屏幕边缘、空间不足时向上翻转并压缩高度。
- **双击选词与动态补全**：双击选中整个单词范围（含 `$` 符号）；自动提取当前 `[ENABLE]`/`[DISABLE]` 区块与全文档的自定义符号，优先填入补全窗口。
- **强化语法检查**：括号配对、未闭合字符串/注释、区块配对、`alloc`/`dealloc` 与注册/取消注册配对、AA 标签与跳转目标、Lua `function`/`if`/`for` 与 `end` 平衡；每项可在设置中单独开关，诊断同时输出到日志窗口。
- **模板代码**：`$ver$` 占位符自动取双击选中词、剪贴板中已定义的变量或首个 `alloc` 定义；`registersymbol()` 等绑定函数补全自动生成 `registersymbol(axcc)`。输入 `alloc(axcc，8)` 回车后按 CE 官方配对语义生成：`alloc ↔ dealloc`、`registersymbol ↔ unregistersymbol`；注册语句写入 `[ENABLE]`，`unregistersymbol` 与 `dealloc` 写入 `[DISABLE]`，缺失的段会自动创建。模板可自定义并绑定触发函数，但配对语句始终落在正确的标签范围内。
- **独立设置界面**：`Ctrl+,` 或右键菜单打开，集中管理常规、补全、语法检查、代码模板与快捷键。
- **多重选区**：`Ctrl+D` 依次选中文档中下一个相同词，可重复添加多个选区；输入或退格会同时替换所有选区。复制当前行改为 `Ctrl+Shift+D`。
- **Vim / nvim 模式**：普通 / 插入 / 可视 / 可视行四种模式，`h j k l w b 0 $ G gg` 移动、`3j` / `2dd` 数字前缀、`x dd yy p P u`、`v V`、`>> <<`、`/`、`Ctrl+R`，以及 `:w` `:q` `:42` 等命令；`Ctrl+Alt+V` 临时开关，也可在设置或右键菜单中开启。
- **可见的语法检查**：状态栏显示已运行的检查项数（如「✓ 语法正常 · 8 项检查」），诊断同步写入日志窗口，右键菜单可打开完整检查报告；新增 AA 方括号配对、`{$lua}`/`{$asm}` 块配对、`alloc` 重复定义、无法识别的 AA 语句等检查。
- **任务栏可用并有投影**：无边框窗口在 Windows 下保留任务栏与 Alt+Tab 入口，并通过 WS_EX_LAYERED 加一层系统级柔和投影，让窗口边界与桌面清晰分离。
- **高 DPI 与兼容性**：Windows 每显示器 DPI 感知、几何尺寸随缩放计算，Linux/macOS 滚轮与菜单适配；小窗口默认居中打开。
- **偏好记忆**：主题、字号、行号、参考面板、窗口位置、最近文件自动保存。

## 环境要求

- Python 3.11+
- Tkinter（Windows 官方 Python 自带；Linux 需安装 `python3-tk`）

## 运行

```bash
python run.py
# 或
python -m ce_edit
```

安装为命令（可选）：

```bash
pip install -e .
ce-edit
```

支持外部调用打开文件（命令行参数、`python -m ce_edit <文件>`、Windows「打开方式」或 `file://` URI）：

```bash
ce_edit.exe D:\scripts\table.lua
python run.py D:\scripts\table.lua
```

## 快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` | 新建 / 打开 / 保存 |
| `Ctrl+Shift+S` | 另存为 |
| `Ctrl+F` / `Ctrl+H` / `Ctrl+G` | 查找 / 替换 / 转到行 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Ctrl+/` | 切换注释 |
| `Ctrl+Shift+F` | 格式化文档 |
| `Ctrl+Space` | 代码补全 |
| `F1` | 打开 API 参考 |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+Shift+0` | 增大 / 减小 / 重置字号 |
| `Ctrl+\` / `Ctrl+0` | 打开或关闭底部终端 / 日志窗口 |
| `Ctrl+Q` | 退出（未保存时弹出确认） |
| `Esc` | 关闭补全或查找浮层 |
| 右键 / `Shift+F10` | 打开当前控件的上下文菜单 |
| `Alt+F` / `Alt+E` / `Alt+V` / `Alt+H` | 直接弹出文件 / 编辑 / 视图 / 帮助菜单 |
| `Ctrl+,` | 打开设置界面 |
| `Ctrl+D` | 选择下一个相同词（可重复形成多重选区） |
| `Ctrl+Shift+D` | 复制当前行 |
| `Ctrl+Alt+V` | 开关 Vim / nvim 模式 |
| Vim 普通模式 | `Esc` 返回 · `i a I A o O` 插入 · `v V` 可视 · `h j k l w b 0 $ G gg` 移动 · `x dd yy p P u` 编辑 · `:w` `:q` `:42` 命令 |
| 拖动顶部标题条 / 双击 | 移动窗口 / 最大化或还原 |
| 在编辑区双击 | 选中当前单词范围 |

编辑时输入 `(`、`[`、`{`、`"`、`'` 会自动配对；选中多行后 `Tab` / `Shift+Tab` 可整体增减缩进。

## 配置文件

- Windows：`%APPDATA%\ce_edit\config.json`
- 其他平台：`~/.config/ce_edit/config.json`

## 开发

```bash
uv sync --extra dev
uv run pytest
uv run pytest --cov=ce_edit --cov-report=term-missing
uv run ruff check .
```

GUI 冒烟检查（会短暂创建窗口）：

```bash
uv run python tools/smoke_gui.py
```

补全数据由 `tools/gen_completion_data.py` 生成到 `ce_edit/data/completions.json`。

## 打包

项目已提供 `ce_edit.spec`，用打包脚本一键生成独立可执行程序：

```bash
python tools/build.py --install-deps
```

常用参数：`--onedir` 生成目录形式（exe + 依赖文件，而非单一 exe）、`--clean` 清理缓存与旧产物、`--check` 打包前运行 ruff 与 pytest、`--dry-run` 只打印命令、`--dist` / `--work` 自定义输出与缓存目录、`--quiet` 减少日志。

```bash
python tools/build.py --onedir
```

单文件模式产物位于 `dist/ce_edit.exe`（Windows）或 `dist/ce_edit`；`--onedir` 模式产物位于 `dist/ce_edit/ce_edit.exe` 及其运行库目录。

## 目录

```
ce_edit/
  app.py           主窗口、菜单、文件与偏好
  editor.py        文本编辑控件、行号、语法与括号高亮
  autocomplete.py  补全弹层
  reference.py     API 参考面板
  findbar.py       查找替换条
  lexer.py         Lua / AA / C 词法与括号校验
  formatter.py     格式化与缩进
  ce_xml.py        CE 表格 XML 互转
  themes.py        主题
  ui.py            主题化控件
  animation.py     动画辅助
  dock.py          底部终端与日志面板
  analysis.py      文档分析、关键词提取与强化语法检查
  templates.py     模板规则、$ver$ 变量与配套代码生成
  settings.py      独立设置界面
  vim.py           nvim 风格模态编辑
tests/             pytest 测试
tools/             生成、冒烟、打包与审计脚本
```
