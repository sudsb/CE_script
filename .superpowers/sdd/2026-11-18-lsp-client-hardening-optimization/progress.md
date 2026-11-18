# SDD ledger — plan: docs/superpowers/plans/2026-11-18-lsp-client-hardening-optimization.md

Setup: 2026-11-18, native inline execution chosen by user.

Ruling: no Git repository exists at workspace root (`git rev-parse` fails) — work in place and keep commit/BASE fields out of ledger; use file state and repeated test output as evidence — cost if wrong: no git history for rollback, so each task must stay small and verified.

Ruling: design builder's two questions were already answered by the 2026-11-18 analysis — E2E harness does not simulate client UI, and LSP settings stay INI-only this round — cost if wrong: client UI side effects need manual smoke, not automation.

Baseline: `pwsh -NoProfile -File test\TestCeLsp.ps1` PASS; `pwsh -NoProfile -File test\TestCeLspList.ps1` PASS; `cmd /d /c "call test\TestFileVersion.cmd"` FAIL pre-existing (expected 7.26.1118.0, binaries 5.5.0.9); `Test-Docs.ps1` has two pre-existing Git-root metadata errors.

Pre-flight: Task 1 produces pure helpers consumed by Tasks 2/3/5/8; names and signatures are consistent in the plan. Task 3 event-handle reuse is prerequisite for Task 4 stderr thread teardown; consistent. Task 4 server stdio isolation is prerequisite for Task 5 protocol harness confidence; consistent. Task 5 change ranges feed Task 6 range diagnostics and Task 8 diagnostic cache; consistent. Task 7 HPATHL field change is independent of Tasks 1-6 except `CeLspClient_Start()`; sequenced after task 6 to avoid conflicting edits. No unresolved interface conflicts.

