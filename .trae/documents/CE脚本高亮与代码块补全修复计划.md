# CE 脚本高亮渲染修复 + `{$lua}`/`{$asm}` 补全

## Context（为什么改）

用户报告两个问题：

1. **输入字母后高亮错乱**：直接打开 AA 脚本时语法高亮正确；一旦在编辑器里输入字母，`mono_initialize()` / `LaunchMonoDataCollector()` 等 CE API 从绿色（`SCE_CE_WORD2`）变成紫色（`SCE_CE_WORD8`），并且 `[ENABLE]` 行首出现红色波浪线（零偏移误报诊断）。
2. **智能提示缺少表脚本标记**：补全列表里没有 `{$lua}`、`{$asm}`，也没有 `{$lua}` … `{$asm}` 的成对代码块。

已在代码中定位到四个独立根因：

| 根因 | 位置 |
|---|---|
| LSP semanticTokens 解码违反规范：换行后的 `deltaStart` 是绝对列号，代码却无条件累加，逐行累积漂移（紫色/绿色边界错位） | `src/CeLspFeatures.c` 语义令牌解码循环 |
| 服务端把 `_FindEntry()` 命中的 CE API 统一标为 `"function"`（紫），覆盖词法层的 API 绿色 | `ce_lsp_server/CeLspServer.c` `_TokenizeLine` |
| `{$lua}`/`{$asm}` 是块**分隔符**（`{$lua}` 开、`{$asm}` 关），不是成对标签；服务端按奇偶计数判定 Unmatched，并在 offset 0 报 error | `ce_lsp_server/CeLspServer.c` `_RunDiagnostics` |
| semanticTokens / 诊断的 region 用 `(region==2)?1:2` 之类反向切换，`{$asm}` 无法正确回到 AA 区 | `ce_lsp_server/CeLspServer.c` `_HandleSemanticTokensFull` |

补全侧根因：`{$lua}`/`{$asm}` 本就在 `completions.json`（`表脚本标记`, kind 8）里，但

- 本地路径用 `_IsIdentifierLike()` 过滤，非标识符名称被丢弃；
- 客户端/服务端的词起点扫描只接受标识符字符，前缀拿不到 `{$`，即使条目保留也无法命中；
- `s_codeBlocks` 里只有 `luaBlock`（名字是 `luaBlock`），输入 `{$` 时匹配不到。

## 已完成（本会话已落盘）

- `src/CeLspFeatures.c`
  - 语义令牌解码：`character = (deltaLine == 0) ? character + deltaStart : deltaStart;`
  - `_RootAtCaret()`：当前缀紧邻 `{$` 时把起点回退 2 个字符，选中标记时替换整个 `{$`。
- `src/CeLspServer.c`
  - `_RunDiagnostics()`：删除 `luaMarkers/asmMarkers/ccodeMarkers` 奇偶校验，改为 `blockOpen` 状态机，未闭合时才在真实偏移报错。
  - 语义令牌新增 `api` 类型（`CE_TOK_API`），`_EntryKindN()` 按 `CE_KIND_FUNCTION` 区分：API → `api`（保持绿色），其它目录条目 → `function`。
  - `_HandleSemanticTokensFull()` 的 region 改为直接赋值：`{$lua}`→1、`{$asm}`→2、`{$ccode}`/`{$CODE}`→3。
  - `_HandleCompletion()`：`wordStart` 在 `{$` 前回退 2，prefix 与 `memberPrefixStart` 同步使用扩展后的起点。
- `src/CeCompletion.c`
  - 新增 `_IsSectionMarkerName()` / `_IsCompletableName()`；`CeCompletion_CreateWordList()` 主目录循环改用 `_IsCompletableName()`，`{$...}` 条目不再被过滤。

## 待实施

### 1. `src/Edit.c` — 本地补全路径的 `{$` 起点回退

`EditAutoCompleteWord()`（约 8704–8787 行）中 `iWordStartPos` 目前是 `DocPos const`，且 `SciCall_AutoCShow(iRootLen, …)` 的 `lenEntered` 只覆盖 `$l`，会在选中 `{$lua}` 后残留一个 `{`（得到 `{${$lua}`）。

改为可变量并在 `bCeScheme` 下回退：

