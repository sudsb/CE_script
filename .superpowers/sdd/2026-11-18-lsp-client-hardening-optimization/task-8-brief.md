### Task 8: 诊断列表与跳转

**Files:**
- Modify: `src/CeLspFeatures.h:42-57`（新增 `CeLspFeatures_ShowDiagnostics()`）
- Modify: `src/CeLspFeatures.c:936-971`（诊断缓存）、`1003-1425`（复用符号窗口模式）
- Modify: `language/common_res.h:899`（新增 `IDM_CE_LSP_DIAGNOSTICS=41069`）
- Modify: `language/np3_en_us/menu_en_us.rc:463-479`
- Modify: `language/np3_zh_cn/menu_zh_cn.rc:463-479`
- Modify: `src/Notepad3.c:7471-7580`（命令分发）、`2813-2826`（如需要）

**Interfaces:**
- Consumes: Task 6 的诊断缓存，Task 7 的设置。
- Produces: `void CeLspFeatures_ShowDiagnostics(void)`；`IDM_CE_LSP_DIAGNOSTICS`。

- [ ] **Step 1: 写失败测试**

在 `test/TestCeLsp.cpp` 增加纯 helper 断言：`_Celsp_DiagIsCurrent()` 对 severity 无关的 URI/version/generation 组合按现有策略返回 true/false；另加 `_Celsp_DiagSeverityForSquiggle(int)` 断言 1/2 为 true、3/4 为 false。客户端诊断缓存的 UI 表现不计入自动化断言。

- [ ] **Step 2: 运行确认失败**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: FAIL，诊断 severity 过滤 helper 不存在。

- [ ] **Step 3: 缓存诊断**

`CeLspFeatures.c` 增加文件静态 `LSP_DIAG s_diags[512]` 与 `s_diagUri/s_diagVersion`；`OnDiagnostics()` 先按当前 URI/版本过滤，解析全部 severity 到缓存，再按 1/2 渲染 marker/indicator。

- [ ] **Step 4: 实现列表窗口**

复用 `_SymWndProc` 的创建/双击跳转/关闭模式；显示 `severity/source/message/line:char`；双击调用 `SciCall_SetSel` 到缓存 range 起点并聚焦编辑窗口。无诊断时显示占位文本。

- [ ] **Step 5: 接命令与菜单**

`common_res.h` 新增 ID；中英文 CE 菜单各加一项；`MsgCommand()` 的 CE 分支调用 `CeLspFeatures_ShowDiagnostics()`。

- [ ] **Step 6: 验证**

Run 编译 x64 与 Win32，确认资源 ID 不冲突。手动打开含 warning/info 的脚本，确认列表显示 3 条、双击跳转正确、编辑器只画 2 条 squiggle。

