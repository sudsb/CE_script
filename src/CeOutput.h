// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeOutput.h                                                                  *
*   Cheat Engine output: forwarded into the docked output/log panel so the     *
*   editor has a single log window for both editor logs and CE output.        *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_OUTPUT_H_
#define _CE_OUTPUT_H_

#include "TypeDefs.h"

void CeOutput_Show(HWND hwndParent);
void CeOutput_Append(HWND hwndParent, int kind, LPCWSTR text);
void CeOutput_Destroy(void);

#endif // _CE_OUTPUT_H_
