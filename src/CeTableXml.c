// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeTableXml.c                                                                *
*   Import / export of the script embedded in a Cheat Engine table (.CT).     *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <commdlg.h>
#include <string.h>
#include <wchar.h>

#include "SciCall.h"
#include "CeScript.h"
#include "CeTableXml.h"

#define CE_XML_MAX_FILE (32 * 1024 * 1024)
#define CE_XML_FILTER   L"Cheat Engine Table (*.CT;*.cet;*.xml)\0*.CT;*.cet;*.xml\0All Files (*.*)\0*.*\0\0"

// ----------------------------------------------------------------------------

static char _Lower(char ch) {
    return (char)((ch >= 'A' && ch <= 'Z') ? (ch + ('a' - 'A')) : ch);
}

static const char* _FindCI(const char* text, size_t len, const char* needle, size_t needleLen) {
    if (needleLen == 0 || len < needleLen) {
        return NULL;
    }
    for (size_t i = 0; i + needleLen <= len; ++i) {
        size_t k = 0;
        while (k < needleLen && _Lower(text[i + k]) == _Lower(needle[k])) {
            ++k;
        }
        if (k == needleLen) {
            return text + i;
        }
    }
    return NULL;
}

// Match "<tag" / "</tag" only when a real tag ends there: "<tagEx>" and
// "</tagExtra>" must not be mistaken for the tag we are extracting.
static const char* _FindTag(const char* xml, size_t len, const char* tag, bool closing) {
    size_t const tagLen = strlen(tag);
    char needle[64] = { '\0' };
    if (tagLen + 2 > COUNTOF(needle)) {
        return NULL;
    }
    needle[0] = '<';
    if (closing) {
        needle[1] = '/';
        memcpy(needle + 2, tag, tagLen);
    }
    else {
        memcpy(needle + 1, tag, tagLen);
    }
    size_t const needleLen = tagLen + (closing ? 2 : 1);
    const char* p = xml;
    for (;;) {
        const char* const found = _FindCI(p, len - (size_t)(p - xml), needle, needleLen);
        if (!found) {
            return NULL;
        }
        const char* const after = found + needleLen;
        if ((size_t)(after - xml) < len) {
            char const ch = *after;
            if (ch == '>' || ch == '/' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                return found;
            }
        }
        p = found + 1;
    }
}

static char* _ReadFileUtf8(const WCHAR* path, size_t* lenOut) {
    *lenOut = 0;
    HANDLE const file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    LARGE_INTEGER size = { 0 };
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > CE_XML_MAX_FILE) {
        CloseHandle(file);
        return NULL;
    }
    size_t const len = (size_t)size.QuadPart;
    char* const data = AllocMem(len + 1, HEAP_ZERO_MEMORY);
    if (!data) {
        CloseHandle(file);
        return NULL;
    }
    DWORD read = 0;
    BOOL const ok = ReadFile(file, data, (DWORD)len, &read, NULL);
    CloseHandle(file);
    if (!ok) {
        FreeMem(data);
        return NULL;
    }
    data[read] = '\0';
    // UTF-8 BOM
    size_t start = 0;
    if (read >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        start = 3;
    }
    if (start > 0) {
        memmove(data, data + start, read - start);
        read -= (DWORD)start;
        data[read] = '\0';
    }
    *lenOut = read;
    return data;
}

