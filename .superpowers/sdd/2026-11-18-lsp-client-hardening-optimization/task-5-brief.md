### Task 5: 增量文档同步与服务端应用

**Files:**
- Modify: `src/CeLspClient.h:88-93`（新增 `CeLspClient_OnDocumentRangeChanged()`）
- Modify: `src/CeLspClient.c:461-532`（`_FlushChanges`）、`924-958`（生命周期）、`1-120`（范围数组静态状态）
- Modify: `src/Notepad3.c:10591-10628`（SCN_MODIFIED 钩子）
- Modify: `ce_lsp_server/CeLspServer.c:3421-3434`（`_HandleDidChange`）

**Interfaces:**
- Consumes: Task 1 的 `_Celsp_MergeChangeRange`、Task 4 的会话状态。
- Produces: `void CeLspClient_OnDocumentRangeChanged(DocPos position, DocPos length, int linesAdded, const char* text)`；服务端 `static bool _ApplyRangeChange(const char* text, int startLine, int startChar, int endLine, int endChar)`。

- [ ] **Step 1: 写失败测试**

新建 `test/TestCeLspEndToEnd.ps1`：向服务端发送完全相同的文档依次用全量 `didChange` 和增量 `didChange`（含中文、多行插入、删除），再发 `textDocument/hover` 或 `documentSymbol` 断言服务端表现一致。若服务端二进制不存在，脚本用源码编译的 harness 替代并标注 SKIP 真实服务器子项。

- [ ] **Step 2: 运行脚本确认失败**

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: FAIL，增量 didChange 未被服务端识别。

- [ ] **Step 3: 客户端累积变更范围**

`CeLspClient_OnDocumentRangeChanged()` 只在 `CeLspFeatures_IsEnabled()` 且 UTF-8 会话时累积；`SC_MOD_INSERTTEXT` 用 `[position,position+length)`，`SC_MOD_DELETETEXT` 用 `[position,position+length)`，text 为空；`linesAdded!=0` 不改变字节范围。范围超过 256 段或发生 didOpen/文档切换时设 `s_fullSync=true`。

- [ ] **Step 4: `_FlushChanges()` 生成 contentChanges**

`s_fullSync==true` 时保持现有全量文本；否则把累积范围转为 LSP `Position`（通过现有 `CeLspPos_OffsetToPos` 或等价接口），生成 `range/text` 对象；发送后清空范围数组和 `s_fullSync`。

- [ ] **Step 5: 服务端应用增量 change**

`_HandleDidChange()` 检测 `contentChanges[i]` 是否有 `range`：无 range 保持 `_SetText()`；有 range 时按 LSP position 转 UTF-8 字节偏移，替换 `g_docText` 区间，更新 `g_docLen`。只使用最后一个 change 的语义改为依次应用所有 changes。

- [ ] **Step 6: 不一致时降级**

服务端若检测到 range 越界/负长度，返回全量重同步请求不可行时，客户端在下一次 flush 发送全量文档；日志记录一次 `incremental sync desync, falling back to full sync`。

- [ ] **Step 7: 验证**

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: PASS，增量与全量最终文档一致。

