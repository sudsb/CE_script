# CeScript（Notepad3 CE 编辑器）使用说明与功能详解

> 适用程序：本仓库构建产物 `Notepad3.exe`（产品名 `CeScript`，基于 Notepad3 / Scintilla / Lexilla）
> 文档依据：当前工作区源码（`src/`、`language/`、`Build/Notepad3.ini`、`ce_bridge/`、`ce_lsp_server/`）与内置《本地文档》（`本地文档.txt`）
> 说明：本文讲“怎么用、有什么功能”；函数级完整条目（2400+ 个关键词）请查阅程序内《本地文档》。

---

## 目录

1. [程序概览](#1-程序概览)
2. [运行环境与启动](#2-运行环境与启动)
3. [界面布局](#3-界面布局)
4. [基础编辑功能](#4-基础编辑功能)
5. [CE 脚本专用功能](#5-ce-脚本专用功能)
6. [脚本运行：本地 Lua 与 CE 桥接](#6-脚本运行本地-lua-与-ce-桥接)
7. [LSP 智能语言功能](#7-lsp-智能语言功能)
8. [快捷键速查](#8-快捷键速查)
9. [配置参考](#9-配置参考)
10. [命令行用法](#10-命令行用法)
11. [常见问题与排错](#11-常见问题与排错)
12. [附录：文档与源码索引](#12-附录文档与源码索引)

---

## 1. 程序概览

CeScript 是一个 Windows 平台的轻量级文本/代码编辑器，在 Notepad3 基础上内置了 **Cheat Engine 脚本编辑**能力。它既可以当作普通记事本替代品使用，也可以在同一份文档里编辑、检查、格式化并运行：

- **CE Lua 脚本**（Cheat Engine 的 Lua API）
- **Auto Assembler（AA）表脚本**（`[ENABLE]` / `[DISABLE]` 段）
- **x86 / x64 汇编**（指令、寄存器、内存操作数）
- **内嵌 C 代码**（`{$ccode}` / `{$CODE}` 段）

核心能力一览：

| 能力 | 说明 |
| --- | --- |
| 混合语法高亮 | 同一文档内识别 Lua / AA / 汇编 / C 区域，按语言分别着色 |
| 智能补全 | 键入即弹出补全列表，并按光标所在语言（AA / Lua / C）加权排序 |
| 签名帮助与文档 | 补全详情、悬停提示、`Ctrl+Alt+D` API 参考面板、完整说明窗格 |
| 实时检查 | 括号、字符串、注释、区块配对、标签、AOB 等结构检查；可选语义检查 |
| 格式化 | Lua 缩进/空格规范化、AA 指令对齐、C 行尾清理 |
| 模板与配套代码 | `alloc` ↔ `dealloc`、`registersymbol` ↔ `unregistersymbol` 自动成对生成 |
| 表格互转 | 导入/导出 `.CT` / `.xml` 表格，复制为 CE 脚本或表格 XML |
| 本地运行 Lua | 不启动 CE，直接用 `lua53`/`lua54` 运行纯 Lua 脚本 |
| CE 桥接运行 | 通过命名管道把脚本送进正在运行的 Cheat Engine 执行，回传输出/返回值/错误 |
| AA 一键启用/禁用 | 自动识别 AA 表脚本，用 CE 原生 `autoAssemble` 启用；再次运行执行 `[DISABLE]` 还原，并监视自定义变量（支持 `// np3watch 名称 类型 进制 数量 [if 条件]` 手动声明、寄存器监控、`begin`/`end` 代码块与一键消除） |
| 取消执行 | 停止变量刷新，并回滚仍处于启用状态的 AA 脚本 |
| 内置语言服务器 | `CeLspServer.exe` 提供补全、诊断、语义高亮、跳转、引用、符号、格式化等 LSP 能力 |

程序仍是便携式设计：解压即可运行，配置写在程序目录的 INI 文件里。

---

## 2. 运行环境与启动

### 2.1 系统要求

- **操作系统**：Windows 8.1 / 10 / 11
- **架构**：x86（Win32）、x64、x64_AVX2、ARM64
- **可选组件**：
  - Cheat Engine 7.x：使用“强制在 CE 中运行”、AA 启用/禁用或桥接功能时需要
  - Lua 运行时 DLL（`lua53-64.dll` / `lua53.dll` / `lua54.dll` 等）：使用“仅本地运行 Lua”时需要，CE 安装目录通常自带
- **不使用 CE 时**，程序可完全作为普通文本编辑器离线使用

### 2.2 启动与打开文件

直接运行 `Notepad3.exe`，或：

```bat
Notepad3.exe D:\scripts\table.lua
Notepad3.exe /g 120,5 D:\scripts\table.cea
```

- 支持命令行参数打开、拖放文件、Windows“打开方式”、`file://` 链接
- 支持多文件参数与 `/g`（跳转到行列）、`/m`（打开并查找）等开关，详见[命令行用法](#10-命令行用法)

### 2.3 支持的文件类型与语言模式

CE 专用语法方案名为 **Cheat Engine Script**，对应扩展名：

```
.cea  .aa  .cet  .ct  .lua
```

> 注意：`.lua` 也会进入 CE 混合语法方案（仍能正确高亮 Lua，同时识别 CE API 与 AA 段）。普通 Lua 工程文件同样可以打开编辑。

其他 50 多种语言（C/C++、Python、PowerShell、JSON、Markdown、SQL 等）由 Lexilla 词法器提供，可在“语言/语法方案”菜单中切换。

### 2.4 配置文件位置

便携优先，程序会在自身目录查找：

1. `CeScript.ini`（首选，避免与同名 Notepad3 安装互相污染）
2. `Notepad3.exe` 同名的 `.ini`

编辑器中按 **Ctrl+F7** 可直接打开当前 INI 文件。用户可见设置的完整目录见 `readme/config/Configuration.md` 与 `Build/Notepad3.ini`。

---

## 3. 界面布局

```
┌──────────────────────────────────────────────────────────────┐
│ 菜单栏：文件 编辑 搜索 视图 CE 外观 设置 帮助 ...             │
├──────────────────────────────────────────────────────────────┤
│ 工具栏（可隐藏）                                              │
├──────────────────────────────────────────────────────────────┤
│ 标签栏（多文档时显示）                                        │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│   编辑区（Scintilla，支持行号、折叠、书签、多选、缩放）        │
│                                                              │
├──────────────────────────────────────────────────────────────┤
│ CE 输出 / 日志面板（停靠或独立窗口；含命令输入框）              │
├──────────────────────────────────────────────────────────────┤
│ 状态栏：行列、编码、换行、选区、检查状态、缩放等               │
└──────────────────────────────────────────────────────────────┘
```

| 区域 | 用途 |
| --- | --- |
| **CE 输出面板** | 汇总脚本 `print()` 输出、返回值、错误、检查日志、桥接状态和编辑器日志；底部输入框可直接执行 `cmd.exe` 命令。`Ctrl+Alt+Shift+O` 显示/隐藏，`Ctrl+Alt+Shift+C` 清除日志，`Ctrl+Alt+Shift+P` 独立窗口/停靠切换 |
| **CE Reference 面板** | 2000+ 条 CE/Lua API 分类列表，支持搜索、分类筛选、双击/回车插入。`Ctrl+Alt+D` 打开（视图菜单） |
| **补全详情浮窗** | 补全列表旁显示当前条目的签名与说明；`Ctrl+Alt+Shift+H` 打开/关闭完整说明窗格 |
| **CE 设置对话框** | `Ctrl+Alt+Shift+S`，集中管理补全、模板、词典等选项 |
| **本地文档** | 帮助菜单中的“本地文档”，打开程序目录下的《本地文档.txt》；在补全列表中按 `F1` 会直接跳到该关键词条目 |

---

## 4. 基础编辑功能

以下为 Notepad3 原有能力，CE 分支全部保留：

- **文件与编码**：ANSI / UTF-8 / UTF-8 BOM / UTF-16 / UTF-32 打开与转换，自动检测编码（uchardet），换行符 CRLF/LF/CR 切换
- **查找替换**：`Ctrl+F` / `Ctrl+H`，支持 PCRE2 正则、区分大小写、全词匹配、循环查找
- **代码折叠与书签**：折叠全部/展开全部，书签跳转
- **多选与列编辑**：多光标/多选区；`Alt+拖拽` 矩形选择
- **视图**：缩放（`Ctrl+滚轮`、10%–1000%）、自动换行、显示不可见字符、深色模式、Focused View（`Ctrl+Alt+V`）
- **实用命令**：插入 GUID、插入时间戳、大小写转换、Base64、进制转换、TinyExpr 表达式求值（输入 `表达式=?`）
- **外置工具**：grepWin 跨文件搜索（`Ctrl+Shift+F`）、MiniPath 文件浏览器（`Ctrl+M`）
- **文件监视**：外部修改提示与自动重载策略
- **多语言界面**：26 种语言资源，随系统语言自动选择（可在设置中切换）

完整菜单说明见 `readme/MenuEntriesAndCmds.md`，快捷键全集见 `readme/KeyboardShortcuts.md`。

---

## 5. CE 脚本专用功能

### 5.1 混合语法高亮

在 Cheat Engine Script 方案下，文档按区域自动识别：

- `[ENABLE]` / `[DISABLE]` 段标记
- `{$lua}` / `{$asm}` / `{$ccode}` 等嵌入标记
- Lua 关键字、标准库、CE Lua API
- Auto Assembler 命令（`alloc`、`label`、`aobscan`、`registersymbol` 等）
- x86/x64 指令、寄存器、尺寸后缀
- 类型与类（`DWORD`、`qword`、`TMemoryRecord` 等）
- 字符串、数字、注释、标签、模块名、通配字节

各分类的颜色可在 **外观 → 自定义语法高亮方案（Ctrl+F12）** 中调整；深色主题会自动套用对应的暗色取值。

### 5.2 智能补全

- **自动弹出**：在 CE 文档中键入标识符时自动出现候选列表
- **按语言排序**：光标位于 AA 段时汇编指令/AA 命令优先；位于 `{$lua}` 或纯 Lua 时 CE API 优先；C 段时 C 类型/关键字优先
- **文档内符号**：当前文档用 `alloc` / `label` / `define` / `function` / `local` 定义的符号会加入候选，并支持局部前缀匹配
- **剪贴板变量**：可开启“允许从剪贴板读取已定义变量”（默认开）
- **自动填参**：对 `registersymbol`、`unregistersymbol`、`dealloc`、`label` 等函数，补全时自动带入变量名，例如 `registersymbol(axcc)`
- **自动配对**：输入 `(` `[` `{` `"` `'` 自动补全另一半；在 AA 代码中键入自定义变量名时可自动包裹 `[]`
- **代码块骨架**：`function`、`if`、`for` 等块结构按多行模板展开
- **签名帮助**：输入 `(` 或选中候选时显示参数签名与一行说明；括号内继续输入会更新参数提示
- **补全详情窗**：选中候选时在列表旁显示该关键词的功能说明

相关开关在 **CE → CE 设置（Ctrl+Alt+Shift+S）**：剪贴板变量、自动填参、输入 `alloc(...)` 回车自动生成配套代码、保存时自动格式化、包含当前文档符号、汇编变量自动 `[]`、高亮用户符号。

### 5.3 API 参考与完整文档

| 入口 | 用法 |
| --- | --- |
| **Cheat Engine Reference**（`Ctrl+Alt+D`） | 分类 + 搜索的可停靠列表；双击或回车把条目插入光标处 |
| **补全详情浮窗** | 列表高亮条目的签名与说明，随列表关闭 |
| **悬停（停留片刻）** | 显示光标下关键词的签名、说明与用法提示 |
| **完整说明（`Ctrl+Alt+Shift+H`）** | 切换独立窗格，显示“功能/语法/示例/约束/相关/易错”六段式说明 |
| **本地文档** | 帮助菜单打开 `本地文档.txt`；补全列表中按 `F1` 定位到当前条目 |
| **外部词典** | `CeDictionary.txt` 或设置中指定路径，每行 `名称\|签名\|分类\|说明`（UTF-8） |

内置目录与补全、CallTip、参考面板使用同一数据源，条目数约 2400+，分为 51 个分类（内存读写、扫描、自动汇编、监视类型/进制、调试、Mono/.NET、string/table/math 库、汇编指令、寄存器等）。

### 5.4 语法检查与语义检查

菜单 **CE → 检查脚本（Ctrl+Alt+K）**：

- **结构检查（始终启用）**
  - 括号、引号、字符常量配对
  - 未闭合的单行/多行注释、长字符串
  - `[ENABLE]` / `[DISABLE]` 段重复与缺失
  - `{$lua}` / `{$asm}` / `{$ccode}` 指令块配对
  - Lua `function/if/for/while/do/repeat ... end` 平衡
  - AA 标签定义与跳转目标、`alloc`/`dealloc`、注册/取消注册配对
  - AOB 特征码格式与空特征码
  - 无效/无法识别的 AA 语句、方括号配对
- **语义检查（`CeLintSemantic`，默认开启）**
  - 未定义指令/变量/标签、重复定义
  - 与 CE 关键字/保留字冲突的符号名
  - Lua 未知名称与参数个数检查
  - 检查结果基于 CE 目录 + 文档内全部符号，正常代码保持安静

**结果展示**：编辑器内用彩色波浪线标出问题，行尾显示简短说明；状态栏显示检查项与结果；完整报告在 CE 输出面板/检查报告中查看。

> 只对**错误（severity 1）与警告（severity 2）**绘制波浪线；服务端返回的信息/提示级（severity 3/4）风格建议不会画线，避免干扰。

### 5.5 格式化

菜单 **CE → 格式化文档（Ctrl+Alt+M）**：

- 优先请求内置语言服务器格式化；服务不可用时回落到本地格式化器
- **Lua**：规范化缩进与单个空格，保持多行字符串/注释原样
- **AA**：标签、段标记、注释顶格；命令与指令统一一级缩进；压缩 CE 不接受的内存操作数空格（如 `[rdi - 18]` → `[rdi-18]`）
- **C**：仅去除行尾空白
- 缩进单位跟随编辑器自身的 `IndentWidth` 与 `TabsAsSpaces`，与按 Tab 的行为一致
- 默认 **保存时自动格式化**（`CeAutoFormatOnSave=1`）；关闭后保存时仍会清理 AA 内存操作数中的非法空格

### 5.6 代码模板与配套代码

**代码模板（Ctrl+Alt+Shift+T）**

- 模板字段：名称、触发函数、目标分区（CARET / ENABLE / DISABLE）、内容
- `$ver$`（以及任意 `$名称$`）会替换为当前选中词、剪贴板中已定义的变量或首个 `alloc` 定义
- 右键菜单 **CE 模板** 可直接插入；模板可绑定到某个函数名，补全接受该函数时自动套用
- 模板最多 12 个，保存在 `[Settings2] CeTemplates`

**生成配套代码（Ctrl+Alt+G）**

按 CE 官方配对语义补全缺失语句：

- `alloc` ↔ `dealloc`
- `registersymbol` ↔ `unregistersymbol`
- 注册语句写入 `[ENABLE]`，`unregistersymbol` / `dealloc` 写入 `[DISABLE]`
- 缺少的段会自动创建
- 输入 `alloc(名字, 8)` 后回车，若开启对应选项也会自动生成配套代码

### 5.7 CE 表格互转

菜单 **CE** 下：

| 命令 | 行为 |
| --- | --- |
| 导入表格（`Ctrl+Alt+N`） | 从 `.CT` / `.xml` 提取 `AssemblerScript` 或 `LuaScript`，替换当前编辑内容 |
| 导出表格（`Ctrl+Alt+Z`） | 把当前文本包装成可导入 CE 的单条目表格 |
| 复制为 CE 脚本 | 复制当前内容为纯 CE 脚本（确保段结构完整） |
| 复制为表格 XML | 复制为单条目表格 XML |
| 粘贴表格 XML | 从剪贴板 XML 中提取脚本 |

### 5.8 右键转换、生成与重命名

在 CE 文档中选中内容后右键，会出现对应子菜单：

- **转换（数字）**：十进制 / 十六进制 / 二进制 / 八进制 / 浮点数（位重解释）/ 双精度（位重解释）/ 转为 float / 转为 double / 还原原数值 / 字节数组（db）/ AOB 特征码
- **转换（内存操作数）**：`byte ptr [...]` / `word ptr [...]` / `dword ptr [...]` / `qword ptr [...]`
- **转换（寄存器）**：64 / 32 / 16 / 8 位寄存器互换
- **生成（符号名）**：`alloc(...)`、`label(...)`、`registersymbol(...)`、`unregistersymbol(...)`、`dealloc(...)`、`define(...)`、完整声明（alloc + 符号 + 标签）
- **重命名符号**：对选中词或光标下标识符做全文档整词替换，单个撤销步骤；与 CE 保留字冲突时给出警告

### 5.9 用户符号高亮

开启后，文档中定义的变量名与 `label` 标签名会以专用样式高亮，便于在长脚本中快速分辨自定义符号与内置 API。设置项：`CeHighlightUserSymbols`。

---

## 6. 脚本运行：本地 Lua 与 CE 桥接

### 6.1 三种运行方式

| 菜单命令 | 快捷键 | 行为 |
| --- | --- | --- |
| **运行脚本（自动）** | `Ctrl+Alt+Shift+R` | 自动判断：纯 Lua 走本地运行时；AA 表脚本或引用 CE API 的脚本走 CE 桥接 |
| **仅本地运行 Lua** | `Ctrl+Alt+Shift+L` | 强制本地 Lua 运行时，不连接 CE，也不包装 AA |
| **强制在 CE 中运行** | — | 强制通过 CE 桥接执行（需要 CE 正在运行） |

“运行脚本（自动）”“仅本地运行 Lua”“强制在 CE 中运行”“取消执行”“一键消除监控指令”“部署/连接 CE 桥接”“CE 输出面板”这些运行命令位于 **CE 菜单**。若当前界面语言 DLL 较旧、缺少这些菜单项，程序启动时会自动把它们追加到 CE 菜单末尾。

### 6.2 执行 AA 表脚本（新增）

含 AA 段标记，或无段但含 CE 专属 AA 指令的文档，会被自动识别并包装成 CE 原生 `autoAssemble()` 请求，而不是当作 Lua 源码编译。

**识别规则**

- 文档包含 `[ENABLE]` / `[DISABLE]` 段标记
- 或无段，但行首（忽略空白）出现 `alloc(`、`globalalloc(`、`sharedalloc(`、`dealloc(`、`registersymbol(`、`unregistersymbol(`、`aobscan*`、`readmem(`、`loadbinary(`、`createthread(`、`fullaccess(`、`kalloc(` 等 CE 专属指令。单独的 `label(` 不触发 AA 识别，避免把普通 Lua 代码误判为 AA

**启用与禁用**

1. 首次运行：先检查 CE 是否已附加目标进程（`getOpenedProcessID`），未附加直接报错；随后用 `autoAssembleCheck(script, true)` 做不写内存的预检，再调用 `autoAssemble(script)` 启用，并保存 CE 返回的 `disableInfo`
2. 再次运行同一文档：用 `autoAssembleCheck(script, false)` 预检后，以保存的 `disableInfo` 调用 `autoAssemble(script, disableInfo)` 执行 `[DISABLE]`，由 CE 释放内存、注销符号、还原原字节
3. 启用/禁用过程都会把目标 PID、`ENABLE` / `DISABLE` 阶段和 CE 返回的错误回传到 CE 输出面板

**段结构要求**

- 带段标记的脚本必须**恰好各有一个** `[ENABLE]` 与 `[DISABLE]`；缺失或重复会弹出警告并中止
- 无段 AA 脚本只允许启用一次；再次运行因没有 `[DISABLE]` 段而报错，不会重复写入
- 禁用时使用的是**当前文档内容**中的 `[DISABLE]` 段；启用后修改过脚本再禁用，请确认还原逻辑仍然有效

**状态与限制**

- 启用状态以文档路径的 FNV-1a 哈希为键保存在 CE 的 Lua 状态中（未保存文档共用一个键，建议先保存文件）
- 状态只存在于 CE 进程内，重启 CE 会丢失；重启后应先在 CE 中手动还原，再重新启用
- 前提：CE 已运行、桥接已部署（见 6.5）且已附加目标进程
- 同一时刻只允许一个脚本执行；脚本连同包装代码不能超过 8 MB 帧上限
- `仅本地运行 Lua` 不会包装 AA：AA 文档在该模式下会按 Lua 语法执行并报错，请使用自动或强制 CE 模式
- 桥接协议无需更新：只调用 CE 原生 `autoAssemble` / `autoAssembleCheck`，已部署的旧版 `np3_ce_bridge.lua` 可直接使用

**自定义变量监视**

- 启用成功后，自动从 `alloc` / `globalalloc` / `sharedalloc` / `allocnx` / `allocxo` / `define` / `registersymbol` / `aobscan*` / `kalloc` 等声明中提取用户数据符号（跳过 CE 内置的 `newmem` 与 `label()` 代码标签，去重，最多 24 个）
- 手动监控：在 AA 脚本内写注释指令（CE 会忽略注释，编辑器解析后交给刷新定时器）：

  ```text
  // np3watch <名称> [类型] [进制] [数量] [if <条件>]
  // np3watch begin [if <条件>]
  // np3watch end
  // np3unwatch <名称>
  // np3nowatch <名称>
  // np3watch off
  // np3watch on
  ```

  - 类型：`auto`（默认）、`byte`、`word`、`dword`、`qword`、`pointer`、`float`、`double`、`string`、`wstring`、`array`、`reg`（寄存器，一般无需手写，命中寄存器名时自动使用）；别名 `u8`~`u64`、`i8`~`i64`、`ptr`、`f32`/`f64`、`str`/`wstr`、`arr`
  - 数组可直接用“标量类型 + 数量”表示，例如 `byte hex 16`；`array` 等价于 `dword` 数组（默认 4 项）
  - 进制：`auto`（默认）、`hex`、`dec`、`bin`；也接受 `hexadecimal` / `decimal` / `binary`
  - 数量：数组元素个数（默认 4）或字符串最大长度（默认 64）。`byte hex 16` 表示按十六进制显示 16 个字节
  - 省略类型时先按声明推断（`define` 字面量、`alloc` 大小、`db`/`dw`/`dd`/`dq`），再按运行时内容探测：可打印字符串、浮点合理性、数值宽度；仍无法判断时按 `dword` 显示
  - `// np3unwatch` 与 `// np3nowatch` 只从自动列表中排除单个符号；`// np3watch off` 关闭全部自动项、仅保留手动项，`on` 恢复
  - 同名对象后写覆盖前写；类型/进制/数量可任意省略，顺序不限；注释标记使用 `//`（CE Auto Assembler 会忽略该行）
  - **寄存器监控**：名称命中寄存器（`rax`~`r15`、`rip`、`eax`~`esp`、`eip`、`ax`~`sp`、`al`/`ah`/`sil`/`r8b`/`r8d` 等、`flags`/`eflags`，小写）时自动按 `reg` 类型监控，每 500 ms 用 CE `getRegisters` 读取，默认 `hex` 显示；32/16/8 位别名由 64 位值掩码/移位派生。示例：`// np3watch rax`、`// np3watch eax dec`
  - **条件监控**：行尾 `if <表达式>`，表达式按 Lua 规则求值，支持多条件、括号和函数调用。`||`→`or`、`&&`→`and`、`!`→`not`、`!=`→`~=`、`$1F`→`0x1F` 由编辑器自动转换，其余（`==`、`>`、`<`、`>=`、`<=`、`~=`、`and`/`or`/`not`、`x.y` 字段、`func(...)`）原样透传。表达式里的裸名称按 **寄存器 → Lua 全局变量/对象 → AA 符号（按 32 位整数读取）** 的顺序解析，因此可以直接引用脚本里定义的变量、表和 AA 符号。条件成立时才输出 `[WATCH]` 行，条件由不成立变为成立时会立即输出一行（即使值没变化）；表达式语法错误时会输出一次 `[WATCH] 名称 | 条件无效: ...`，监控本身仍然保留。被监控对象和条件里的对象互相独立。示例：

    ```text
    // np3watch rcx if rax == 3 || rbx != 4
    // np3watch rdx if rax == 1 && (rbx > 2 || !ecx)
    // np3watch rsi if myTable.hp > 0 and hp >= 10
    // np3watch hp dword hex if hp > 0
    ```
  - **代码块**：`// np3watch begin [if <条件>]` 与 `// np3watch end` 配对，把一组监控成组启用；块内监控行继承块条件，行自身再写 `if` 时两个条件用 `and` 组合；块可嵌套（最多 8 层），忘记 `end` 会让后续监控行一直带上块条件
  - **块内简写**：`begin` 与 `end` 之间可以省略 `np3watch` 前缀，直接写 `// 名称 [类型] [进制] [数量] [if 条件]`，并可用 `// begin` / `// end` 继续嵌套。简写行只认“名称 + 已知类型/进制/数量 + if 条件”，其它多词注释不会被当成监控项。示例：

    ```text
    // np3watch begin if rax == 2
    // rcx
    // rdx hex
    // r8d if r8d != 0
    // end
    ```

    与逐行写 `// np3watch rcx` 完全等价，两种写法可以混用；块外的 `// rcx` 仍是普通注释。
  - **一键消除**：菜单 **CE → 一键消除监控指令** 删除选区内（无选区则全文）所有 `np3watch` / `np3unwatch` / `np3nowatch` 注释行，以及块内的简写监控行，一次操作可整体撤销
- CE 侧每 500 ms 用 `getAddress` 异步解析地址，尚未注册的符号持续重试，每 5 秒输出一次 `[WATCH] waiting for <名称>`
- 值或类型变化时才向输出面板追加 `[WATCH] name @ 地址 = 值 | type=类型 | fmt=进制`；寄存器监控无地址，显示 `[WATCH] name = 值 | type=reg | fmt=进制`；带条件的条目追加 ` | if <条件>` 后缀；数组按逗号展开，`hex` 前缀 `0x`，`bin` 前缀 `0b`，`float`/`double` 默认用 `%g` 显示
- CE 侧缓冲最多 200 行，编辑器每秒拉取一次；值刷新采用追加新行而非原地替换，未连接 CE 时拉取静默跳过

**取消执行 / 停止刷新**

- 菜单：**CE → 取消执行 / 停止刷新**
- 立即停止变量刷新；若该文档脚本仍处于启用状态，则执行 `[DISABLE]` 段回滚；该命令只禁用、不会启用脚本
- 再次点击运行按钮同样会停止刷新并禁用；要重新启用，需在禁用完成后再运行一次

### 6.3 自动路由规则

满足任一条件即判定为“需要 CE”，改走桥接：

- 文档出现 `[ENABLE]` / `[DISABLE]` / `{$asm}` / `{$lua}` / `{$ccode}` 等段标记
- 无段但行首出现 `alloc(` / `aobscan*` / `registersymbol(` 等 CE 专属 AA 指令（见 6.2）
- 引用 CE 扩展库成员（如 `string.split`、`table.clone`）
- 引用 CE API 函数名（如 `readBytes`、`autoAssemble`、`AOBScan`）

### 6.4 本地 Lua 运行时

- 按需加载 `lua53-64.dll` / `lua53-32.dll`（或 `lua53.dll` / `lua54.dll` / `lua.dll`）
- 查找顺序：程序目录 → CE 安装目录 → `[Settings2] CeInstallPath` → 系统 `PATH`
- **建议**：把 `Notepad3.exe` 放在 Cheat Engine 安装目录，可直接复用 CE 自带的 Lua DLL
- `print()` 重定向到 CE 输出面板；`os.exit()` 被禁用；脚本最长执行 **30 秒**，超时自动中断
- 纯 Lua 标准库（`string`/`table`/`math`/`os`/`io`/`coroutine`/`debug`/`utf8`/`bit`）可用；CE 扩展库（如 `string.split`）只能在 CE 内使用

### 6.5 CE 桥接

**部署/连接（CE → 部署/连接 CE 桥接）**

1. 自动定位 Cheat Engine 安装目录
2. 把 `ce_bridge/np3_ce_bridge.lua` 复制到 `<Cheat Engine>\autorun\`
3. 启动或重启 Cheat Engine 以加载桥接
4. 通过命名管道 `\\.\pipe\NP3_CE_BRIDGE` 双向通信

若自动定位失败，可在 `[Settings2] CeInstallPath` 手动指定 CE 目录，或把桥接脚本手动复制到 CE 的 `autorun` 目录。

**执行特性**

- 脚本在 CE 自己的 Lua 状态机中执行，可直接使用 CE 原生 API：内存读写、断点、AOB 扫描、`autoAssemble`、反汇编、窗体创建等
- `print()` 输出、脚本返回值、错误信息逐行回传到 CE 输出面板
- 桥接脚本默认不弹 CE 的 Lua 窗口；需要调试桥接时可设置 `NP3_CE_BRIDGE.echoToCe = true`
- 桥接内置 `np3` 辅助表：
  - `np3.read(address, size)` / `np3.write(address, bytes)`
  - `np3.disassemble(address, count)`
  - `np3.hook(address, callback)` / `np3.unhook(id)`
  - `np3.aa(script)`、`np3.symbols(address)`、`np3.process()`
  - `np3.dumpEvents()`：取回断点回调期间缓冲的输出
- 一键执行 AA 不依赖新增桥接命令，只使用 CE 原生 `autoAssemble` / `autoAssembleCheck`，已部署的桥接脚本无需更新

### 6.6 CE 输出面板

- 显示：脚本输出、返回值、错误、检查日志、桥接连接状态、编辑器运行日志
- 级别着色：普通、错误（红）、警告（琥珀）、信息、调试（灰）；颜色随 Notepad3 明暗主题自动适配
- 底部输入框执行 `cmd.exe /c <命令>`，输出回填面板，单条命令 15 秒超时
- 停靠模式：拖动标题栏调整高度并记忆；`浮出` 按钮或 `Ctrl+Alt+Shift+P` 把面板抽取为独立窗口
- 独立窗口模式：带系统标题栏，可移动/缩放；拖动内部标题栏也可移动窗口；`停靠` 按钮或再次按 `Ctrl+Alt+Shift+P` 切回停靠
- `清空` 按钮或 `Ctrl+Alt+Shift+C` 只清除当前显示内容，后续脚本/命令输出照常写入
- `A-` / `A+` 调整字号；`x` 或独立窗口的系统关闭按钮只收起面板，不销毁日志
- 输出到达时会自动显示面板（用户手动收起后不再自动弹出）

---

## 7. LSP 智能语言功能

程序内置语言服务器 `CeLspServer.exe`（源码 `ce_lsp_server/CeLspServer.c`），编辑器作为 LSP 客户端通过 stdio JSON-RPC 通信。

### 7.1 启动与回退

- 打开 CE 文档并开始编辑时按需启动服务器
- 默认路径：程序目录下的 `CeLspServer.exe`；可用 `CeLspServerPath` 指定
- 文档超过 **1 MB** 时跳过 LSP（避免全量同步拖慢界面），使用本地补全
- 服务器不可用时，`CeLspFallback=1` 会回落到内置静态补全
- 总开关：`CeUseLsp=1`（关闭后完全使用旧本地补全路径）

### 7.2 功能对照

| 功能 | 使用方式 | 说明 |
| --- | --- | --- |
| 代码补全 | 键入标识符自动触发 | 服务端候选 + 本地词典合并，再按语言上下文重新排序 |
| 签名帮助 | 输入 `(` | 参数列表、当前参数高亮、说明 |
| 悬停提示 | 鼠标停留 | 签名与文档；可显示“未知关键词”警告 |
| 跳转到定义 | `Ctrl+Alt+Shift+F` | 光标处符号定义位置 |
| 跳转到声明 | `Ctrl+Alt+Shift+V` | 声明位置（如 `label` / `define`） |
| 查找所有引用 | `Ctrl+Alt+Shift+U` | 结果以多选区呈现，数量写入输出面板 |
| 文档符号 | `Ctrl+Alt+Shift+B` | 过滤框 + 名称/类型/行列表，回车或双击跳转 |
| 工作区符号 | `Ctrl+Alt+Shift+I` | 首次以光标词查询，之后本地过滤 |
| 实时诊断 | 编辑时自动 | 语法/语义/风格问题以标记点 + 波浪线显示 |
| 格式化 | `Ctrl+Alt+M` | 服务端 `textDocument/formatting`，逆序应用编辑 |
| 语义高亮 | 自动 | 用 `semanticTokens` 重新着色标识符、函数、汇编、段标记等 |
| 内联提示 | 默认关闭 | `CeLspInlayHints=1` 时以行注解显示参数/类型提示 |
| 代码透镜 | 默认关闭 | `CeLspCodeLens=1` 时以行注解显示标题 |
| 文档链接 | `Ctrl+双击` 链接目标 | 打开链接指向的文件 |
| 文档高亮 | 光标停留约 300 ms | 高亮当前符号在文档中的其他出现位置 |

### 7.3 诊断展示规则

- 服务端 `publishDiagnostics` 与本地 `CeLint` 共用同一套渲染器：彩色波浪线 + 行尾简短说明
- **仅 severity 1（错误）和 2（警告）绘制装饰**；severity 3/4 的信息与提示不画波浪线
- 本地语法检查结果与服务端诊断互不覆盖
- 开关：`CeLspDocDiagnostics`（默认开）

> 设计取舍：`inlayHint` 与 `codeLens` 默认关闭，避免代码下方出现大量额外描述行；需要时再在 INI 中开启。

---

## 8. 快捷键速查

### 8.1 CE 专用快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+Alt+K` | 检查脚本 |
| `Ctrl+Alt+M` | 格式化文档 |
| `Ctrl+Alt+G` | 生成配套代码 |
| `Ctrl+Alt+N` | 导入 CE 表格 |
| `Ctrl+Alt+Z` | 导出 CE 表格 |
| `Ctrl+Alt+D` | 打开 Cheat Engine Reference 面板 |
| `Ctrl+Alt+Shift+S` | CE 设置 |
| `Ctrl+Alt+Shift+T` | 代码模板管理 |
| `Ctrl+Alt+Shift+H` | 完整说明窗格（开关） |
| `Ctrl+Alt+Shift+F` | 跳转到定义 |
| `Ctrl+Alt+Shift+V` | 跳转到声明 |
| `Ctrl+Alt+Shift+U` | 查找所有引用 |
| `Ctrl+Alt+Shift+B` | 文档符号 |
| `Ctrl+Alt+Shift+I` | 工作区符号 |
| `Ctrl+Alt+Shift+R` | 运行脚本（自动路由，含 AA 启用/禁用） |
| `Ctrl+Alt+Shift+L` | 仅本地运行 Lua |
| —（CE 菜单） | 取消执行 / 停止刷新（停止监视并回滚已启用的 AA） |
| —（CE 菜单） | 一键消除监控指令（删除选区/全文的 np3watch 注释行，可撤销） |
| `Ctrl+Alt+Shift+O` | CE 输出面板（开关） |
| `F1` | 补全列表中打开该关键词的本地文档；否则打开在线帮助 |

### 8.2 常用编辑快捷键

| 快捷键 | 功能 |
| --- | --- |
| `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `F6` | 新建 / 打开 / 保存 / 另存为 |
| `Ctrl+F` / `Ctrl+H` / `F3` / `Shift+F3` | 查找 / 替换 / 查找下一个 / 上一个 |
| `Ctrl+G` | 转到行 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Ctrl+Alt+Enter` | 补全单词 |
| `Ctrl+F7` | 打开当前 INI 配置 |
| `Ctrl+F12` / `F12` | 自定义语法方案 / 选择语法方案 |
| `Ctrl+Shift+F` | grepWin 跨文件搜索 |
| `Ctrl+M` | MiniPath 文件浏览器 |
| `Ctrl+滚轮` | 缩放（10%–1000%） |
| `Alt+拖拽` | 矩形/列选择 |

> 完整快捷键表见 `readme/KeyboardShortcuts.md`。

---

## 9. 配置参考

### 9.1 CE 设置对话框（`Ctrl+Alt+Shift+S`）

包含：

- 允许从剪贴板读取已定义变量
- 函数补全时自动填入变量
- 输入 `alloc(...)` 后回车自动生成配套代码
- 保存时自动格式化并缩进
- 补全时包含当前文档已定义符号（支持局部匹配）
- 汇编代码中键入或补全自定义变量名时自动包裹 `[]`
- 高亮用户自定义变量名与 `label` 标签名
- 自动填参函数列表（逗号分隔）
- 缩进宽度（只读，跟随程序设置）
- 外部词典路径与“重载词典”
- 代码模板列表：新增 / 编辑 / 删除 / 插入选中模板 / 恢复默认

### 9.2 `[Settings2]` 中的 CE 选项

| 键 | 默认值 | 说明 |
| --- | --- | --- |
| `CeClipboardVariable` | `1` | 允许从剪贴板读取已定义变量 |
| `CeAutoArguments` | `1` | 函数补全自动填入变量 |
| `CeAutoPairCode` | `1` | `alloc(...)` 回车后自动生成配套代码 |
| `CeAutoFormatOnSave` | `1` | 保存时自动格式化 |
| `CeDynamicCompletions` | `1` | 补全包含文档内符号 |
| `CeUseLsp` | `1` | 启用内置语言服务器 |
| `CeLspFallback` | `1` | 服务器不可用时回退本地补全 |
| `CeLspServerPath` | 空 | 服务器路径，空 = 程序目录 `CeLspServer.exe` |
| `CeLspDebounceMs` | `120` | `didChange` 防抖（20–1000 ms） |
| `CeLspSemanticTokens` | `1` | 启用语义高亮 |
| `CeLspInlayHints` | `0` | 内联提示（默认关） |
| `CeLspCodeLens` | `0` | 代码透镜（默认关） |
| `CeLspDocDiagnostics` | `1` | 显示服务端诊断 |
| `CeWrapAsmSymbols` | `1` | 汇编变量名自动包裹 `[]` |
| `CeHighlightUserSymbols` | `1` | 高亮用户符号 |
| `CeLintSemantic` | `1` | 启用语义检查 |
| `CeDictionaryPath` | 空 | 外部词典，空 = 程序目录 `CeDictionary.txt` |
| `CeInstallPath` | 空 | Cheat Engine 安装目录，空 = 自动探测 |
| `CeAutoArgFunctions` | `registersymbol, unregistersymbol, dealloc, label` | 自动填参函数列表 |
| `CeOutputFontSize` | `11` | 输出面板字号（8–30） |
| `CePanelHeight` | `0` | 输出面板高度（像素，0 = 自动） |
| `CeTemplates` | 空 | 代码模板序列化数据 |

> 与 Notepad3 上游设置一样，大多数 `[Settings2]` 项修改后需要重启；CE 相关选项在设置对话框确认后即时生效。

### 9.3 外部词典格式

`CeDictionary.txt` 每行：

```
名称|签名|分类|说明
```

- UTF-8 编码
- 空行与无法解析的行会被忽略
- 修改后在 CE 设置中点“重载词典”，无需重启

---

## 10. 命令行用法

所有开关大小写不敏感，可用 `/` 或 `-` 前缀：

```bat
Notepad3.exe /utf8sig /crlf d:\temp\Test.txt     :: UTF-8 BOM + CRLF 打开
Notepad3.exe /g 250,5 src\Notepad3.c             :: 跳到第 250 行第 5 列
Notepad3.exe /m TODO main.c                      :: 打开并查找第一个 "TODO"
Notepad3.exe /b /i                               :: 剪贴板监视模式并最小化启动
Notepad3.exe /v report.log                       :: 静默打印后退出
Notepad3.exe /?                                  :: 内置帮助
```

常用类别：

- 文件与多文件：位置参数、`+`、`-`、`/y`、`/z`
- 编码：`/ansi`、`/unicode`、`/unicodebe`、`/utf8`、`/utf8sig`、`/e <名称>`
- 换行：`/crlf`、`/cr`、`/lf`
- 导航与搜索：`/g`、`/m`（支持正则/大小写/反斜杠修饰）
- 文件监视：`/l`、`/l0`、`/l1`
- 新文档：`/q`、`/qs`、`/c`
- 剪贴板收集：`/b`
- 词法方案：`/s`、`/d`、`/h`、`/x`
- 窗口：`/p`、`/t`、`/i`、`/o`
- 单实例/窗口复用：`/n`、`/ns`、`/r`、`/rs`、`/rp`
- 打印：`/v`、`/vd`
- INI 与提权：`/f`、`/f0`、`/u`

完整说明见 `readme/cmdln/CmdLnOptions.md`。

---

## 11. 常见问题与排错

| 现象 | 原因与处理 |
| --- | --- |
| 键入后没有补全列表 | 确认当前文档使用 Cheat Engine Script 方案；确认 `CeUseLsp=1` 或 `CeLspFallback=1`；检查输出面板是否有服务器启动失败日志 |
| 补全列表里全是 Lua API，汇编指令排后 | 光标不在 AA 段，或文档未被识别为 AA 区域；确认 `[ENABLE]` 段与汇编行格式正确 |
| 补全没有 `CeLspServer.exe` | 确认可执行文件与服务器同目录；或用 `CeLspServerPath` 指定绝对路径 |
| 大文件没有 LSP 功能 | 文档超过 1 MB 会自动回退本地补全，属预期行为 |
| 代码下方没有内联提示/透镜 | `CeLspInlayHints`、`CeLspCodeLens` 默认关闭，需要时在 INI 中设为 `1` |
| 波浪线太多，包含风格建议 | 检查是否只有错误/警告；信息级诊断不会绘制。如为本地检查，可关闭 `CeLintSemantic` |
| 保存后代码被重排 | `CeAutoFormatOnSave` 默认开启；不需要时在 CE 设置中关闭 |
| “仅本地运行 Lua”提示缺少运行时 | 把 `lua53-64.dll` 放到程序目录，或把程序放进 CE 目录，或设置 `CeInstallPath` |
| CE 桥接连接失败 | 确认 CE 已启动且 `autorun\np3_ce_bridge.lua` 已部署，部署后需重启 CE；检查 `CeInstallPath` |
| 运行 AA 提示 `no target process attached in Cheat Engine` | 先在 Cheat Engine 中附加目标进程，再重新运行脚本 |
| 第二次运行 AA 提示 `no [DISABLE] section; cannot disable` | 无段 AA 脚本只允许启用一次；请补充 `[DISABLE]` 段，或重启 CE 后在 CE 中手动还原再重新启用 |
| 运行 AA 提示必须各包含一个 `[ENABLE]` / `[DISABLE]` | 删除重复段或补齐缺失段；带段脚本要求两者各恰好一个 |
| AA 执行后没有 `[WATCH]` 变量输出 | 自动监视只收集 `alloc` / `define` / `registersymbol` / `aobscan*` 等声明的数据符号（最多 24 个，跳过 `newmem` 和 `label()`）；需要监控其它对象时用 `// np3watch <名称> [类型] [进制] [数量]` 手动声明；符号尚未注册时会周期性输出 `waiting` |
| 重启 CE 后再运行同一 AA 导致重复写入 | `disableInfo` 与监视状态只保存在 CE 进程内；重启后应先在 CE 中手动还原，再重新启用 |
| CE 菜单没有“取消执行 / 停止刷新” | 程序启动时会为缺少该项的语言 DLL 自动追加；若该语言资源完全没有 CE 菜单，请切换界面语言或更新语言 DLL |
| 脚本运行 30 秒后被终止 | 本地 Lua 与桥接都有执行超时；拆分为短脚本或减少阻塞等待 |
| `.lua` 打开后变成混合高亮 | `.lua` 扩展名映射到 CE 方案，可手动切换为普通 Lua 方案 |
| 配置改了不生效 | 多数 `[Settings2]` 需重启；CE 对话框中的选项即时生效 |

---

## 12. 附录：文档与源码索引

| 主题 | 位置 |
| --- | --- |
| 内置完整关键词参考 | `本地文档.txt`（帮助菜单 → 本地文档；补全列表按 `F1` 定位） |
| 菜单逐项说明 | `readme/MenuEntriesAndCmds.md` |
| 快捷键全集 | `readme/KeyboardShortcuts.md` |
| 配置参考 | `readme/config/Configuration.md`、`Build/Notepad3.ini` |
| 命令行参考 | `readme/cmdln/CmdLnOptions.md` |
| 文件内容标记（`.LOG`、Emacs 变量等） | `readme/config/FileContentFlags.md` |
| 自定义语法方案/主题 | `readme/schema/CustomSchema.md` |
| 加密功能 | `readme/encryption/Encryption.md` |
| 编码检测 | `readme/uchardet/EncodingDetection.md` |
| CE 编辑器源码 | `src/Ce*.c` / `src/Ce*.h` |
| CE 语言服务器源码 | `ce_lsp_server/CeLspServer.c` |
| CE 桥接脚本 | `ce_bridge/np3_ce_bridge.lua` |
| 补全数据生成 | `tools/gen_ce_keywords.py`、`tools/gen_ce_api_types.py` |
| 独立 Python 版 CE 编辑器 | `ce_edit/README.md` |

---

*本文档描述的是当前工作区源码所实现的程序行为；如与实际构建产物不一致，以源码与程序内《本地文档》为准。*