static bool _WriteFileUtf8(const WCHAR* path, const char* text, size_t len) {
    HANDLE const file = CreateFileW(path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    BOOL const ok = WriteFile(file, text, (DWORD)len, &written, NULL);
    CloseHandle(file);
    return ok && written == len;
}

// ----------------------------------------------------------------------------

static void _Unescape(char* text) {
    char* src = text;
    char* dst = text;
    while (*src) {
        if (*src == '&') {
            if (_strnicmp(src, "&lt;", 4) == 0) {
                *dst++ = '<';
                src += 4;
                continue;
            }
            if (_strnicmp(src, "&gt;", 4) == 0) {
                *dst++ = '>';
                src += 4;
                continue;
            }
            if (_strnicmp(src, "&quot;", 6) == 0) {
                *dst++ = '"';
                src += 6;
                continue;
            }
            if (_strnicmp(src, "&apos;", 6) == 0) {
                *dst++ = '\'';
                src += 6;
                continue;
            }
            if (_strnicmp(src, "&amp;", 5) == 0) {
                *dst++ = '&';
                src += 5;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

// Return a heap copy of the text between <tag> and </tag>, or NULL.
static char* _ExtractTag(const char* xml, size_t len, const char* tag) {
    const char* const start = _FindTag(xml, len, tag, false);
    if (!start) {
        return NULL;
    }
    const char* const bodyStart = (const char*)memchr(start, '>', len - (size_t)(start - xml));
    if (!bodyStart) {
        return NULL;
    }
    const char* const end = _FindTag(bodyStart + 1, len - (size_t)(bodyStart + 1 - xml), tag, true);
    if (!end) {
        return NULL;
    }
    size_t const bodyLen = (size_t)(end - (bodyStart + 1));
    char* const body = AllocMem(bodyLen + 1, HEAP_ZERO_MEMORY);
    if (!body) {
        return NULL;
    }
    memcpy(body, bodyStart + 1, bodyLen);
    body[bodyLen] = '\0';
    _Unescape(body);
    return body;
}

static void _Escape(CE_BUF* out, const char* text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        switch (text[i]) {
        case '&':
            CeBuf_CatZ(out, "&amp;");
            break;
        case '<':
            CeBuf_CatZ(out, "&lt;");
            break;
        case '>':
            CeBuf_CatZ(out, "&gt;");
            break;
        default:
            CeBuf_Cat(out, text + i, 1);
            break;
        }
    }
}

static bool _LooksLikeLuaScript(const CE_DOC* doc) {
    static const char* const aaHints[] = {
        "alloc(", "globalalloc(", "label(", "registersymbol(", "aobscan", "define(",
        "dealloc(", "fullaccess(", "assert(", "createthread(", "[enable]", "[disable]",
        "mov ", "jmp ", "call ", "push ", "lea ", "db ", "dd "
    };
    for (DocLn line = 0; line < doc->lines; ++line) {
        DocPos n = 0;
        const char* const p = CeScript_Trimmed(doc, line, &n);
        if (n == 0) {
            continue;
        }
        for (int i = 0; i < (int)COUNTOF(aaHints); ++i) {
            size_t const hlen = strlen(aaHints[i]);
            if (n >= (DocPos)hlen) {
                size_t k = 0;
                while (k < hlen && _Lower(p[k]) == _Lower(aaHints[i][k])) {
                    ++k;
                }
                if (k == hlen) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void _Description(const CE_DOC* doc, char* out, size_t outCch) {
    StringCchCopyA(out, outCch, "CE Script");
    for (DocLn line = 0; line < doc->lines; ++line) {
        DocPos n = 0;
        const char* const p = CeScript_Trimmed(doc, line, &n);
        if (n >= 3 && p[0] == '-' && p[1] == '-') {
            const char* q = p + 2;
            while (q < p + n && (*q == ' ' || *q == '\t')) {
                ++q;
            }
            size_t len = (size_t)((p + n) - q);
            if (len > 0 && len < outCch) {
                memcpy(out, q, len);
                out[len] = '\0';
                return;
            }
        }
        if (n >= 3 && p[0] == '/' && p[1] == '/') {
            const char* q = p + 2;
            while (q < p + n && (*q == ' ' || *q == '\t')) {
                ++q;
            }
            size_t len = (size_t)((p + n) - q);
            if (len > 0 && len < outCch) {
                memcpy(out, q, len);
                out[len] = '\0';
                return;
            }
        }
    }
}

// ----------------------------------------------------------------------------

bool CeTableXml_Import(HWND hwndParent) {
    WCHAR path[1024] = { L'\0' };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndParent;
    ofn.lpstrFilter = CE_XML_FILTER;
    ofn.lpstrFile = path;
    ofn.nMaxFile = COUNTOF(path);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"Import Cheat Engine Table";
    if (!GetOpenFileNameW(&ofn)) {
        return false;
    }

    size_t len = 0;
    char* const xml = _ReadFileUtf8(path, &len);
    if (!xml) {
        MessageBoxW(hwndParent, L"The table file could not be read.", L"CE Table", MB_OK | MB_ICONERROR);
        return false;
    }

    char* script = _ExtractTag(xml, len, "AssemblerScript");
    if (!script) {
        script = _ExtractTag(xml, len, "LuaScript");
    }
    FreeMem(xml);
    if (!script) {
        MessageBoxW(hwndParent, L"No <AssemblerScript> or <LuaScript> was found in this table.",
            L"CE Table", MB_OK | MB_ICONWARNING);
        return false;
    }

    CeScript_ReplaceAll(script);
    FreeMem(script);
    return true;
}

// Build the one-entry cheat table XML for the current document.
static bool _BuildXml(const CE_DOC* doc, bool haveDoc, CE_BUF* out) {
    char description[128] = { '\0' };
    if (haveDoc) {
        _Description(doc, description, COUNTOF(description));
    }
    else {
        StringCchCopyA(description, COUNTOF(description), "CE Script");
    }

    if (!CeBuf_Init(out, 4096)) {
        return false;
    }

    CeBuf_CatZ(out, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
    CeBuf_CatZ(out, "<CheatTable>\n<CheatEntries>\n<CheatEntry>\n<ID>1</ID>\n<Description>\"");
    _Escape(out, description, strlen(description));
    CeBuf_CatZ(out, "\"</Description>\n");

    if (haveDoc && _LooksLikeLuaScript(doc)) {
        CeBuf_CatZ(out, "<LuaScript>");
        _Escape(out, doc->text, (size_t)doc->len);
        CeBuf_CatZ(out, "</LuaScript>\n");
    }
    else {
        CeBuf_CatZ(out, "<VariableType>Auto Assembler Script</VariableType>\n<AssemblerScript>");
        bool const hasEnable = haveDoc && (_FindCI(doc->text, (size_t)doc->len, "[enable]", 8) != NULL);
        if (!hasEnable) {
            CeBuf_CatZ(out, "[ENABLE]\n");
        }
        if (haveDoc) {
            _Escape(out, doc->text, (size_t)doc->len);
        }
        if (!hasEnable) {
            CeBuf_CatZ(out, "\n[DISABLE]\n");
        }
        CeBuf_CatZ(out, "</AssemblerScript>\n");
    }
    CeBuf_CatZ(out, "</CheatEntry>\n</CheatEntries>\n</CheatTable>\n");
    return (out->data != NULL);
}

// ----------------------------------------------------------------------------
// Clipboard helpers (UTF-8 <-> UTF-16).

static bool _SetClipboardText(HWND hwnd, const char* utf8, size_t len) {
    int const wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, NULL, 0);
    if (wlen <= 0) {
        return false;
    }
    HGLOBAL const mem = GlobalAlloc(GMEM_MOVEABLE, ((size_t)wlen + 1) * sizeof(WCHAR));
    if (!mem) {
        return false;
    }
    WCHAR* const wide = (WCHAR*)GlobalLock(mem);
    if (!wide) {
        GlobalFree(mem);
        return false;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, (int)len, wide, wlen);
    wide[wlen] = L'\0';
    GlobalUnlock(mem);

    if (!OpenClipboard(hwnd)) {
        GlobalFree(mem);
        return false;
    }
    EmptyClipboard();
    bool const ok = (SetClipboardData(CF_UNICODETEXT, mem) != NULL);
    CloseClipboard();
    if (!ok) {
        GlobalFree(mem);
    }
    return ok;
}

static char* _GetClipboardText(HWND hwnd, size_t* lenOut) {
    *lenOut = 0;
    if (!OpenClipboard(hwnd)) {
        return NULL;
    }
    HANDLE const handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
        CloseClipboard();
        return NULL;
    }
    const WCHAR* const wide = (const WCHAR*)GlobalLock(handle);
    if (!wide) {
        CloseClipboard();
        return NULL;
    }
    int const wlen = (int)wcslen(wide);
    int const bytes = WideCharToMultiByte(CP_UTF8, 0, wide, wlen, NULL, 0, NULL, NULL);
    char* text = NULL;
    if (bytes >= 0) {
        text = AllocMem((size_t)bytes + 1, HEAP_ZERO_MEMORY);
        if (text) {
            WideCharToMultiByte(CP_UTF8, 0, wide, wlen, text, bytes, NULL, NULL);
            text[bytes] = '\0';
            *lenOut = (size_t)bytes;
        }
    }
    GlobalUnlock(handle);
    CloseClipboard();
    return text;
}

// ----------------------------------------------------------------------------

bool CeTableXml_Export(HWND hwndParent) {
    CE_DOC doc = { 0 };
    bool const haveDoc = CeScript_InitDoc(&doc);

    WCHAR path[1024] = { L'\0' };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndParent;
    ofn.lpstrFilter = CE_XML_FILTER;
    ofn.lpstrFile = path;
    ofn.nMaxFile = COUNTOF(path);
    ofn.lpstrDefExt = L"CT";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"Export Cheat Engine Table";
    StringCchCopyW(path, COUNTOF(path), L"script.CT");
    if (!GetSaveFileNameW(&ofn)) {
        if (haveDoc) {
            CeScript_FreeDoc(&doc);
        }
        return false;
    }

    CE_BUF out = { 0 };
    if (!_BuildXml(&doc, haveDoc, &out)) {
        CeBuf_Free(&out);
        if (haveDoc) {
            CeScript_FreeDoc(&doc);
        }
        return false;
    }

    bool ok = _WriteFileUtf8(path, out.data, out.len);
    CeBuf_Free(&out);
    if (haveDoc) {
        CeScript_FreeDoc(&doc);
    }

    if (!ok) {
        MessageBoxW(hwndParent, L"The table file could not be written.", L"CE Table", MB_OK | MB_ICONERROR);
    }
    return ok;
}

bool CeTableXml_CopyScript(HWND hwndParent) {
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        MessageBoxW(hwndParent, L"The document is empty.", L"CE Script", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    bool const luaOnly = _LooksLikeLuaScript(&doc);
    bool const hasEnable = (_FindCI(doc.text, (size_t)doc.len, "[enable]", 8) != NULL);

    CE_BUF out = { 0 };
    if (!CeBuf_Init(&out, (size_t)doc.len + 64)) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    if (!luaOnly && !hasEnable) {
        CeBuf_CatZ(&out, "[ENABLE]\n");
    }
    CeBuf_Cat(&out, doc.text, (size_t)doc.len);
    if (!luaOnly && !hasEnable) {
        CeBuf_CatZ(&out, "\n[DISABLE]\n");
    }
    CeScript_FreeDoc(&doc);

    bool const ok = _SetClipboardText(hwndParent, out.data, out.len);
    CeBuf_Free(&out);
    if (!ok) {
        MessageBoxW(hwndParent, L"The clipboard could not be updated.", L"CE Script", MB_OK | MB_ICONERROR);
    }
    return ok;
}

bool CeTableXml_CopyXml(HWND hwndParent) {
    CE_DOC doc = { 0 };
    bool const haveDoc = CeScript_InitDoc(&doc);

    CE_BUF out = { 0 };
    if (!_BuildXml(&doc, haveDoc, &out)) {
        CeBuf_Free(&out);
        if (haveDoc) {
            CeScript_FreeDoc(&doc);
        }
        return false;
    }
    if (haveDoc) {
        CeScript_FreeDoc(&doc);
    }

    bool const ok = _SetClipboardText(hwndParent, out.data, out.len);
    CeBuf_Free(&out);
    if (!ok) {
        MessageBoxW(hwndParent, L"The clipboard could not be updated.", L"CE Table", MB_OK | MB_ICONERROR);
    }
    return ok;
}

bool CeTableXml_PasteXml(HWND hwndParent) {
    size_t len = 0;
    char* const text = _GetClipboardText(hwndParent, &len);
    if (!text) {
        MessageBoxW(hwndParent, L"The clipboard does not contain text.", L"CE Table", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    char* script = _ExtractTag(text, len, "AssemblerScript");
    if (!script) {
        script = _ExtractTag(text, len, "LuaScript");
    }
    FreeMem(text);
    if (!script) {
        MessageBoxW(hwndParent, L"No <AssemblerScript> or <LuaScript> was found on the clipboard.",
            L"CE Table", MB_OK | MB_ICONWARNING);
        return false;
    }

    CeScript_ReplaceAll(script);
    FreeMem(script);
    return true;
}
