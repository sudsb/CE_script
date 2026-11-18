### Task 1: 纯策略 helper 与测试基线

**Files:**
- Modify: `src/CeLspClient.c:38-60`（保留现有纯 helper，新增纯 helper）
- Modify: `test/TestCeLsp.ps1:24-30`（抽取新增 helper）
- Modify: `test/TestCeLsp.cpp`（新增断言）

**Interfaces:**
- Consumes: 无。
- Produces:
  - `static bool _Celsp_MessageIsCurrent(UINT msgGeneration, UINT currentGeneration)`；
  - `static bool _Celsp_RequestExpired(ULONGLONG now, ULONGLONG deadline)`；
  - `static int _Celsp_PickPendingDrop(int count, const int* kinds, const ULONGLONG* deadlines, int newKind)`；
  - `static bool _Celsp_MergeChangeRange(int* start, int* end, int newStart, int newEnd)`；
  - `static bool _Celsp_DiagSeverityForSquiggle(int severity)`；
  - `static bool _Celsp_DiagIsCurrent(...)` 保持现有签名不变。

- [ ] **Step 1: 在 `test/TestCeLsp.cpp` 增加失败断言**

`TestSessionPolicy()` 中增加：
- `_Celsp_MessageIsCurrent(7,7)==true`、`(7,8)==false`；
- `_Celsp_RequestExpired(1000,999)==true`、`(1000,1000)==false`、`(1000,1001)==false`；
- `_Celsp_PickPendingDrop(3, kinds={1,2,2}, deadlines={500,100,200}, newKind=2)==1`；
- `_Celsp_MergeChangeRange(&s,&e,2,5,0)` 为 true 且后续 `(6,10)` 合并为 `(2,10)`；`(20,22)` 返回 false 并保持 `(2,10)`，表示客户端应另起范围段。

- [ ] **Step 2: 运行测试确认失败**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: FAIL，错误为新 helper 未定义或断言失败。

- [ ] **Step 3: 在 `CeLspClient.c` 实现四个纯 helper**

保持无 Win32/Scintilla 依赖；`_Celsp_PickPendingDrop` 优先返回同类最早 deadline，无同类时返回 deadline 最小下标；`_Celsp_MergeChangeRange` 只在相邻或重叠时合并，半开区间 `[start,end)`，不跨不相邻区间。

- [ ] **Step 4: 让 harness 抽取新 helper**

在 `test/TestCeLsp.ps1` 的 `$sig` 列表追加五个新签名（含 `_Celsp_DiagSeverityForSquiggle`），保持现有文本抽取方式，不 include `CeLspClient.c` 全文。

- [ ] **Step 5: 运行测试确认通过**

Run: `pwsh -NoProfile -File test\TestCeLsp.ps1`
Expected: PASS，输出无 `FAIL`。

