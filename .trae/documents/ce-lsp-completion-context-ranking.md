# CE 补全列表按语言上下文加权排序（LSP 路径）

## Context

用户截图显示：光标位于 AA 脚本 `[ENABLE]` 段的汇编代码区，输入 `mo` 后补全列表前几项是 `mono_enumDomains` / `mono_enumAssemblies` / `mono_image_enumClasses` 等 CE Lua API，而 `mov` 等汇编指令排在很后面甚至不可见。

根因：默认 `Settings2.CeUseLsp == true`，补全列表走 LSP 路径 `CeLspFeatures_OnCompletion()`（[CeLspFeatures.c:577](file:///d:/code-project/c/Notepad3-master/src/CeLspFeatures.c)）。该函数按服务端返回顺序（即 `g_ceCompletions[]` 的目录顺序，`ce_lsp_server/CeLspServer.c:1225` 按表顺序遍历）直接拼接 `s_list`，**完全没有语言上下文权重**。而本地回退路径 `CeCompletion_CreateWordList()` 已有 `_ScoreCatalogue()` 的 AA/Lua/C 分组加权（[CeCompletion.c:1963](file:///d:/code-project/c/Notepad3-master/src/CeCompletion.c)），所以 LSP 路径绕过了既有排序逻辑。

目标：在 LSP 路径上复用同一套分组权重重新排序（AA 模式：汇编/AA 关键字 > 普通变量 > Lua API；Lua 模式反之；C 模式：C 关键字优先），同权重保持服务端原顺序；并补充单元测试。

## 变更一：`src/CeCompletion.c` — 导出可复用的上下文权重

1. 从 `_ScoreCatalogue()`（约 1963-1991 行）中抽出分组加分为独立函数 `_GroupBonus(CE_GROUP group, int mode)`，规则保持完全一致：
   - `group == want` → `+520`
   - `NEUTRAL` → `+40`
   - `AA` 与 `LUA` 互为期望 → `-260`
   - `mode == CE_LANG_AA && group == C` → `+90`（其余 0）
   `_ScoreCatalogue()` 改为 `score += _GroupBonus(group, mode);`，行为不变。

2. 在 `_ScoreCatalogue` 之后新增公开函数：
   ```c
   int CeCompletion_ContextRank(const char* pszName, int kind, int mode)
   ```
   - 先用 `CeCompletion_Find(pszName)`（已存在，973 行）取目录条目；取不到时用 `{ pszName, NULL, NULL, 0, kind }` 临时条目。
   - `_EntryGroup()` 定组，累加：`_GroupBonus()` + `s_commonCeNames` 命中 `+240` + `isCall` `+70`（与 `_ScoreCatalogue` 的附加项一致，不含 `_MatchBonus`/长度项——LSP 侧已按 prefix 过滤过）。
   - `mode` 为 `CE_LANG_COMMENT` 时按 Lua 处理（`_GroupBonus` 的三元链天然如此）。

3. 在 `_CurrentModeAtCaret()`（1562 行）之后新增：
   ```c
   int CeCompletion_CurrentMode(void) { (void)_EnsureDocSymbols(); return _CurrentModeAtCaret(); }
   ```
   `_EnsureDocSymbols()`（2102 行，1s 节流）内部会调用 `_RebuildLineModes()`（1714 行），与本地路径的时效性一致，避免每次补全都对全文档重扫。

## 变更二：`src/CeCompletion.h` — 声明

```c
int CeCompletion_CurrentMode(void);   // 光标处语言（CE_LANG_*）
int CeCompletion_ContextRank(const char* pszName, int kind, int mode);
```

## 变更三：`src/CeLspFeatures.c` — 解析后统一排序再建列表

1. `CELSP_ITEM` 增加 `int weight; int order;`（`_FindItem()` 按 label 查找，不受排序影响）。
2. 新增 `_CompareItem()`：`weight` 降序，`weight` 相同按 `order` 升序（`order` 使排序成为全序，无需稳定排序）。
3. 重构 `CeLspFeatures_OnCompletion()`（577-712 行）：
   - 解析阶段只填 `s_items`（`order` 记为 `i` 或 `limit + i`），**不再**在循环内 `_ListAdd`；extras 追加逻辑与去重保持不变。
   - 解析完成后一次性取 `int const mode = CeCompletion_CurrentMode();`，对每个 item 计算 `weight = CeCompletion_ContextRank(dst->label, dst->kind, mode);`。
   - `qsort(s_items, s_itemCount, sizeof(CELSP_ITEM), _CompareItem);`
   - 排序后再按现有格式（`" "` 分隔 + `"?<kindId>"` 后缀）重建 `s_list`，`s_listLen`/`s_list[0]` 重置逻辑保持；`SC_ORDER_CUSTOM` + `AutoCShow(&s_list[1])` 等保持不变。

预期效果（AA 模式，prefix `mo`）：`mov`（AA 组 +520、common +240）远高于 `mono_*`（FUNCTION → Lua 组 -260）；Lua 模式下顺序相反；C 模式下 `DWORD`/`int` 等 C 关键字（`s_cOnlyKeywords` → C 组 +520）优先，AA/Lua 均为 0 分保持服务端原序。

## 变更四：测试

`test/TestCeCompletion.ps1` + `test/TestCeCompletion.cpp`：

1. ps1 增加数组提取辅助函数（从 `static const char* const s_commonCeNames[] = {` / `s_cOnlyKeywords[] = {` 起截到 `};`），并在 `$parts` 中按依赖顺序加入：
   `s_commonCeNames`、`s_cOnlyKeywords`、`_NameInList`、`_EntryGroup`、`_GroupBonus`、`CeCompletion_ContextRank`。
2. TestCeCompletion.cpp 补 `CE_KIND_*` 与 `CE_LANG_*` 常量定义；新增用例：
   - AA 模式：`ContextRank("mov", CE_KIND_ASM, AA) > ContextRank("mono_enumDomains", CE_KIND_FUNCTION, AA)`
   - Lua 模式：`ContextRank("readInteger", CE_KIND_FUNCTION, LUA) > ContextRank("mov", CE_KIND_ASM, LUA)`
   - C 模式：`ContextRank("DWORD", CE_KIND_KEYWORD, C) > ContextRank("mov", CE_KIND_ASM, C)`

## 验证

```powershell
pwsh -NoProfile -File test\TestCeCompletion.ps1          # 新增排序用例
pwsh -NoProfile -File test\TestCeLintIntegration.ps1      # 既有 lint 回归
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
& $msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
```

手工验证（需运行 GUI，可能无法自动执行）：打开 CE 脚本模板，在 `[ENABLE]` 段输入 `mo` 确认 `mov` 排在最前；在 `{$lua}` 段输入 `mo` 确认 `mono_*` 在前。若无法运行 GUI，则在总结中说明该项未验证。

## 不涉及

- `ce_lsp_server/CeLspServer.c` 的服务端顺序不改（客户端重排覆盖服务端列表与合并进来的本地字典词）。
- 不改本地回退路径行为（仅等价重构 `_ScoreCatalogue`）。
