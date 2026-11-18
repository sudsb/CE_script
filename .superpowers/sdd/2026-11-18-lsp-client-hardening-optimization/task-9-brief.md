### Task 9: 全量验证与文档收尾

**Files:**
- Modify: `docs/superpowers/specs/2026-11-18-lsp-client-hardening-optimization-design.md`（状态改为已实现/部分待运行时验证）
- Modify: `docs/README.md`（如新增了 harness，加入索引行）

**Interfaces:**
- Consumes: Task 1-8 全部输出。
- Produces: 验证记录。

- [ ] **Step 1: 运行纯测试**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: PASS。

Run: `pwsh -NoProfile -File test\TestCeLspList.ps1`
Expected: PASS。

Run: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1`
Expected: PASS；若真实 `CeLspServer.exe` 不存在，记录真实服务器子项为 SKIP，不把 SKIP 写成 PASS。

- [ ] **Step 2: 运行其他针对性测试**

Run: `pwsh -NoProfile -File Build\scripts\Test-Docs.ps1`
Expected: PASS。

Run: `cmd /d /c "call test\TestFileVersion.cmd"`
Expected: PASS；若失败，先确认是否基线失败。

- [ ] **Step 3: 运行构建**

Discover MSBuild:
`& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1`

Run: `& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`
Expected: 0，无新错误。

Run: `& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=Win32 /nologo /v:minimal`
Expected: 0，无新错误。

- [ ] **Step 4: 更新设计状态与文档索引**

把设计文档状态改为“已实现（静态/自动化通过项列明；GUI 项按实际执行结果标注）”；若新增 harness 未进入索引，补一行。

- [ ] **Step 5: 不提交**

Run: `git --no-pager status --short`
Expected: 仅显示本计划涉及文件；不执行 `git commit`。
