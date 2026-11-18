# CE LSP 客户端功能增强实施计划

## Context

用户要求完善 Notepad3 CE 分支内置 LSP 的客户端功能：跳转到定义/声明、查找所有引用、文档符号、工作区符号搜索、实时诊断（语法/语义/风格）、格式化、语义高亮、内联提示、代码透镜、文档链接、文档高亮，并借服务端 publishDiagnostics 与 semanticTokens 增强 Lua / CE Auto Assembler / 汇编的支持。

- **服务端** `ce_lsp_server/CeLspServer.c` 正由后台子代理实现（agentId 0adba087-0eb0-45de-9fd1-75a12def86de），本计划**不触碰**该文件。
- 本计划覆盖客户端（`src\CeLspClient.[ch]`）、UI 适配层（`src\CeLspFeatures.[ch]`）、配置、命令/菜单/加速键与 `Notepad3.c` 接线。
- 协议契约（已与服务端子代理约定，标准 LSP）：definition/declaration/references → `Location[]`；documentSymbol → `DocumentSymbol[]`；workspace/symbol → `SymbolInformation[]`；formatting → `TextEdit[]`；semanticTokens/full → `{data:int[]}` delta 5 元组 + initialize 中 `semanticTokensProvider.legend.tokenTypes`；inlayHint/codeLens/documentLink/documentHighlight → 标准数组；诊断走 `textDocument/publishDiagnostics` 通知。

## 已确认的架构事实（探索结论）

