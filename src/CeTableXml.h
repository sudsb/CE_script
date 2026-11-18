// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeTableXml.h                                                                *
*   Import / export of the script embedded in a Cheat Engine table (.CT).     *
*                                                                             *
*   CE stores an Auto Assembler script in <AssemblerScript> and a trainer    *
*   script in <LuaScript>.  Import replaces the editor text with the         *
*   extracted script; export wraps the current text in a one-entry table.     *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_TABLE_XML_H_
#define _CE_TABLE_XML_H_

#include "TypeDefs.h"

bool CeTableXml_Import(HWND hwndParent);
bool CeTableXml_Export(HWND hwndParent);

// Clipboard variants, mirroring the reference CE Lua / AA editor.
bool CeTableXml_CopyScript(HWND hwndParent); // plain CE script, sections ensured
bool CeTableXml_CopyXml(HWND hwndParent);    // one-entry table XML
bool CeTableXml_PasteXml(HWND hwndParent);   // extract <AssemblerScript>/<LuaScript>

#endif // _CE_TABLE_XML_H_
