### Task 7: 配置默认值、INI 与服务器参数/长路径

**Files:**
- Modify: `src/TypeDefs.h:926-934`（`CeLspServerPath` 改 `HPATHL`，新增 `CeLspServerArgs[256]`）
- Modify: `src/CeSettings.c:351-360`（补齐默认值）
- Modify: `src/Config/Config.cpp:1236-1247,2201-2208`（读写新字段）
- Modify: `Build/Notepad3.ini:80-84`（补全 8 个键和参数说明）
- Modify: `src/CeLspClient.c:683-698`（动态命令行）

**Interfaces:**
- Consumes: 无。
- Produces: `Settings2.CeLspServerArgs`；`Settings2.CeLspServerPath` 为 `HPATHL`。

- [ ] **Step 1: 写失败测试**

`test/TestCeLspList.ps1` 或新增纯文本断言：读取 `CeSettings.c`、`Config.cpp`、`Build/Notepad3.ini`，断言 4 个缺失 bool 默认值出现、8 个 INI 键出现、`CeLspServerArgs` 被读写。文本断言可由 PowerShell `Select-String` 完成。

- [ ] **Step 2: 运行确认失败**

Run: `pwsh -NoProfile -File test\TestCeLspList.ps1`
Expected: FAIL，缺失键报告。

- [ ] **Step 3: 改字段与默认值**

`CeLspServerPath` 改 `HPATHL` 后，所有初始化/释放点按现有 `HPATHL` 生命周期处理；`CeSettings_ResetDefaults()` 补 4 个 bool 默认值与参数空串。

- [ ] **Step 4: INI 读写与示例**

沿用 `IniSectionGetBool/SetBool`；新增 `CeLspServerArgs` 用字符串读写；`Build/Notepad3.ini` 列出全部键，注释默认值。

- [ ] **Step 5: 动态命令行**

`CeLspClient_Start()` 用 `Path_Get(Settings2.CeLspServerPath)` 得到路径，按 `pathLen + argsLen + 8` 动态分配 `WCHAR*`；无自定义路径时仍拼 `<模块目录>\CeLspServer.exe`；`CreateProcessW` 命令行加 args（若有）。

- [ ] **Step 6: 验证**

Run: `pwsh -NoProfile -File test\TestCeLspList.ps1`
Expected: PASS。Run 编译 x64，确认无 struct 初始化遗漏。

