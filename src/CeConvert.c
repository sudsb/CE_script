// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeConvert.c                                                                 *
*   Number base conversion, memory-size prefixes, register width conversion   *
*   and declaration generation for a selected CE symbol.                      *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "resource.h"
#include "SciCall.h"
#include "CeCompletion.h"
#include "CeConvert.h"
#include "CeScript.h"
#include "CeSettings.h"

// ----------------------------------------------------------------------------

static bool _GetSelection(char* pszOut, size_t cchOut, DocPos* pStart, DocPos* pEnd)
{
    DocPos const start = SciCall_GetSelectionStart();
    DocPos const end = SciCall_GetSelectionEnd();
    if ((end <= start) || ((size_t)(end - start) >= cchOut)) {
        return false;
    }
    char const* const p = SciCall_GetRangePointer(start, end - start);
    if (!p) {
        return false;
    }
    memcpy(pszOut, p, (size_t)(end - start));
    pszOut[end - start] = '\0';
    *pStart = start;
    *pEnd = end;
    return true;
}

static char* _Trim(char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        ++s;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

// ----------------------------------------------------------------------------
// Registers: the four columns are 64 / 32 / 16 / 8 bit aliases.

static const char* const s_regRows[][4] = {
    { "rax",  "eax",  "ax",   "al"   },
    { "rbx",  "ebx",  "bx",   "bl"   },
    { "rcx",  "ecx",  "cx",   "cl"   },
    { "rdx",  "edx",  "dx",   "dl"   },
    { "rsi",  "esi",  "si",   "sil"  },
    { "rdi",  "edi",  "di",   "dil"  },
    { "rbp",  "ebp",  "bp",   "bpl"  },
    { "rsp",  "esp",  "sp",   "spl"  },
    { "r8",   "r8d",  "r8w",  "r8b"  },
    { "r9",   "r9d",  "r9w",  "r9b"  },
    { "r10",  "r10d", "r10w", "r10b" },
    { "r11",  "r11d", "r11w", "r11b" },
    { "r12",  "r12d", "r12w", "r12b" },
    { "r13",  "r13d", "r13w", "r13b" },
    { "r14",  "r14d", "r14w", "r14b" },
    { "r15",  "r15d", "r15w", "r15b" },
    { "rip",  "eip",  "ip",   "ip"   },
    { "xmm0", "xmm0", "xmm0", "xmm0" },
};

static bool _FindReg(const char* name, int* pRow)
{
    for (int r = 0; r < (int)COUNTOF(s_regRows); ++r) {
        for (int c = 0; c < 4; ++c) {
            if (_stricmp(name, s_regRows[r][c]) == 0) {
                *pRow = r;
                return true;
            }
        }
    }
    return false;
}

static bool _IsIdentifier(const char* s)
{
    if (!s[0] || !(isalpha((unsigned char)s[0]) || s[0] == '_')) {
        return false;
    }
    for (const char* p = s; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '$' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool _IsNumber(const char* s)
{
    if (!s[0]) {
        return false;
    }
    const char* p = s;
    if (*p == '+' || *p == '-') {
        ++p;
    }
    bool digit = false;
    bool hexLetter = false;
    bool dot = false;
    for (; *p; ++p) {
        if (isdigit((unsigned char)*p)) {
            digit = true;
        }
        else if ((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
            hexLetter = true;
        }
        else if (*p == '.' && !dot) {
            dot = true;
        }
        else if ((*p == 'x' || *p == 'X' || *p == 'b' || *p == 'B') && (p == s + 1) && (s[0] == '0')) {
            // 0x / 0b prefix
        }
        else if ((*p == 'h' || *p == 'H' || *p == 'f' || *p == 'F') && !p[1]) {
            // trailing radix / float suffix
        }
        else {
            return false;
        }
    }
    return digit;
}

static const char* _StripSizePrefix(const char* s)
{
    static const char* const prefixes[] = {
        "byte ptr ", "word ptr ", "dword ptr ", "qword ptr ",
        "float ", "double ", "ptr "
    };
    for (int i = 0; i < (int)COUNTOF(prefixes); ++i) {
        size_t const n = strlen(prefixes[i]);
        if (_strnicmp(s, prefixes[i], n) == 0) {
            return s + n;
        }
    }
    return s;
}

// ----------------------------------------------------------------------------

CE_SEL_KIND CeConvert_ClassifySelection(void)
{
    char buf[256];
    DocPos start = 0, end = 0;
    if (!_GetSelection(buf, COUNTOF(buf), &start, &end)) {
        return CE_SEL_NONE;
    }
    char* const s = _Trim(buf);
    if (!s[0]) {
        return CE_SEL_NONE;
    }
    const char* const mem = _StripSizePrefix(s);
    if ((mem[0] == '[') && strchr(mem, ']')) {
        return CE_SEL_MEMORY;
    }
    int row = -1;
    if (_FindReg(s, &row)) {
        return CE_SEL_REGISTER;
    }
    if (_IsNumber(s)) {
        return CE_SEL_NUMBER;
    }
    if (_IsIdentifier(s) && !CeCompletion_IsBuiltinKeyword(s)) {
        return CE_SEL_SYMBOL;
    }
    return CE_SEL_NONE;
}

// ----------------------------------------------------------------------------
// Numeric conversions.

typedef struct _ce_number {
    bool                  isFloat;
    bool                  negative;
    bool                  singlePrecision;
    double                fval;
    unsigned long long    uval;
    int                   width;   // bytes used for the byte-array output
} CE_NUMBER;

static bool _ParseNumber(const char* src, CE_NUMBER* out)
{
    ZeroMemory(out, sizeof(*out));
    char buf[128];
    StringCchCopyA(buf, COUNTOF(buf), src);
    char* s = _Trim(buf);
    size_t len = strlen(s);
    if (len == 0) {
        return false;
    }
    if (s[len - 1] == 'f' || s[len - 1] == 'F') {
        out->singlePrecision = true;
        s[--len] = '\0';
    }
    bool hexSuffix = false;
    if (len > 0 && (s[len - 1] == 'h' || s[len - 1] == 'H')) {
        hexSuffix = true;
        s[--len] = '\0';
    }
    out->negative = (s[0] == '-');
    if (s[0] == '+' || s[0] == '-') {
        ++s;
    }
    bool hexPrefix = ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X'));
    bool binPrefix = ((s[0] == '0') && (s[1] == 'b' || s[1] == 'B'));
    bool hasHexLetter = false;
    for (const char* p = s; *p; ++p) {
        if (((*p >= 'a') && (*p <= 'f')) || ((*p >= 'A') && (*p <= 'F'))) {
            hasHexLetter = true;
        }
    }
    bool const isFloat = !hexPrefix && !hexSuffix && !hasHexLetter && binPrefix == false &&
                         (strchr(s, '.') != NULL);
    if (isFloat) {
        char* endPtr = NULL;
        double const v = strtod(s, &endPtr);
        if (endPtr == s) {
            return false;
        }
        out->isFloat = true;
        out->fval = out->negative ? -v : v;
        if (out->singlePrecision) {
            float const f = (float)out->fval;
            unsigned int bits = 0;
            memcpy(&bits, &f, sizeof(bits));
            out->uval = bits;
            out->width = 4;
        }
        else {
            unsigned long long bits = 0;
            memcpy(&bits, &out->fval, sizeof(bits));
            out->uval = bits;
            out->width = 8;
        }
        return true;
    }

    char const* digits = s;
    int base = 10;
    if (hexPrefix) {
        base = 16;
        digits = s + 2;
    }
    else if (binPrefix) {
        base = 2;
        digits = s + 2;
    }
    else if (hexSuffix || hasHexLetter) {
        base = 16;
    }
    char* endPtr = NULL;
    unsigned long long const v = _strtoui64(digits, &endPtr, base);
    if (endPtr == digits) {
        return false;
    }
    out->uval = v; // magnitude; the sign is kept separately for the decimal form
    out->width = (v <= 0xFFULL) ? 1 : (v <= 0xFFFFULL) ? 2 :
                 (v <= 0xFFFFFFFFULL) ? 4 : 8;
    return true;
}

static void _FormatBytes(const CE_NUMBER* num, char* out, size_t cch, bool dbPrefix)
{
    StringCchCopyA(out, cch, dbPrefix ? "db " : "");
    char byte[8];
    for (int i = 0; i < num->width; ++i) {
        unsigned int const b = (unsigned int)((num->uval >> (8 * i)) & 0xFFULL);
        StringCchPrintfA(byte, COUNTOF(byte), "%02X", b);
        StringCchCatA(out, cch, byte);
        if ((i + 1) < num->width) {
            StringCchCatA(out, cch, " ");
        }
    }
}

static void _FormatBinary(unsigned long long value, char* out, size_t cch)
{
    StringCchCopyA(out, cch, "0b");
    size_t const head = strlen(out);
    char bits[65];
    int n = 0;
    if (value == 0) {
        bits[n++] = '0';
    }
    while (value && (n < 64)) {
        bits[n++] = (char)('0' + (value & 1ULL));
        value >>= 1;
    }
    for (int i = 0; i < n && (head + (size_t)i + 1 < cch); ++i) {
        out[head + (size_t)i] = bits[n - 1 - i];
        out[head + (size_t)i + 1] = '\0';
    }
}

static bool _ExecuteNumber(unsigned int cmd, const char* sel, DocPos start, DocPos end)
{
    CE_NUMBER num;
    if (!_ParseNumber(sel, &num)) {
        return false;
    }
    char text[256] = { '\0' };
    switch (cmd) {
    case IDM_CE_CONV_TO_DEC:
        if (num.isFloat) {
            StringCchPrintfA(text, COUNTOF(text), "%.9g", num.fval);
        }
        else {
            StringCchPrintfA(text, COUNTOF(text), "%s%llu", num.negative ? "-" : "", num.uval);
        }
        break;
    case IDM_CE_CONV_TO_HEX:
        StringCchPrintfA(text, COUNTOF(text), "0x%llX", num.uval);
        break;
    case IDM_CE_CONV_TO_BIN:
        _FormatBinary(num.uval, text, COUNTOF(text));
        break;
    case IDM_CE_CONV_TO_OCT:
        StringCchPrintfA(text, COUNTOF(text), "0o%llo", num.uval);
        break;
    case IDM_CE_CONV_TO_BYTES:
        _FormatBytes(&num, text, COUNTOF(text), true);
        break;
    case IDM_CE_CONV_TO_AOB:
        _FormatBytes(&num, text, COUNTOF(text), false);
        break;
    case IDM_CE_CONV_TO_FLOAT:
        // Bit-reinterpret the value's IEEE-754 bits as a 32-bit float.
        if (num.isFloat) {
            StringCchPrintfA(text, COUNTOF(text), "%s%.9gf",
                              num.negative ? "-" : "", num.fval);
        }
        else {
            unsigned int const bits = (unsigned int)(num.uval & 0xFFFFFFFFULL);
            float f = 0.0f;
            memcpy(&f, &bits, sizeof(f));
            StringCchPrintfA(text, COUNTOF(text), "%.9gf", f);
        }
        break;
    case IDM_CE_CONV_TO_DOUBLE:
        // Bit-reinterpret the value's IEEE-754 bits as a 64-bit double.
        if (num.isFloat) {
            StringCchPrintfA(text, COUNTOF(text), "%s%.17g",
                              num.negative ? "-" : "", num.fval);
        }
        else {
            unsigned long long const bits = num.uval;
            double d = 0.0;
            memcpy(&d, &bits, sizeof(d));
            StringCchPrintfA(text, COUNTOF(text), "%.17g", d);
        }
        break;
    case IDM_CE_CONV_TO_FVAL:
        // Value cast: keep the numeric value, render it as a float literal.
        if (num.isFloat) {
            StringCchPrintfA(text, COUNTOF(text), "%s%.9gf",
                              num.negative ? "-" : "", num.fval);
        }
        else {
            StringCchPrintfA(text, COUNTOF(text), "%s%.9gf",
                              num.negative ? "-" : "", (double)num.uval);
        }
        break;
    case IDM_CE_CONV_TO_DVAL:
        // Value cast: keep the numeric value, render it as a double literal.
        if (num.isFloat) {
            StringCchPrintfA(text, COUNTOF(text), "%s%.17g",
                              num.negative ? "-" : "", num.fval);
        }
        else {
            StringCchPrintfA(text, COUNTOF(text), "%s%.17g",
                              num.negative ? "-" : "", (double)num.uval);
        }
        break;
    case IDM_CE_CONV_RESTORE:
        // Reverse of the bit-reinterpret conversion: a float/double literal
        // becomes the decimal integer whose bit pattern it represents.
        StringCchPrintfA(text, COUNTOF(text), "%llu", num.uval);
        break;
    default:
        return false;
    }
    SciCall_BeginUndoAction();
    SciCall_SetSel(start, end);
    SciCall_ReplaceSel(text);
    SciCall_EndUndoAction();
    return true;
}

// ----------------------------------------------------------------------------

static bool _ExecuteMemory(unsigned int cmd, const char* sel, DocPos start, DocPos end)
{
    const char* prefix = NULL;
    switch (cmd) {
    case IDM_CE_CONV_MEM_BYTE:   prefix = "byte ptr ";  break;
    case IDM_CE_CONV_MEM_WORD:   prefix = "word ptr ";  break;
    case IDM_CE_CONV_MEM_DWORD:  prefix = "dword ptr "; break;
    case IDM_CE_CONV_MEM_QWORD:  prefix = "qword ptr "; break;
    case IDM_CE_CONV_MEM_FLOAT:  prefix = "(float) ";   break;
    case IDM_CE_CONV_MEM_DOUBLE: prefix = "(double) ";  break;
    default:
        return false;
    }
    char text[300];
    StringCchPrintfA(text, COUNTOF(text), "%s%s", prefix, _StripSizePrefix(sel));
    SciCall_BeginUndoAction();
    SciCall_SetSel(start, end);
    SciCall_ReplaceSel(text);
    SciCall_EndUndoAction();
    return true;
}

static bool _ExecuteRegister(unsigned int cmd, const char* sel, DocPos start, DocPos end)
{
    int row = -1;
    if (!_FindReg(sel, &row)) {
        return false;
    }
    int column = -1;
    switch (cmd) {
    case IDM_CE_CONV_REG_64: column = 0; break;
    case IDM_CE_CONV_REG_32: column = 1; break;
    case IDM_CE_CONV_REG_16: column = 2; break;
    case IDM_CE_CONV_REG_8:  column = 3; break;
    default:
        return false;
    }
    char const* const replacement = s_regRows[row][column];
    if (_stricmp(replacement, sel) == 0) {
        return true; // already the requested width
    }
    SciCall_BeginUndoAction();
    SciCall_SetSel(start, end);
    SciCall_ReplaceSel(replacement);
    SciCall_EndUndoAction();
    return true;
}

// ----------------------------------------------------------------------------

// End of the last line starting with |kind|("(" ...), or -1.  Keeps alloc()
// next to alloc(), label() next to label(), and so on.
static DocPos _KindAnchor(const CE_DOC* doc, const CE_SECTION* sec, const char* kind)
{
    size_t const kindLen = strlen(kind);
    if (kindLen == 0) {
        return -1;
    }
    DocPos p = sec->bodyStart;
    DocPos anchor = -1;
    while (p < sec->end) {
        DocPos q = p;
        while ((q < sec->end) && (doc->text[q] != '\n') && (doc->text[q] != '\r')) {
            ++q;
        }
        const char* s = doc->text + p;
        size_t len = (size_t)(q - p);
        while ((len > 0) && ((*s == ' ') || (*s == '\t'))) {
            ++s;
            --len;
        }
        if ((len > kindLen) && (_strnicmp(s, kind, kindLen) == 0) &&
            ((s[kindLen] == '(') || (s[kindLen] == ' ') || (s[kindLen] == '\t'))) {
            anchor = q;
        }
        p = q;
        while ((p < sec->end) && ((doc->text[p] == '\n') || (doc->text[p] == '\r'))) {
            ++p;
        }
    }
    return anchor;
}

// Declarations belong to the declaration block of their section, never to the
// assembly line the user happened to select a symbol on.
static void _InsertStatement(const char* statement, bool enable)
{
    char unit[40];
    CeSettings_IndentUnit(unit, COUNTOF(unit));
    CE_BUF out = { 0 };
    if (!CeBuf_Init(&out, 256)) {
        return;
    }
    CeBuf_CatZ(&out, CeScript_EolStr());
    CeBuf_CatZ(&out, unit);
    for (const char* p = statement; *p; ) {
        if (*p == '\n') {
            CeBuf_CatZ(&out, CeScript_EolStr());
            CeBuf_CatZ(&out, unit);
            ++p;
        }
        else {
            CeBuf_Cat(&out, p, 1);
            ++p;
        }
    }
    char const* const text = out.data ? out.data : "";

    DocPos const selStart = SciCall_GetSelectionStart();
    DocPos const selEnd = SciCall_GetSelectionEnd();
    DocPos insert = -1;

    char kind[32] = { '\0' };
    {
        size_t k = 0;
        for (const char* kp = statement; *kp && (*kp != '(') && (*kp != '\n') &&
                                        ((k + 1) < COUNTOF(kind)); ++kp) {
            kind[k++] = *kp;
        }
        kind[k] = '\0';
    }

    CE_DOC doc = { 0 };
    if (CeScript_InitDoc(&doc)) {
        CE_SECTION sections[CE_MAX_SECTIONS];
        int const sectionCount = CeScript_CollectSections(&doc, sections, CE_MAX_SECTIONS);
        for (int i = 0; i < sectionCount; ++i) {
            if (sections[i].enable != enable) {
                continue;
            }
            insert = _KindAnchor(&doc, &sections[i], kind);
            if (insert < 0) {
                insert = CeScript_DeclarationAnchor(&doc, &sections[i], enable);
            }
            break;
        }
        CeScript_FreeDoc(&doc);
    }
    if (insert < 0) {
        // No [ENABLE]/[DISABLE] section yet: fall back to the current line.
        DocLn const line = SciCall_LineFromPosition(selEnd);
        insert = SciCall_GetLineEndPosition(line);
    }

    SciCall_BeginUndoAction();
    SciCall_SetSel(insert, insert);
    SciCall_ReplaceSel(text);
    DocPos const delta = (insert <= selStart) ? (DocPos)strlen(text) : 0;
    SciCall_SetSel(selStart + delta, selEnd + delta);
    SciCall_EndUndoAction();
    CeBuf_Free(&out);
}

// A label definition line ends with ':' and has no spaces ("newmem:",
// "return:", "Module:Func+51:").  The trailing colon decides.
static bool _IsLabelDefLine(const char* p, size_t len)
{
    while ((len > 0) && ((*p == ' ') || (*p == '\t'))) {
        ++p;
        --len;
    }
    while ((len > 0) && ((p[len - 1] == ' ') || (p[len - 1] == '\t') ||
                         (p[len - 1] == '\r') || (p[len - 1] == '\n'))) {
        --len;
    }
    if ((len < 2) || (p[len - 1] != ':')) {
        return false;
    }
    if (!(isalpha((unsigned char)*p) || (*p == '_') || isdigit((unsigned char)*p))) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if ((p[i] == ' ') || (p[i] == '\t')) {
            return false;
        }
    }
    return true;
}

static bool _HasLabelDef(const CE_DOC* doc, const char* name)
{
    size_t const n = strlen(name);
    for (DocLn l = 0; l < doc->lines; ++l) {
        DocPos len = 0;
        const char* const p = CeScript_Trimmed(doc, l, &len);
        if (((size_t)len >= (n + 1)) && (_strnicmp(p, name, n) == 0) && (p[n] == ':')) {
            return true;
        }
    }
    return false;
}

// "label(qaq)" also needs the matching "qaq:" code region.  It is placed just
// before the next label definition after the caret (or at the section end).
static void _InsertLabelRegion(const char* name)
{
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return;
    }
    if (_HasLabelDef(&doc, name)) {
        CeScript_FreeDoc(&doc);
        return;
    }

    CE_SECTION sections[CE_MAX_SECTIONS];
    int const sectionCount = CeScript_CollectSections(&doc, sections, CE_MAX_SECTIONS);
    DocLn const caretLine = SciCall_LineFromPosition(SciCall_GetSelectionEnd());
    DocPos insert = -1;
    for (int i = 0; i < sectionCount; ++i) {
        if (!sections[i].enable) {
            continue;
        }
        for (DocLn l = caretLine + 1; l < doc.lines; ++l) {
            DocPos const lineStart = doc.starts[l];
            if (lineStart >= sections[i].end) {
                break;
            }
            DocPos len = 0;
            const char* const p = CeScript_LinePtr(&doc, l, &len);
            if (_IsLabelDefLine(p, (size_t)len)) {
                insert = lineStart;
                break;
            }
        }
        if (insert < 0) {
            insert = sections[i].end;
        }
        break;
    }
    if (insert < 0) {
        // No [ENABLE] section: put the label region right below the caret.
        DocLn const line = SciCall_LineFromPosition(SciCall_GetSelectionEnd());
        insert = SciCall_GetLineEndPosition(line);
    }
    CeScript_FreeDoc(&doc);

    const char* const eol = CeScript_EolStr();
    char const prev = (insert > 0) ? (char)SciCall_GetCharAt(insert - 1) : '\n';
    bool const needEol = (prev != '\n') && (prev != '\r');
    char text[256];
    StringCchPrintfA(text, COUNTOF(text), "%s%s:%s%s",
                     needEol ? eol : "", name, eol, eol);

    DocPos const selStart = SciCall_GetSelectionStart();
    DocPos const selEnd = SciCall_GetSelectionEnd();
    SciCall_BeginUndoAction();
    SciCall_SetSel(insert, insert);
    SciCall_ReplaceSel(text);
    DocPos const delta = (insert <= selStart) ? (DocPos)strlen(text) : 0;
    SciCall_SetSel(selStart + delta, selEnd + delta);
    SciCall_EndUndoAction();
}

static bool _ExecuteGenerate(unsigned int cmd, const char* sel)
{
    char name[128];
    StringCchCopyA(name, COUNTOF(name), sel);
    char* const s = _Trim(name);
    if (!_IsIdentifier(s)) {
        return false;
    }
    char line[256];
    char statement[512];
    bool enable = true;
    bool const makeLabelRegion = (cmd == IDM_CE_GEN_LABEL) || (cmd == IDM_CE_GEN_FULL);
    switch (cmd) {
    case IDM_CE_GEN_ALLOC:
        StringCchPrintfA(statement, COUNTOF(statement), "alloc(%s,8)", s);
        break;
    case IDM_CE_GEN_LABEL:
        StringCchPrintfA(statement, COUNTOF(statement), "label(%s)", s);
        break;
    case IDM_CE_GEN_REGISTERSYMBOL:
        StringCchPrintfA(statement, COUNTOF(statement), "registersymbol(%s)", s);
        break;
    case IDM_CE_GEN_UNREGISTERSYMBOL:
        StringCchPrintfA(statement, COUNTOF(statement), "unregistersymbol(%s)", s);
        enable = false;
        break;
    case IDM_CE_GEN_DEALLOC:
        StringCchPrintfA(statement, COUNTOF(statement), "dealloc(%s)", s);
        enable = false;
        break;
    case IDM_CE_GEN_DEFINE:
        StringCchPrintfA(statement, COUNTOF(statement), "define(%s,0)", s);
        break;
    case IDM_CE_GEN_FULL:
        StringCchPrintfA(line, COUNTOF(line), "alloc(%s,8)", s);
        StringCchPrintfA(statement, COUNTOF(statement), "%s\nregistersymbol(%s)\nlabel(%s)",
                         line, s, s);
        break;
    default:
        return false;
    }
    _InsertStatement(statement, enable);
    if (makeLabelRegion) {
        _InsertLabelRegion(s);
    }
    return true;
}

// ----------------------------------------------------------------------------

bool CeConvert_Execute(unsigned int cmd)
{
    if ((cmd < IDM_CE_CONV_FIRST || cmd > IDM_CE_CONV_LAST) &&
        (cmd < IDM_CE_GEN_FIRST || cmd > IDM_CE_GEN_LAST)) {
        return false;
    }
    char buf[256];
    DocPos start = 0, end = 0;
    if (!_GetSelection(buf, COUNTOF(buf), &start, &end)) {
        return false;
    }
    char* const s = _Trim(buf);
    if (!s[0]) {
        return false;
    }
    if (cmd >= IDM_CE_GEN_FIRST) {
        return _ExecuteGenerate(cmd, s);
    }
    if (cmd <= IDM_CE_CONV_TO_AOB ||
        (cmd >= IDM_CE_CONV_TO_FLOAT && cmd <= IDM_CE_CONV_RESTORE)) {
        return _ExecuteNumber(cmd, s, start, end);
    }
    if (cmd <= IDM_CE_CONV_MEM_DOUBLE) {
        return _ExecuteMemory(cmd, s, start, end);
    }
    return _ExecuteRegister(cmd, s, start, end);
}

// ----------------------------------------------------------------------------
// Symbol rename (context menu).  A CE script refers to its own symbols from
// alloc/label/registersymbol, jumps and [symbol] operands, so all whole-word
// occurrences have to change together; Scintilla's target search keeps this a
// single undo step and preserves the caret.

static bool _IsSymbolName(const char* s)
{
    if (!s || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) {
        return false;
    }
    for (const char* p = s + 1; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) {
            return false;
        }
    }
    return true;
}

static bool _GetRenameTarget(char* out, size_t cch, DocPos clickPos)
{
    char sel[256];
    DocPos start = 0, end = 0;
    if (_GetSelection(sel, COUNTOF(sel), &start, &end)) {
        char* const s = _Trim(sel);
        if (!_IsSymbolName(s)) {
            return false;
        }
        return (StringCchCopyA(out, cch, s) == S_OK);
    }
    DocPos const pos = (clickPos >= 0) ? clickPos : SciCall_GetCurrentPos();
    if (pos < 0) {
        return false;
    }
    DocPos const ws = SciCall_WordStartPosition(pos, true);
    DocPos const we = SciCall_WordEndPosition(pos, true);
    if ((we <= ws) || ((size_t)(we - ws) >= cch)) {
        return false;
    }
    char const* const p = SciCall_GetRangePointer(ws, we - ws);
    if (!p) {
        return false;
    }
    memcpy(out, p, (size_t)(we - ws));
    out[we - ws] = '\0';
    return _IsSymbolName(out);
}

bool CeConvert_RenameSymbol(HWND hwnd, DocPos clickPos)
{
    if (SciCall_GetReadOnly()) {
        return false;
    }
    char oldName[128] = { '\0' };
    if (!_GetRenameTarget(oldName, COUNTOF(oldName), clickPos)) {
        MessageBoxW(hwnd, L"请把光标放在变量名上，或只选中一个变量名。",
                    L"重命名符号", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    WCHAR wchNew[128] = { L'\0' };
    MultiByteToWideChar(CP_UTF8, 0, oldName, -1, wchNew, (int)COUNTOF(wchNew));
    if (!CeSettings_InputBox(hwnd, L"重命名符号", L"新名称（字母、数字、下划线）：",
                             wchNew, COUNTOF(wchNew))) {
        return false;
    }
    char newName[128] = { '\0' };
    WideCharToMultiByte(CP_UTF8, 0, wchNew, -1, newName, (int)COUNTOF(newName), NULL, NULL);
    if (!_IsSymbolName(newName)) {
        MessageBoxW(hwnd, L"名称必须以字母或下划线开头，且只能包含字母、数字和下划线。",
                    L"重命名符号", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (_stricmp(newName, oldName) == 0) {
        return false;
    }
    // A symbol that collides with an AA/Lua keyword, register or CE built-in
    // makes the script fail to activate in Cheat Engine: refuse it up front.
    if (CeCompletion_IsBuiltinKeyword(newName) || (_stricmp(newName, "newmem") == 0)) {
        MessageBoxW(hwnd, L"该名称与 CE 关键字或保留字冲突，脚本可能无法正常激活，请换一个名称。",
                    L"重命名符号", MB_OK | MB_ICONWARNING);
        return false;
    }

    int const savedFlags = SciCall_GetSearchFlags();
    DocPos const savedTargetStart = SciCall_GetTargetStart();
    DocPos const savedTargetEnd = SciCall_GetTargetEnd();
    size_t const oldLen = strlen(oldName);
    DocPos docEnd = SciCall_GetLength();
    DocPos pos = 0;
    int count = 0;

    SciCall_BeginUndoAction();
    SciCall_SetSearchFlags(SCFIND_WHOLEWORD); // case-insensitive, whole identifier
    while (pos < docEnd) {
        SciCall_SetTargetRange(pos, docEnd);
        if (SciCall_SearchInTarget((DocPos)oldLen, oldName) < 0) {
            break;
        }
        DocPos const tStart = SciCall_GetTargetStart();
        // SCI_REPLACETARGET's length argument describes the *new* text (-1 =
        // NUL-terminated), and its return value is the length inserted, not the
        // position after the replacement.  Passing the old match length here
        // truncated longer names and restarting at that length made the loop
        // re-find the same match forever.  Advance from the updated target end
        // and re-read the document length instead of tracking a delta.
        SciCall_ReplaceTarget(-1, newName);
        DocPos const next = SciCall_GetTargetEnd();
        if (next <= tStart) { // defensive: never allow a non-advancing loop
            break;
        }
        pos = next;
        docEnd = SciCall_GetLength();
        ++count;
    }
    SciCall_EndUndoAction();

    SciCall_SetSearchFlags(savedFlags);
    SciCall_SetTargetRange(savedTargetStart, savedTargetEnd);
    SciCall_ScrollCaret();
    return (count > 0);
}