Task 1: complete (tests: `pwsh -NoProfile -File test\TestCeLsp.ps1` → all checks passed). RED observed as C3861 missing helpers; GREEN after implementation.
Task 1: Ruling: `_Celsp_MergeChangeRange` uses the 5-argument form from the brief Step 1 (`docLen` clamps ranges) instead of the 4-argument form in Produces — the asserted behavior needs a document bound and matches the design's range safety requirement — cost if wrong: one extra argument at Task 5 call sites.
Task 2: complete (tests: `pwsh -NoProfile -File test\TestCeLsp.ps1` → all checks passed; `msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64` → exit 0). Manual unresponsive-server smoke not run (no GUI/server runtime in this session); recorded as unrun, not passed.
Task 3: complete (tests: `pwsh -NoProfile -File test\TestCeLsp.ps1` → all checks passed; `msbuild src\Notepad3.vcxproj /t:ClCompile /p:Configuration=Release /p:Platform=x64` → exit 0). Manual force-kill smoke not run.
Task 3: Ruling: async teardown no longer queues LSP `shutdown`/`exit`; the writer closes inherited stdin on the generation boundary and the built-in server exits on EOF, with the existing 300ms process timeout as backstop — queueing frames into a detached queue would let an old writer drain the restarted session's frames — cost if wrong: server misses the formal shutdown handshake.
Task 3: Ruling: added public `CeLspClient_OnTeardownTimer()` for the UI fallback branch because static `_FinalizeTeardown()` cannot be called from `Notepad3.c` — cost if wrong: one extra header API.
Task 3: Ruling: `_WriterThread` now takes a generation argument and checks it before draining; without this an old writer could drain the new session's queue after event handles became shared — cost if wrong: writer signature change only.
Task 4: complete (tests: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1` → all checks passed; `pwsh -NoProfile -File test\TestCeLsp.ps1` → all checks passed; x64 ClCompile → exit 0). Test harness RED was infrastructure-only (missing script then parse bugs); product stderr routing verified by compile and protocol-level stream separation, not by a Win32 client harness.
Task 4: Ruling: the new `TestCeLspEndToEnd.ps1` uses a scripted stdio server to verify stdout framing/stderr separation but does not instantiate `CeLspClient.c`; client log UI remains manual smoke — cost if wrong: a regression in `_StderrThread` itself would not be caught automatically.
Task 5: complete (tests: `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1` → all checks passed after server rebuild; `pwsh -NoProfile -File test\TestCeLsp.ps1` → all checks passed; server build and x64 ClCompile → exit 0). RED observed: incremental response differed from full response before server change.
Task 5: Ruling: only UTF-8 insertions accumulate incremental ranges; deletions, undo/redo, multi-line deletions and UTF-16 sessions force full sync — computing an old-document end position for deletions from post-change Scintilla state is unreliable, and full sync is already the fallback contract — cost if wrong: deletions still pay full-document sync.
Task 5: Ruling: server capability changed from Full(1) to Incremental(2); `_HandleDidChange()` applies each change in order and still accepts no-range full text — cost if wrong: third-party clients that assumed Full must still work, which the no-range branch preserves.
Task 6: complete (tests: rebuilt server, `pwsh -NoProfile -File test\TestCeLspEndToEnd.ps1` → all checks passed; `pwsh -NoProfile -File test\TestCeLsp.ps1` → all checks passed; x64 ClCompile → exit 0). SemanticTokens/diagnostic-range assertions were added after the server behavior existed, so they are guards rather than observed RED tests; incremental-sync RED from Task 5 remains the protocol-level proof.
Task 6: Ruling: range diagnostics recompute the full scan then merge by changed line instead of true line-local analysis — cross-line state (sections, label/alloc declarations) makes partial scans unsafe — cost if wrong: diagnostics remain O(document), but unchanged lines keep their previous findings.
Task 6: Ruling: semanticTokens caching is server-side keyed by document version; client-side duplicate suppression was not added because no version API exists in `CeLspClient.h` — cost if wrong: duplicate requests still cross the pipe but no recomputation occurs.
Task 7: complete (tests: `pwsh -NoProfile -File test\TestCeLspList.ps1` → all checks passed after config changes; x64 ClCompile → exit 0). RED observed as missing default/INI keys before changes.
Task 7: Ruling: config verification uses source/INI text assertions plus compilation because `Settings2` is populated by Win32/INI code that cannot be loaded in the pure harness; these assertions guard drift but do not prove runtime INI round-tripping — cost if wrong: an INI API regression would need manual/automated UI verification.
Task 8: complete (tests: x64 and Win32 ClCompile → exit 0; pure severity/filter assertions already covered by Task 1 and still passing). GUI diagnostics-list smoke not run.
Task 8: Ruling: exposed `CeLspClient_DiagnosticSeverityVisible()` instead of calling the static pure helper from `CeLspFeatures.c`; without this the client-side squiggle filter and the list cache could drift — cost if wrong: one extra public API.
Final review: fresh code-explorer reviewer run against plan/spec/ledger. Found 1 Critical and 4 Important issues; self-graded as reported.

Final: fixed C1 insertion range covered inserted text instead of being empty — `TestInsertionPosition` RED→GREEN plus `TestCeLspEndToEnd.ps1` middle-insertion case; suite 3/3 LSP harnesses green, Release x64/Win32 builds green.
Final: fixed I1 `SciCall_GetColumn()` tab-expanded columns by using snapshot-based `_Celsp_InsertionPosition()`; same RED→GREEN test as C1.
Final: fixed I2 multiple insertions in one debounce window by pure `_Celsp_AllowIncremental()` guard (RED→GREEN) forcing full sync after the first range.
Final: fixed I3 settings toggles by refreshing decorations from CE Settings OK/Reload and clearing stale hint/codeLens annotations; verified by x64/Win32 ClCompile, GUI toggle smoke still unrun.
Final: fixed I4 server range failure by `np3/lspResync` notification and client full-sync fallback; `TestCeLspEndToEnd.ps1` RED→GREEN after server line validation.
Final: fixed I5 diagnostics-list Chinese-only labels by locale-aware `_DiagUiText()`; verified by compilation, GUI locale smoke unrun.

Final: minor (deferred): no placeholder text in empty diagnostics list (M1).
Final: minor (deferred): teardown timer fallback only kills timer without extra UI cleanup (M2).
Final: minor (deferred): `Settings2.CeLspServerPath` HPATHL allocation has no explicit release at shutdown (M3).
Final: minor (deferred): dropping same-kind pending requests does not emit `$/cancelRequest` (M4).
Final: minor (deferred): stderr log generation check and post are not atomic (M5).
Final: minor (deferred): `CeLspPos_FromOffset` treats utf-8 as code-point units rather than LSP-standard bytes for third-party servers (M6).
Final: minor (deferred): 1000ms timeout timer granularity can delay a 3s cancel to ~4s (M7).

Task 9: complete (tests: TestCeLsp.ps1, TestCeLspList.ps1, TestCeLspEndToEnd.ps1 all pass; Release x64 and Win32 full builds exit 0; Test-Docs.ps1 and TestFileVersion.cmd retain pre-existing baseline failures; GUI smoke not run). No git worktree/commits exist because the checkout is not a Git repository.
