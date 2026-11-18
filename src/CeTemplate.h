// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeTemplate.h                                                                *
*   Cheat Engine code templates and counterpart (pair) code generation.       *
*                                                                             *
*   CE's canonical script registers a symbol in [ENABLE] and unregisters and  *
*   deallocates it in [DISABLE].  Given an alloc/globalalloc (or the current  *
*   selection), this fills in the missing statements in the right section.    *
*                                                                             *
*   User templates follow ce_edit's TemplateRule: name / trigger / body /     *
*   section / auto.  '$ver$' (and any $name$ placeholder) is replaced by a    *
*   defined symbol when the template is inserted.                             *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_TEMPLATE_H_
#define _CE_TEMPLATE_H_

#include "TypeDefs.h"

// Generate the missing registersymbol/unregistersymbol/dealloc statements.
// Shows a short message when nothing can be generated.
bool CeTemplate_GeneratePairs(HWND hwndParent);

// Quiet variant used by the "auto pair code" Enter hook.
bool CeTemplate_GeneratePairsQuiet(void);

// Template catalogue (stored in Settings2.CeTemplates).
void        CeTemplate_ResetDefaults(void);
int         CeTemplate_Count(void);
const WCHAR* CeTemplate_Name(int index);

// Edit a single template in place.  Pass index == CeTemplate_Count() to append
// a new template (when below CE_TEMPLATES_MAX); an empty name falls back to
// "(未命名模板)".  Overwrites an existing slot when index < count.
void CeTemplate_Set(int index, LPCWSTR name, LPCWSTR trigger,
                    LPCWSTR section, bool autoPair, LPCWSTR body);
// Read a single template's fields (used by the settings editor dialog).
void CeTemplate_Get(int index, WCHAR* name, WCHAR* trigger, WCHAR* section,
                    bool* autoPair, WCHAR* body);
// Remove the template at |index|, shifting later entries down.
void CeTemplate_Remove(int index);

// Serialize one line per template ("name|trigger|section|auto|body"), using
// ";;" between templates and '\n' escapes inside the body.
void CeTemplate_Serialize(WCHAR* pszOut, size_t cchOut);
void CeTemplate_Deserialize(const WCHAR* pszText);

// Insert a rendered template at the caret.  Returns false when no variable
// could be resolved or the document is not a CE script.
bool CeTemplate_Insert(int index, HWND hwndParent);

// True when a function name matches a template trigger (completion fill-up).
bool CeTemplate_IsTrigger(const char* utf8Name);

// Called after Enter: generates counterpart code when the previous line is an
// alloc(...) statement and the corresponding option is enabled.
bool CeTemplate_OnReturn(void);

#endif // _CE_TEMPLATE_H_