- 客户端 [CeLspClient.c](file:///d:/code-project/c/Notepad3-master/src/CeLspClient.c)：`_SendRequest`(L492)/`_Position`(L~200)/`_FlushChanges`(L393)/`_AddPending` 可复用；`OnMessage`(L933) 按 kind 分发、版本不符丢弃；INIT case(L995) 仅解析 `positionEncoding`。`CeLspClient_OnSyncTimer`(L777) 调 `_FlushChanges`。
- [CeLspPos.h](file:///d:/code-project/c/Notepad3-master/src/CeLspPos.h)：`CeLspPos_ToOffset(text,len,line,char,utf16)` 做 LSP→字节偏移。
- [CeLspFeatures.c](file:///d:/code-project/c/Notepad3-master/src/CeLspFeatures.c)：`_Log`→`CePanel_Log`；`OnDiagnostics`(L667) 当前为空实现（注释说明由本地 CeLint 渲染）——本计划改为渲染服务端诊断（markers+indicator，与 CeLint 的 EOLAnnotation 不冲突）。
- SciCall.h 已有封装：`AddSelection`(L375)/`SetMainSelection`(L377)/`SetStyling`(L815)/`StartStyling`(L816)/`StyleSetFore`(L811)/`MarkerDefine`(L965)/`IndicSetStyle`(L995)/`IndicSetFore`(L997)/`SetIndicatorCurrent`(L1013)/`IndicatorFillRange`(L1017)/`BeginUndoAction`(L1068)/`SetTargetRange`(L586)/`ReplaceTarget`(L587)/`AnnotationSetText`(L1106)/`AnnotationSetStyle`(L1108)/`AnnotationClearAll`(L1113)/`AnnotationSetVisible`(L1114)。缺 `SetClickNotifies`（可能缺 `MarkerSet`）→ 按仓库规则在 SciCall.h 补封装。
- 占用情况：indicator 枚举 `INDIC_NP3_CE_*` 到 [TypeDefs.h](file:///d:/code-project/c/Notepad3-master/src/TypeDefs.h#L423-L442) `INDIC_NP3_ID_CNT`；marker 0-9 被占用（`MARKER_NP3_*`），10-20 空闲；EOLAnnotation 归 CeDiagnostics/CeLint，普通 Annotation 无人使用（grep 无 `SciCall_AnnotationSetText` 调用点）。
- CE 样式常量：`SCE_CE_*`（0-25）定义于 [SciXLexer.h](file:///d:/code-project/c/Notepad3-master/lexilla/lexers_x/SciXLexer.h#L109-L134)。
- 命令接线：[Notepad3.c](file:///d:/code-project/c/Notepad3-master/src/Notepad3.c) 命令 switch L7404-7494（`IDM_CE_DOC_QUERY` L7484）；`WM_CELSP_MESSAGE/STATUS` L2778-2784；`WM_TIMER` 的 `IDT_TIMER_CELSP_SYNC` 分支 L2786-2795；`SCN_UPDATEUI` ~L10578；`SCN_DWELLSTART` ~L10665；`SCN_CLICKNOTIFYS` 全仓库未处理。CE 命令无按 lexer 使能门控（IDM_CE_CHECK 仅一处）。
- [resource.h](file:///d:/code-project/c/Notepad3-master/src/resource.h#L88)：`IDT_TIMER_CELSP_SYNC=243`，新定时器用 244。
- [common_res.h](file:///d:/code-project/c/Notepad3-master/language/common_res.h#L904)：IDM_CE_* 最大已用 41124，新命令从 41125 起。
- 菜单：仅 en_us/zh_cn 有 CE POPUP（两文件均 L463-483）。加速键 Ctrl+Alt+字母已全部占用；Ctrl+Alt+Shift 已占用 D,S,T,R,L,O → 新命令用 **F/V/U/B/I**（已确认空闲）。
- 符号列表交互参考 [CeReference.c](file:///d:/code-project/c/Notepad3-master/src/CeReference.c)（ListView+过滤 edit+EN_CHANGE 150ms 防抖），但不新建对话框模板资源——用 CreateWindowExW 手工创建非模态窗口，避免动所有语言的 rc。
- Settings2 CE 区：[TypeDefs.h L917-933](file:///d:/code-project/c/Notepad3-master/src/TypeDefs.h#L917-L933)；Config.cpp 读 L2075-2082、写 L2179-2182。

## 修改内容

### 1. src\CeLspClient.h
- `CELSP_REQ_KIND` 新增（插在 HOVER 与 SHUTDOWN 之间）：DEFINITION, DECLARATION, REFERENCES, DOCSYMBOLS, WSSYMBOLS, FORMATTING, SEMTOKENS, INLAY, CODELENS, DOCLINKS, DOCHIGHLIGHT。
- 新增能力位枚举 `CELSP_CAP_*`（DEFINITION/DECLARATION/REFERENCES/DOCSYMBOLS/WSSYMBOLS/FORMATTING/SEMTOKENS/INLAY/CODELENS/DOCLINKS/DOCHIGHLIGHT）与 `bool CeLspClient_HasCap(int cap)`。
- 新增 API：
  - `bool CeLspClient_PosToOffset(int line, int character, DocPos* out)`（锁内用 s_text/s_textLen/s_utf16 调 CeLspPos_ToOffset；无快照返回 false）
  - `bool CeLspClient_RequestPositional(const char* method, int kind)`（仿 RequestHover）
  - `bool CeLspClient_RequestReferences(void)`（context.includeDeclaration=true）
  - `bool CeLspClient_RequestWorkspaceSymbols(const char* query)`
  - `bool CeLspClient_RequestFormatting(void)`
  - `int CeLspClient_TokenTypeCount(void)` / `const char* CeLspClient_TokenType(int idx)`（semanticTokens legend）

### 2. src\CeLspClient.c
- 新增 `static unsigned s_caps`、`static char s_tokenTypes[32][32]`、`static int s_tokenTypeCount`。
- INIT case：解析 initialize result 的 11 个能力位（`semanticTokensProvider` 为对象即视为支持，并读取 `legend.tokenTypes` 字符串数组）。
- 实现上述新请求 API（均先 `_FlushChanges()`，位置类用 `_Position`）。
- `OnMessage` switch 新增分发：DEFINITION/DECLARATION/REFERENCES→`CeLspFeatures_OnLocations(result, kind)`；DOCSYMBOLS→`OnDocumentSymbols`；WSSYMBOLS→`OnWorkspaceSymbols`；FORMATTING→`OnFormatting`；SEMTOKENS→`OnSemanticTokens`；INLAY→`OnInlayHints`；CODELENS→`OnCodeLens`；DOCLINKS→`OnDocumentLinks`；DOCHIGHLIGHT→`OnDocumentHighlight`。
- `CeLspClient_OnSyncTimer` 在 `_FlushChanges()` 后调用 `CeLspFeatures_RefreshDecorations()`（重取 semanticTokens/inlayHint/codeLens/documentLink）。

### 3. src\CeLspFeatures.h / src\CeLspFeatures.c（核心）
新公共入口（Notepad3.c 调用）：`CeLspFeatures_GotoDefinition/GotoDeclaration/FindReferences/ShowDocumentSymbols/ShowWorkspaceSymbols/FormatDocument/RefreshDecorations/OnClickNotify(DocPos pos, int modifiers)`。

- **导航**：`OnLocations` 解析 `Location[]`（或 `LocationLink`，取 range/selectionRange 首值）；definition/declaration 取第一个结果 `PosToOffset`→`SciCall_GotoPos+SetSel` 并高亮词；无结果 `_Log` 提示。references 用 `SciCall_AddSelection` 多选 + `SetMainSelection(0)`，计数写 CePanel。
- **符号列表**：`OnDocumentSymbols`/`OnWorkspaceSymbols` 缓存 name/kind/line/character 数组，弹出非模态窗口（CreateWindowExW：顶部 edit 过滤 + ListView 名称/类型/行；Enter/双击→`PosToOffset` 跳转并关窗；参考 CeReference.c 的过滤与 WM_SETREDRAW 模式）。`ShowWorkspaceSymbols` 打开空过滤框，输入即本地过滤已缓存结果；首次由命令发 `workspace/symbol`（query=当前光标词）。
- **格式化**：`FormatDocument`：active+cap→`RequestFormatting`；否则回落 `CeFormat_Document()`。`OnFormatting` 将 TextEdit[] 按起始偏移**逆序**应用（BeginUndoAction/SetTargetRange/ReplaceTarget/EndUndoAction），完成后 `CeLspClient_OnDocumentChanged()`。
- **语义高亮**：`OnSemanticTokens` 解码 delta 5 元组（deltaLine/deltaStart/length/tokenType/tokenMods），逐 token 用 `PosToOffset` 求字节起止（下一 token 起点即当前终点，末 token 到文档尾），`StartStyling+SetStyling` 写入样式；类型名→`SCE_CE_*` 映射：keyword→WORD5? 不——keyword→SCE_CE_WORD、function→SCE_CE_WORD8、variable/parameter→SCE_CE_IDENTIFIER、string→SCE_CE_STRING、number→SCE_CE_NUMBER、comment→SCE_CE_COMMENT、operator→SCE_CE_OPERATOR、macro→SCE_CE_PREPROCESSOR、label→SCE_CE_LABEL、type→SCE_CE_WORD5、assembly→SCE_CE_WORD4、aa→SCE_CE_WORD3、section→SCE_CE_WORD6、module→SCE_CE_MODULE；未知类型跳过。受 `Settings2.CeLspSemanticTokens` 控制。
- **inlayHint + codeLens**：各缓存一份行数组；`_RenderAnnotations()` 合并为每行一条 top annotation（codeLens 文本 + ` ; ` + hints），`AnnotationSetText` + `AnnotationSetVisible(SC_ANNOTATION_BOXED)`，`StyleSetFore/Back(STYLE_ANNOTATION)` 取柔和色；开关关闭或无数据时 `AnnotationClearAll`。受 `CeLspInlayHints`/`CeLspCodeLens` 控制。
- **documentLink**：缓存 range+target；`OnClickNotify` 在 Ctrl+单击时命中则打开目标文件（复用 Notepad3.c 现有文件打开路径，实现时确认函数名，如 `FileLoad`；非当前文件则提示）。`OnDocumentOpened` 中 `SciCall_SetClickNotifies(true)`。
- **documentHighlight**：新 indicator `INDIC_NP3_LSP_HILITE`（ROUNDBOX 半透明）；`OnUpdateUI` 检测光标位置变化→`SetTimer(Globals.hwndMain, IDT_TIMER_CELSP_HILITE, 300, NULL)`；到期请求；`OnDocumentHighlight` 先 `IndicatorClearRange` 全文再逐 range 填充；kind 1/2/3 用同色。
- **服务端诊断**：`OnDiagnostics` 改为实际渲染（受 `Settings2.CeLspDocDiagnostics`）：severity 1/2 → 本地 `#define MARKER_LSP_ERROR 10 / MARKER_LSP_WARNING 11`（10-20 空闲，避开 `MARKER_NP3_*` 0-9 与历史/折叠 21+）`MarkerDefine(SC_MARK_CIRCLE/SC_MARK_ARROW)`+`MarkerSetFore`+`MarkerAdd`；同时新 indicator `INDIC_NP3_LSP_DIAG` squiggle 下划线；先 `MarkerDeleteHandle` 旧句柄（保存 handle 数组）再重画；与 CeLint（EOLAnnotation）/CeDiagnostics（INDIC_NP3_CE_DIAG_*）零冲突。
- **生命周期**：`OnDocumentOpened` 清 decorations/links/diagnostics + `RefreshDecorations`；`OnDocumentClosed`/`OnServerStatus(EXITED)` 同样清理；`RefreshDecorations` 内部按 cap+开关发 4 类请求。

### 4. src\SciCall.h
- 补 `DeclareSciCallV1(SetClickNotifies, SETCLICKNOTIFIES, bool, clickNotifies);`；若 `MarkerSet` 无封装则用 `MarkerAdd/MarkerDeleteAll` 或补声明。

### 5. src\TypeDefs.h
- `INDIC_ID` 在 `INDIC_NP3_ID_CNT` 前插入 `INDIC_NP3_LSP_HILITE`、`INDIC_NP3_LSP_DIAG`（追加式，不移动现有值）。
- Settings2 CE 区新增 4 个 bool：`CeLspSemanticTokens`、`CeLspInlayHints`、`CeLspCodeLens`、`CeLspDocDiagnostics`。

### 6. src\Config\Config.cpp
- 读（L2082 后）/写（L2182 后）按现有模式加 4 开关，默认 true/true/false/true。

### 7. src\resource.h
- `#define IDT_TIMER_CELSP_HILITE 244`。

### 8. language\common_res.h
- 41125-41129：`IDM_CE_GOTO_DEFINITION / IDM_CE_GOTO_DECLARATION / IDM_CE_FIND_REFERENCES / IDM_CE_DOC_SYMBOLS / IDM_CE_WS_SYMBOLS`。

### 9. 菜单与加速键
- `language\np3_en_us\menu_en_us.rc` 与 `np3_zh_cn\menu_zh_cn.rc` 的 CE POPUP（L463-483）：在 `IDM_CE_FORMAT` 后加分隔 + 5 项（英文/中文文本，带 `\tCtrl+Alt+Shift+X` 提示）。
- `src\Notepad3.rc` 加速表 CE 区（L297 后）加 5 条 `VIRTKEY, SHIFT, CONTROL, ALT, NOINVERT`：F=定义、V=声明、U=引用、B=文档符号、I=工作区符号。

### 10. src\Notepad3.c
- 命令 switch（L7486 `IDM_CE_DOC_QUERY` 后）加 5 个 case 调 `CeLspFeatures_*`；`IDM_CE_FORMAT` case 改为 `CeLspFeatures_FormatDocument(hwnd)`（内部回落 `CeFormat_Document`，保持原声明可见）。
- WM_TIMER（L2791 后）加 `IDT_TIMER_CELSP_HILITE` 分支：KillTimer + `CeLspFeatures_OnHighlightTimer()`。
- fnScintilla 加 `case SCN_CLICKNOTIFYS:`（仿 SCN_DWELLSTART 模式）→ `CeLspFeatures_OnClickNotify(pos, modifiers)`。

## 语言检查增强说明

Lua/AA/asm 的语法错误、语义问题、风格警告由服务端诊断实现（后台子代理负责）；客户端以 markers+squiggle 实时呈现，并以 semanticTokens 的 assembly/aa/section 等 token 类型驱动 AA/asm 着色。

## 执行顺序

1. SciCall.h / TypeDefs.h / resource.h / common_res.h（基础定义）
2. CeLspClient.h → CeLspClient.c
3. CeLspFeatures.h → CeLspFeatures.c（最大块）
4. Config.cpp、两个 menu rc、Notepad3.rc、Notepad3.c
5. 构建验证

## 验证

- 后台子代理完成后其自行编译服务端；等待其通知。
- `& $msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`（编译期错误清零）
- 全量链接 `& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64`
- GUI 手工功能验证需桌面环境；无法运行时明确报告"未运行"。
- 不做 git 提交；不改 `ce_lsp_server\CeLspServer.c`；不动无关代码。

## 风险与对策

- 服务端能力名/结构与约定不符 → 客户端全部按 `HasCap` 门控，缺能力仅记日志不崩溃。
- SetStyling 与词法器竞争：文本改动后 lexer 重涂，装饰在 `_FlushChanges` 后统一重取，最终一致。
- 普通 Annotation 若日后被其他模块占用：当前 grep 确认无人使用，风险低。
- 加速键冲突：F/V/U/B/I 在 SHIFT+CONTROL+ALT 表已 grep 确认空闲。
