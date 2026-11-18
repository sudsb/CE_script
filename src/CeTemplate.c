// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeTemplate.c                                                                *
*   Cheat Engine counterpart (pair) code generation.                          *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <ctype.h>
#include <string.h>

#include <wchar.h>

#include "SciCall.h"
#include "CeCompletion.h"
#include "CeScript.h"
#include "CeSettings.h"
#include "CeTemplate.h"

// ----------------------------------------------------------------------------

static bool _EqualsCI(const char* text, size_t len, const char* word) {
    size_t const wordLen = strlen(word);
    if (len != wordLen) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char const a = (text[i] >= 'A' && text[i] <= 'Z') ? (char)(text[i] + 32) : text[i];
        char const b = (word[i] >= 'A' && word[i] <= 'Z') ? (char)(word[i] + 32) : word[i];
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool _IsIdentChar(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_';
}

static bool _HasCall(const CE_DOC* doc, const char* func, const char* name) {    size_t const funcLen = strlen(func);
    size_t const nameLen = strlen(name);
    const char* p = doc->text;
    const char* const end = doc->text + doc->len;
    while (p < end) {
        const char* const hit = (const char*)memchr(p, func[0], (size_t)(end - p));
        if (!hit) {
            break;
        }
        p = hit + 1;
        if (hit > doc->text && _IsIdentChar(hit[-1])) {
            continue; // "dealloc(" must not count as "alloc("
        }
        if ((size_t)(end - hit) < funcLen || !_EqualsCI(hit, funcLen, func)) {
            continue;
        }
        const char* q = hit + funcLen;
        while (q < end && (*q == ' ' || *q == '\t')) {
            ++q;
        }
        if (q >= end || *q != '(') {
            continue;
        }
        ++q;
        while (q < end && (*q == ' ' || *q == '\t')) {
            ++q;
        }
        if ((size_t)(end - q) < nameLen || !_EqualsCI(q, nameLen, name)) {
            continue;
        }
        const char* after = q + nameLen;
        if (after < end && (_IsIdentChar(*after) || *after == '.')) {
            continue;
        }
        return true;
    }
    return false;
}

// "newmem" is Cheat Engine's built-in template variable: the original CE script
// template already wires it up (alloc + registersymbol in [ENABLE], dealloc in
// [DISABLE]). User templates and the counterpart generator must never touch it;
// they operate only on user-defined variables.
static bool _IsNewmem(const char* name) {
    return _EqualsCI(name, strlen(name), "newmem");
}

// Selection (double-clicked word) wins when it names a defined symbol.
static bool _SelectedName(const CE_DEF* defs, int defCount, char* out, size_t outCch) {
    DocPos const start = SciCall_GetSelectionStart();
    DocPos const end = SciCall_GetSelectionEnd();
    if (end <= start || (end - start) >= 64) {
        return false;
    }
    char text[64] = { '\0' };
    for (DocPos i = start; i < end; ++i) {
        text[i - start] = (char)SciCall_GetCharAt(i);
    }
    text[end - start] = '\0';
    // Trim blanks and punctuation the user may have included.
    char* first = text;
    while (*first == ' ' || *first == '\t' || *first == '(' || *first == '[' ||
           *first == ',' || *first == ')' || *first == ']') {
        ++first;
    }
    size_t len = strlen(first);
    while (len > 0 && (first[len - 1] == ' ' || first[len - 1] == '\t' ||
                       first[len - 1] == ')' || first[len - 1] == ']' ||
                       first[len - 1] == ',')) {
        first[--len] = '\0';
    }
    if (len == 0 || !(first[0] == '_' || (first[0] >= 'a' && first[0] <= 'z') ||
                      (first[0] >= 'A' && first[0] <= 'Z'))) {
        return false;
    }
    if (CeScript_FindDef(defs, defCount, first) < 0) {
        return false;
    }
    if (_IsNewmem(first)) {
        return false; // never operate on CE's built-in template variable
    }
    StringCchCopyA(out, outCch, first);
    return true;
}

static bool _FirstAllocName(const CE_DEF* defs, int defCount, char* out, size_t outCch) {
    for (int i = 0; i < defCount; ++i) {
        if ((_EqualsCI(defs[i].kind, strlen(defs[i].kind), "alloc") ||
             _EqualsCI(defs[i].kind, strlen(defs[i].kind), "globalalloc") ||
             _EqualsCI(defs[i].kind, strlen(defs[i].kind), "sharedalloc")) &&
            !_IsNewmem(defs[i].name)) {
            StringCchCopyA(out, outCch, defs[i].name);
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------

// Normalize a section value into its stored form: strip surrounding brackets,
// uppercase, and fold the caret aliases (CARET / CURSOR / 光标) into L"CARET".
// An empty value falls back to L"ENABLE"; anything else is kept verbatim so
// users can target their own [NAME] blocks.
static void _NormalizeSection(LPCWSTR section, WCHAR* out, size_t cchOut) {
    out[0] = L'\0';
    if (!section) {
        StringCchCopyW(out, cchOut, L"ENABLE");
        return;
    }
    WCHAR buf[32];
    StringCchCopyW(buf, COUNTOF(buf), section);
    size_t len = wcslen(buf);
    if (len >= 2 && buf[0] == L'[' && buf[len - 1] == L']') {
        memmove(buf, buf + 1, (len - 1) * sizeof(WCHAR));
        buf[len - 2] = L'\0';
    }
    for (WCHAR* p = buf; *p; ++p) {
        *p = (WCHAR)towupper((wint_t)*p);
    }
    if (_wcsicmp(buf, L"CARET") == 0 || _wcsicmp(buf, L"CURSOR") == 0 ||
        wcscmp(buf, L"光标") == 0 || wcscmp(buf, L"光标位置") == 0) {
        StringCchCopyW(out, cchOut, L"CARET");
        return;
    }
    if (buf[0] == L'\0') {
        StringCchCopyW(out, cchOut, L"ENABLE");
        return;
    }
    StringCchCopyW(out, cchOut, buf);
}

// Find the end of the body of the [NAME] block identified by |target| (upper
// case, no brackets).  Returns the insertion offset (after any trailing blank
// lines) and sets *found, or -1 / false when no such block exists.  Any section
// marker is recognised, not just [ENABLE]/[DISABLE].
static DocPos _FindSectionInsertPos(const CE_DOC* doc, const char* target, bool* found) {
    *found = false;
    const DocLn lines = doc->lines;
    for (DocLn i = 0; i < lines; ++i) {
        const char* const lineStart = doc->text + doc->starts[i];
        const char* const lineEnd = doc->text + doc->starts[i + 1];
        const char* s = CeScript_SkipBlanks(lineStart, lineEnd);
        if (*s != '[') {
            continue;
        }
        const char* const close = (const char*)memchr(s, ']', (size_t)(lineEnd - s));
        if (!close) {
            continue;
        }
        const size_t nameLen = (size_t)(close - s - 1);
        if (nameLen == strlen(target) && _EqualsCI(s + 1, nameLen, target)) {
            DocPos insertPos = doc->len;
            for (DocLn j = i + 1; j < lines; ++j) {
                const char* const ls = doc->text + doc->starts[j];
                const char* const le = doc->text + doc->starts[j + 1];
                const char* const ss = CeScript_SkipBlanks(ls, le);
                if (*ss == '[' && memchr(ss, ']', (size_t)(le - ss))) {
                    insertPos = doc->starts[j];
                    break;
                }
            }
            while (insertPos > 0) {
                const char c = doc->text[insertPos - 1];
                if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
                    --insertPos;
                } else {
                    break;
                }
            }
            *found = true;
            return insertPos;
        }
    }
    return -1;
}

// Insert |text| (already rendered) at the end of the named [NAME] block.  When
// the block is missing, a fresh [NAME] block is appended at the end of the
// document instead.  Returns false when the document is not a CE script.
static bool _InsertIntoSection(const WCHAR* section, const char* text) {
    char target[32];
    WideCharToMultiByte(CP_UTF8, 0, section, -1, target, (int)COUNTOF(target), NULL, NULL);
    for (char* q = target; *q; ++q) {
        *q = (char)toupper((unsigned char)*q);
    }
    size_t tl = strlen(target);
    if (tl >= 2 && target[0] == '[' && target[tl - 1] == ']') {
        memmove(target, target + 1, tl - 2);
        target[tl - 2] = '\0';
    }

    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return false;
    }
    bool found = false;
    const DocPos pos = _FindSectionInsertPos(&doc, target, &found);
    const char* const eol = CeScript_EolStr();
    const size_t eolLen = strlen(eol);

    CE_BUF out = { 0 };
    if (!CeBuf_Init(&out, (size_t)doc.len + 1024)) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    const size_t textLen = text ? strlen(text) : 0;
    if (found) {
        CeBuf_Cat(&out, doc.text, (size_t)pos);
        if (pos > 0 && doc.text[pos - 1] != '\n') {
            CeBuf_Cat(&out, eol, eolLen);
        }
        if (text && textLen) {
            CeBuf_Cat(&out, text, textLen);
            if (text[textLen - 1] != '\n') {
                CeBuf_Cat(&out, eol, eolLen);
            }
        }
        CeBuf_Cat(&out, doc.text + pos, (size_t)(doc.len - pos));
    } else {
        CeBuf_Cat(&out, doc.text, (size_t)doc.len);
        if (doc.len > 0 && doc.text[doc.len - 1] != '\n') {
            CeBuf_Cat(&out, eol, eolLen);
        }
        CeBuf_CatZ(&out, "[");
        CeBuf_CatZ(&out, target);
        CeBuf_CatZ(&out, "]");
        CeBuf_Cat(&out, eol, eolLen);
        if (text && textLen) {
            CeBuf_Cat(&out, text, textLen);
            if (text[textLen - 1] != '\n') {
                CeBuf_Cat(&out, eol, eolLen);
            }
        }
    }
    if (out.data) {
        CeScript_ReplaceAll(out.data);
    }
    CeBuf_Free(&out);
    CeScript_FreeDoc(&doc);
    return true;
}

static void _Info(HWND hwnd, LPCWSTR text, bool quiet) {
    if (!quiet) {
        MessageBoxW(hwnd, text, L"CE Script", MB_OK | MB_ICONINFORMATION);
    }
}

static bool _GeneratePairs(HWND hwndParent, bool quiet) {
    if (!CeScript_IsActive()) {
        return false;
    }
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return false;
    }
    CE_DEF* const defs = AllocMem((size_t)CE_MAX_DEFS * sizeof(CE_DEF), HEAP_ZERO_MEMORY);
    if (!defs) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    int const defCount = CeScript_CollectDefs(&doc, defs, CE_MAX_DEFS);

    char name[64] = { '\0' };
    if (!_SelectedName(defs, defCount, name, COUNTOF(name)) &&
        !_FirstAllocName(defs, defCount, name, COUNTOF(name)) &&
        defCount > 0) {
        // Fall back to the first user-defined (non-newmem) symbol.
        for (int i = 0; i < defCount; ++i) {
            if (!_IsNewmem(defs[i].name)) {
                StringCchCopyA(name, COUNTOF(name), defs[i].name);
                break;
            }
        }
    }
    if (name[0] == '\0') {
        FreeMem(defs);
        CeScript_FreeDoc(&doc);
        _Info(hwndParent, L"No symbol found. Type alloc(name, 8) or select a defined symbol first.", quiet);
        return false;
    }

    bool const hasAlloc = _HasCall(&doc, "alloc", name) ||
        _HasCall(&doc, "globalalloc", name) ||
        _HasCall(&doc, "sharedalloc", name);
    bool const hasReg = _HasCall(&doc, "registersymbol", name);
    bool const hasUnreg = _HasCall(&doc, "unregistersymbol", name);
    bool const hasDealloc = _HasCall(&doc, "dealloc", name);

    bool const needEnable = hasAlloc && !hasReg;
    bool const needUnreg = (hasAlloc || hasReg) && !hasUnreg;
    bool const needDealloc = hasAlloc && !hasDealloc;

    if (!needEnable && !needUnreg && !needDealloc) {
        FreeMem(defs);
        CeScript_FreeDoc(&doc);
        _Info(hwndParent, L"The symbol already has every counterpart statement.", quiet);
        return false;
    }

    CE_SECTION sections[CE_MAX_SECTIONS];
    int const sectionCount = CeScript_CollectSections(&doc, sections, CE_MAX_SECTIONS);
    int enableIndex = -1;
    int disableIndex = -1;
    for (int i = 0; i < sectionCount; ++i) {
        if (sections[i].enable && enableIndex < 0) {
            enableIndex = i;
        }
        else if (!sections[i].enable && disableIndex < 0) {
            disableIndex = i;
        }
    }

    char enableLines[256] = { '\0' };
    char disableLines[512] = { '\0' };
    if (needEnable) {
        StringCchPrintfA(enableLines, COUNTOF(enableLines), "registersymbol(%s)", name);
    }
    if (needUnreg) {
        StringCchPrintfA(disableLines, COUNTOF(disableLines), "unregistersymbol(%s)", name);
    }
    if (needDealloc) {
        if (disableLines[0] != '\0') {
            StringCchCatA(disableLines, COUNTOF(disableLines), "\n");
        }
        StringCchCatA(disableLines, COUNTOF(disableLines), "dealloc(");
        StringCchCatA(disableLines, COUNTOF(disableLines), name);
        StringCchCatA(disableLines, COUNTOF(disableLines), ")");
    }

    const char* const eol = CeScript_EolStr();
    size_t const eolLen = strlen(eol);
    char indentUnit[40];
    int const unitLen = CeSettings_IndentUnit(indentUnit, COUNTOF(indentUnit));
    CE_BUF out = { 0 };
    if (!CeBuf_Init(&out, (size_t)doc.len + 512)) {
        FreeMem(defs);
        CeScript_FreeDoc(&doc);
        return false;
    }

    if (sectionCount == 0) {
        // No sections yet: wrap the script, then append the counterparts.
        CeBuf_CatZ(&out, "[ENABLE]");
        CeBuf_Cat(&out, eol, eolLen);
        CeBuf_Cat(&out, doc.text, (size_t)doc.len);
        if (doc.len == 0 || doc.text[doc.len - 1] != '\n') {
            CeBuf_Cat(&out, eol, eolLen);
        }
        if (enableLines[0] != '\0') {
            CeBuf_Cat(&out, indentUnit, (size_t)unitLen);
            CeBuf_CatZ(&out, enableLines);
            CeBuf_Cat(&out, eol, eolLen);
        }
        CeBuf_CatZ(&out, "[DISABLE]");
        CeBuf_Cat(&out, eol, eolLen);
        if (disableLines[0] != '\0') {
            const char* p = disableLines;
            while (*p) {
                CeBuf_Cat(&out, indentUnit, (size_t)unitLen);
                while (*p && *p != '\n') {
                    CeBuf_Cat(&out, p, 1);
                    ++p;
                }
                if (*p == '\n') {
                    ++p;
                }
                CeBuf_Cat(&out, eol, eolLen);
            }
        }
    }
    else {
        DocPos cursor = 0;
        for (int i = 0; i < sectionCount; ++i) {
            const CE_SECTION* const sec = &sections[i];
            bool const isEnable = sec->enable;
            const char* const lines = isEnable ? enableLines : disableLines;
            bool const isTarget = isEnable ? (i == enableIndex) : (i == disableIndex);
            if (!isTarget || lines[0] == '\0') {
                continue;
            }
            DocPos insertPos = CeScript_DeclarationAnchor(&doc, sec, isEnable);
            if (insertPos < 0) {
                // No declaration yet: append to the section body, stripping
                // the trailing blanks/EOLs first.
                insertPos = sec->end;
                while (insertPos > sec->bodyStart) {
                    char const prev = doc.text[insertPos - 1];
                    if (prev == '\n' || prev == '\r' || prev == ' ' || prev == '\t') {
                        --insertPos;
                    }
                    else {
                        break;
                    }
                }
            }
            if (insertPos > cursor) {
                CeBuf_Cat(&out, doc.text + cursor, (size_t)(insertPos - cursor));
                cursor = insertPos;
            }
            // Split the (possibly multi-line) statement list into document EOLs.
            const char* p = lines;
            while (*p) {
                CeBuf_Cat(&out, eol, eolLen);
                CeBuf_Cat(&out, indentUnit, (size_t)unitLen);
                while (*p && *p != '\n') {
                    CeBuf_Cat(&out, p, 1);
                    ++p;
                }
                if (*p == '\n') {
                    ++p;
                }
            }
        }
        if (cursor < doc.len) {
            CeBuf_Cat(&out, doc.text + cursor, (size_t)(doc.len - cursor));
        }
        // Missing target sections: append a labelled block at the end.
        if (needEnable && enableIndex < 0) {
            CeBuf_Cat(&out, eol, eolLen);
            CeBuf_CatZ(&out, "[ENABLE]");
            CeBuf_Cat(&out, eol, eolLen);
            CeBuf_Cat(&out, indentUnit, (size_t)unitLen);
            CeBuf_CatZ(&out, enableLines);
            CeBuf_Cat(&out, eol, eolLen);
        }
        if ((needUnreg || needDealloc) && disableIndex < 0) {
            CeBuf_Cat(&out, eol, eolLen);
            CeBuf_CatZ(&out, "[DISABLE]");
            CeBuf_Cat(&out, eol, eolLen);
            const char* p = disableLines;
            while (*p) {
                CeBuf_Cat(&out, indentUnit, (size_t)unitLen);
                while (*p && *p != '\n') {
                    CeBuf_Cat(&out, p, 1);
                    ++p;
                }
                if (*p == '\n') {
                    ++p;
                }
                CeBuf_Cat(&out, eol, eolLen);
            }
        }
    }

    if (out.data) {
        CeScript_ReplaceAll(out.data);
    }
    CeBuf_Free(&out);
    FreeMem(defs);
    CeScript_FreeDoc(&doc);
    return true;
}

bool CeTemplate_GeneratePairs(HWND hwndParent) {
    return _GeneratePairs(hwndParent, false);
}

bool CeTemplate_GeneratePairsQuiet(void) {
    return _GeneratePairs(Globals.hwndMain, true);
}

// ----------------------------------------------------------------------------
// User templates (stored in Settings2.CeTemplates).

static void _SetTemplate(int index, LPCWSTR name, LPCWSTR trigger,
                         LPCWSTR section, bool autoPair, LPCWSTR body)
{
    if (index < 0 || index >= CE_TEMPLATES_MAX) {
        return;
    }
    CE_TEMPLATE* const tpl = &Settings2.CeTemplates[index];
    StringCchCopyW(tpl->name, COUNTOF(tpl->name), name);
    StringCchCopyW(tpl->trigger, COUNTOF(tpl->trigger), trigger);
    StringCchCopyW(tpl->section, COUNTOF(tpl->section), section);
    StringCchCopyW(tpl->body, COUNTOF(tpl->body), body);
    tpl->autoPair = autoPair;
}

void CeTemplate_ResetDefaults(void) {
    ZeroMemory(Settings2.CeTemplates, sizeof(Settings2.CeTemplates));
    _SetTemplate(0, L"注册符号", L"registersymbol", L"ENABLE", true, L"registersymbol($ver$)");
    _SetTemplate(1, L"取消注册", L"unregistersymbol", L"DISABLE", true, L"unregistersymbol($ver$)");
    _SetTemplate(2, L"分配内存", L"alloc", L"ENABLE", true, L"alloc($ver$,8)");
    _SetTemplate(3, L"释放内存", L"dealloc", L"DISABLE", true, L"dealloc($ver$)");
    _SetTemplate(4, L"标签定义", L"label", L"ENABLE", true, L"label($ver$)");
    _SetTemplate(5, L"定义常量", L"define", L"ENABLE", false, L"define($ver$,0)");
    _SetTemplate(6, L"完整释放", L"dealloc", L"DISABLE", false,
                 L"unregistersymbol($ver$)\ndealloc($ver$)");
    _SetTemplate(7, L"完整分配", L"alloc", L"ENABLE", false,
                 L"alloc($ver$,8)\nregistersymbol($ver$)\nlabel($ver$)");
    Settings2.CeTemplateCount = 8;
}

int CeTemplate_Count(void) {
    return clampi(Settings2.CeTemplateCount, 0, CE_TEMPLATES_MAX);
}

const WCHAR* CeTemplate_Name(int index) {
    if (index < 0 || index >= CeTemplate_Count()) {
        return L"";
    }
    WCHAR* const name = Settings2.CeTemplates[index].name;
    return name[0] ? name : L"(未命名模板)";
}

static void _SetTemplate(int index, LPCWSTR name, LPCWSTR trigger,
                         LPCWSTR section, bool autoPair, LPCWSTR body);

void CeTemplate_Set(int index, LPCWSTR name, LPCWSTR trigger,
                    LPCWSTR section, bool autoPair, LPCWSTR body)
{
    if (index < 0 || index > CE_TEMPLATES_MAX) {
        return;
    }
    if ((index > Settings2.CeTemplateCount) ||
        (Settings2.CeTemplateCount >= CE_TEMPLATES_MAX)) {
        return;
    }
    if (name == NULL || name[0] == L'\0') {
        name = L"(未命名模板)";
    }
    if (trigger == NULL) {
        trigger = L"";
    }
    if (body == NULL) {
        body = L"";
    }
    WCHAR sec[32];
    _NormalizeSection(section, sec, COUNTOF(sec));
    _SetTemplate(index, name, trigger, sec, autoPair, body);
    if (index == Settings2.CeTemplateCount) {
        ++Settings2.CeTemplateCount;
    }
}

void CeTemplate_Get(int index, WCHAR* name, WCHAR* trigger, WCHAR* section,
                    bool* autoPair, WCHAR* body)
{
    if (index < 0 || index >= Settings2.CeTemplateCount) {
        if (name)     name[0] = L'\0';
        if (trigger)  trigger[0] = L'\0';
        if (section)  section[0] = L'\0';
        if (body)     body[0] = L'\0';
        if (autoPair) *autoPair = false;
        return;
    }
    const CE_TEMPLATE* const tpl = &Settings2.CeTemplates[index];
    if (name)     StringCchCopyW(name, CE_TEMPLATE_NAME_CCH, tpl->name);
    if (trigger)  StringCchCopyW(trigger, 64, tpl->trigger);
    if (section)  StringCchCopyW(section, 16, tpl->section);
    if (autoPair) *autoPair = tpl->autoPair;
    if (body)     StringCchCopyW(body, CE_TEMPLATE_BODY_CCH, tpl->body);
}

void CeTemplate_Remove(int index)
{
    if (index < 0 || index >= Settings2.CeTemplateCount) {
        return;
    }
    for (int i = index; i < Settings2.CeTemplateCount - 1; ++i) {
        Settings2.CeTemplates[i] = Settings2.CeTemplates[i + 1];
    }
    ZeroMemory(&Settings2.CeTemplates[Settings2.CeTemplateCount - 1],
               sizeof(CE_TEMPLATE));
    --Settings2.CeTemplateCount;
}

static void _EscapeBody(LPCWSTR src, WCHAR* dst, size_t cchDst) {
    size_t o = 0;
    for (LPCWSTR p = src; *p && (o + 2 < cchDst); ++p) {
        switch (*p) {
        case L'\\': dst[o++] = L'\\'; dst[o++] = L'\\'; break;
        case L'\n': dst[o++] = L'\\'; dst[o++] = L'n';   break;
        case L'\t': dst[o++] = L'\\'; dst[o++] = L't';   break;
        case L'|':  dst[o++] = L'\\'; dst[o++] = L'p';   break;
        case L';':  dst[o++] = L'\\'; dst[o++] = L's';   break;
        case L'\r': break;
        default:    dst[o++] = *p; break;
        }
    }
    dst[o] = L'\0';
}

static void _UnescapeBody(LPCWSTR src, WCHAR* dst, size_t cchDst) {
    size_t o = 0;
    for (LPCWSTR p = src; *p && (o + 1 < cchDst); ++p) {
        if (p[0] == L'\\' && p[1]) {
            ++p;
            dst[o++] = (p[0] == L'n') ? L'\n' :
                       (p[0] == L't') ? L'\t' :
                       (p[0] == L'p') ? L'|'  :
                       (p[0] == L's') ? L';'  : p[0];
        }
        else {
            dst[o++] = *p;
        }
    }
    dst[o] = L'\0';
}

void CeTemplate_Serialize(WCHAR* pszOut, size_t cchOut) {
    if (!pszOut || cchOut == 0) {
        return;
    }
    pszOut[0] = L'\0';
    WCHAR escaped[CE_TEMPLATE_BODY_CCH * 2];
    WCHAR line[XXHUGE_BUFFER];
    for (int i = 0; i < CeTemplate_Count(); ++i) {
        const CE_TEMPLATE* const tpl = &Settings2.CeTemplates[i];
        _EscapeBody(tpl->body, escaped, COUNTOF(escaped));
        StringCchPrintfW(line, COUNTOF(line), L"%s|%s|%s|%d|%s",
                         tpl->name, tpl->trigger, tpl->section,
                         tpl->autoPair ? 1 : 0, escaped);
        if (i > 0) {
            StringCchCatW(pszOut, cchOut, L";;");
        }
        StringCchCatW(pszOut, cchOut, line);
    }
}

void CeTemplate_Deserialize(const WCHAR* pszText) {
    if (!pszText) {
        return;
    }
    Settings2.CeTemplateCount = 0;
    WCHAR copy[XXHUGE_BUFFER];
    StringCchCopyW(copy, COUNTOF(copy), pszText);
    WCHAR* ctx = NULL;
    WCHAR* part = wcstok_s(copy, L";", &ctx); // ";;" separator
    while (part && *part && (Settings2.CeTemplateCount < CE_TEMPLATES_MAX)) {
        WCHAR fields[5][CE_TEMPLATE_BODY_CCH];
        const WCHAR* p = part;
        for (int f = 0; f < 5; ++f) {
            const WCHAR* const sep = wcschr(p, L'|');
            size_t const len = sep ? (size_t)(sep - p) : wcslen(p);
            size_t const use = min_s(len, COUNTOF(fields[f]) - 1);
            memcpy(fields[f], p, use * sizeof(WCHAR));
            fields[f][use] = L'\0';
            p = sep ? (sep + 1) : (p + len);
        }
        WCHAR body[CE_TEMPLATE_BODY_CCH];
        _UnescapeBody(fields[4], body, COUNTOF(body));
        WCHAR sec[32];
        _NormalizeSection(fields[2], sec, COUNTOF(sec));
        if (fields[0][0] && fields[1][0] && body[0]) {
            _SetTemplate(Settings2.CeTemplateCount++, fields[0], fields[1], sec,
                         (_wtoi(fields[3]) != 0), body);
        }
        part = wcstok_s(NULL, L";", &ctx);
    }
}

bool CeTemplate_IsTrigger(const char* utf8Name) {
    if (StrIsEmptyA(utf8Name)) {
        return false;
    }
    WCHAR wide[64];
    MultiByteToWideChar(CP_UTF8, 0, utf8Name, -1, wide, (int)COUNTOF(wide));
    for (int i = 0; i < CeTemplate_Count(); ++i) {
        if (_wcsicmp(Settings2.CeTemplates[i].trigger, wide) == 0) {
            return true;
        }
    }
    return false;
}

// Render a template body into |out|, substituting every "$name$" placeholder.
// When |var| is non-NULL it replaces the placeholder; when NULL the placeholder
// is replaced by spaces, yielding a blank function (used when the clipboard has
// no variable name).
static bool _CaretAtLineStart(void)
{
    DocPos const pos = SciCall_GetCurrentPos();
    DocLn const line = SciCall_LineFromPosition(pos);
    DocPos const start = SciCall_PositionFromLine(line);
    for (DocPos i = start; i < pos; ++i) {
        char const ch = (char)SciCall_GetCharAt(i);
        if ((ch != ' ') && (ch != '\t')) {
            return false;
        }
    }
    return true;
}

static void _RenderTemplate(const CE_TEMPLATE* tpl, const char* var, bool indentFirst, CE_BUF* out) {
    char body[CE_TEMPLATE_BODY_CCH * 3];
    WideCharToMultiByte(CP_UTF8, 0, tpl->body, -1, body, (int)COUNTOF(body), NULL, NULL);
    const char* const eol = CeScript_EolStr();
    char indentUnit[40];
    int const unitLen = CeSettings_IndentUnit(indentUnit, COUNTOF(indentUnit));
    // When pasted at the caret, match the current line's indentation; when
    // dropped into a [SECTION] block, the block owns the indentation.
    if (indentFirst && _CaretAtLineStart()) {
        CeBuf_Cat(out, indentUnit, (size_t)unitLen);
    }
    for (const char* p = body; *p; ) {
        if (*p == '$') {
            const char* const close = strchr(p + 1, '$');
            if (close) {
                if (var) {
                    CeBuf_CatZ(out, var);
                }
                else {
                    // Blank function: pad the placeholder with spaces so the
                    // function keeps its shape.
                    for (size_t k = 0; k + 1 < (size_t)(close - p); ++k) {
                        CeBuf_CatZ(out, " ");
                    }
                }
                p = close + 1;
                continue;
            }
        }
        if (*p == '\n') {
            CeBuf_CatZ(out, eol);
            ++p;
            // Continuation lines are function-like statements: indent them
            // unless the template already supplies its own leading blanks.
            if (*p && (*p != ' ') && (*p != '\t')) {
                CeBuf_Cat(out, indentUnit, (size_t)unitLen);
            }
            continue;
        }
        CeBuf_Cat(out, p, 1);
        ++p;
    }
}

bool CeTemplate_Insert(int index, HWND hwndParent) {
    UNREFERENCED_PARAMETER(hwndParent);
    if (!CeScript_IsActive() || index < 0 || index >= CeTemplate_Count()) {
        return false;
    }

    // A selected identifier in the editor wins; the clipboard is the fallback.
    // Read the selection before ReplaceSel() below overwrites it.  "newmem" is
    // CE's built-in template variable and is never operated on here.
    char var[240];
    bool haveVar = CeCompletion_GetSelectedVariable(var, COUNTOF(var)) ||
                   CeCompletion_GetClipboardVariable(var, COUNTOF(var));
    if (haveVar && _IsNewmem(var)) {
        haveVar = false;
    }

    const CE_TEMPLATE* const tpl = &Settings2.CeTemplates[index];
    const bool isCaret = (_wcsicmp(tpl->section, L"CARET") == 0);
    CE_BUF out = { 0 };
    if (!CeBuf_Init(&out, CE_TEMPLATE_BODY_CCH * 3)) {
        return false;
    }
    _RenderTemplate(tpl, haveVar ? var : NULL, isCaret, &out);

    bool ok = false;
    if (isCaret) {
        // Paste at the caret, matching the current line's indentation.
        SciCall_BeginUndoAction();
        SciCall_ReplaceSel(out.data ? out.data : "");
        SciCall_EndUndoAction();
        ok = true;
    } else {
        // Drop into the named [SECTION] block (ENABLE / DISABLE / custom).
        ok = _InsertIntoSection(tpl->section, out.data ? out.data : "");
    }
    CeBuf_Free(&out);
    return ok;
}

bool CeTemplate_OnReturn(void) {
    if (!CeScript_IsActive() || !CeSettings_AutoPairCode()) {
        return false;
    }
    DocPos const pos = SciCall_GetCurrentPos();
    DocLn const line = SciCall_LineFromPosition(pos);
    if (line <= 0) {
        return false;
    }
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return false;
    }
    unsigned char* const modes = AllocMem((size_t)doc.lines, HEAP_ZERO_MEMORY);
    if (!modes) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    CeScript_LineModes(&doc, modes);
    DocPos n = 0;
    const char* const p = CeScript_Trimmed(&doc, line - 1, &n);
    const char* const end = p + n;
    bool isAlloc = (modes[line - 1] == (unsigned char)CE_LANG_AA);
    static const char* const kinds[] = { "alloc", "globalalloc", "sharedalloc" };
    if (isAlloc) {
        isAlloc = false;
        for (int i = 0; i < (int)COUNTOF(kinds) && !isAlloc; ++i) {
            size_t const klen = strlen(kinds[i]);
            if (((size_t)(end - p) > klen) && (_strnicmp(p, kinds[i], klen) == 0) &&
                (p[klen] == '(' || p[klen] == ' ' || p[klen] == '\t')) {
                isAlloc = true;
            }
        }
    }
    FreeMem(modes);
    CeScript_FreeDoc(&doc);
    if (!isAlloc) {
        return false;
    }
    return CeTemplate_GeneratePairsQuiet();
}