```c
DocPos iWordStartPos = SciCall_WordStartPosition(iPosBefore, true);
// CE section markers ({$lua}/{$asm}) are not identifier shaped, so Scintilla's
// word start stops right after "{$".  Include the opener so the accepted
// marker replaces it instead of leaving a stray "{" behind.
if (bCeScheme && (iWordStartPos >= 2) &&
    (SciCall_GetCharAt(iWordStartPos - 1) == '$') &&
    (SciCall_GetCharAt(iWordStartPos - 2) == '{')) {
    iWordStartPos -= 2;
}
```

后续 `iWordLen` / `iRootLen` / `pRoot` 都基于扩展后的起点，无需再改。仅在字面前缀恰为 `{$` 时触发，不影响 Lua 表构造 `{`。

### 2. `src/CeCompletion.c` — 新增 `{$lua} … {$asm}` 代码块

在 `s_codeBlocks[]`（约 775–800 行）末尾追加一条，名字以 `{$` 开头，才能被 `{$` 前缀命中，且 `_TryInsertCodeBlock()` 按 `caret - strlen(name)` 精确替换，`{`/`$`/`}` 不构成障碍（AutoC 只用空格与 `?` 作分隔符）：

```c
{ { "{$lua}Block", "{$lua} ... {$asm} 成对代码块", "AA 代码块", 0, CE_KIND_AA },
  "{$lua}\n$0\n{$asm}\n", CE_BLOCK_AA },
```

注意：

- `_CandidateMatches()` 为子串匹配，输入 `{$l` 时 `{$lua}Block` 与 `{$lua}` 都会出现，前缀命中得 320 分，排序在前。
- 实施时先确认 `_CodeBlocksAllowed()` 在行首 `{$` 场景返回 true（若它要求"语句可起始"位置，行首应满足；若它排除 `{`，需只对该新条目放开）。
- 存量 `luaBlock` 保留（输入 `lua` 仍可插入），两者不冲突。

### 3. 目录数据 — 修正 `{$lua}` / `{$asm}` 的描述

`ce_edit/ce_edit/data/completions.json` 中的签名错误地把结束标记写成自身：

- `{$lua}`：`"{$lua} ... {$lua} -- 在 AA 中嵌入 Lua"` → `"{$lua} ... {$asm} -- 在 AA 中嵌入 Lua"`
- `{$asm}`：`"{$asm} ... {$asm} -- 在 Lua 中嵌入汇编"` → `"{$asm} -- 切回 Auto Assembler 汇编块"`

`keyword_docs.json` 中两者已有完整中文文档（`功能/语法/示例/约束/相关/易错`），核对后如需再补一条"成对代码块"说明再加。改完执行：

```powershell
python tools/gen_ce_keywords.py
```

重新生成 `src/StyleLexers/ce_completions_data.c/.h`（不要手改生成文件）。

### 4. 编译与测试

```powershell
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -requires Microsoft.Component.MSBuild `
    -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1

& $msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
pwsh -NoProfile -File test\TestCeBridge.ps1
```

要求：无新增警告/错误（`/WX` 下未引用函数视为错误，改 `_EntryN`→`_EntryKindN` 时已删除旧函数）。

## 验证

1. **构建**：ClCompile 通过 + Release x64 全量链接通过。
2. **回归**：`test\TestCeBridge.ps1` 全绿（AA 状态标记 `return '1'`/`'0'` 断言仍成立）。
3. **手工联调（需真实 CE + LSP，无法在 CI 覆盖）**：
   - 打开含 `{$lua}` / `mono_initialize()` / `LaunchMonoDataCollector()` 的脚本：CE API 保持绿色，`[ENABLE]` 行首无红色波浪线（打开的瞬间即正确）。
   - 在任意位置输入字母后：高亮不错位、API 仍为绿色（语义令牌不再漂移、不再覆盖为紫色）。
   - 输入 `{$`：列表出现 `{$lua}`、`{$asm}`、`{$ccode}` 等与 `{$lua}Block`；选中 `{$lua}` 后文本为 `{$lua}`（无残留 `{`）；选中 `{$lua}Block` 后插入 `{$lua}\n\n{$asm}` 且光标停在中间空行。
   - 输入 `lua` 选 `luaBlock` 仍正常。
4. **未验证项**：真实 Cheat Engine 进程下的 LSP 语义令牌、诊断与补全呈现只能手工确认；本次仅能声明编译与自动化测试结果。
