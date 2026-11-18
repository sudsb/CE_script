#pragma once

#include "TypeDefs.h"

// True when the script references the Cheat Engine API (or AA sections), i.e.
// when it can only run inside CE.  Pure Lua scripts return false and are run
// by the bundled-on-demand local Lua runtime instead of connecting to CE.
bool CeLua_IsCeRelated(const char* pszScript, size_t len);

// Synchronous, worker-thread entry point: run |script| in a fresh Lua state
// and post print()/result/error lines to the CE output pane.  No CE process,
// autorun script or named pipe is touched.  The caller owns |script|.
void CeLua_Execute(HWND hwnd, const char* pszScript, size_t len);
