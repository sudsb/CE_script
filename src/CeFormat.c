// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeFormat.c                                                                  *
*   Conservative document formatter for Cheat Engine scripts.                 *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <ctype.h>
#include <string.h>

#include "SciCall.h"
#include "CeScript.h"
#include "CeSettings.h"
#include "CeFormat.h"

// ----------------------------------------------------------------------------

static bool _Contains(const char* p, size_t len, const char* needle) {
    size_t const n = strlen(needle);
    if (len < n) {
        return false;
    }
    for (size_t i = 0; i + n <= len; ++i) {
        if (memcmp(p + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool _IsBlank(char ch) {
    return ch == ' ' || ch == '\t';
}

// Copy |len| characters, collapsing runs of blanks outside strings/comments.
static size_t _CollapseBlanks(const char* p, size_t len, char* out, size_t outCch) {
    size_t o = 0;
    bool inString = false;
    char quote = '\0';
    bool pendingBlank = false;
    for (size_t i = 0; i < len && o + 1 < outCch; ++i) {
        char const ch = p[i];
        if (inString) {
            out[o++] = ch;
            if (ch == '\\' && (i + 1) < len && o + 1 < outCch) {
                out[o++] = p[++i];
            }
            else if (ch == quote) {
                inString = false;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            if (pendingBlank && o > 0) {
                out[o++] = ' ';
                pendingBlank = false;
            }
            inString = true;
            quote = ch;
            out[o++] = ch;
            continue;
        }
        if (ch == '-' && (i + 1) < len && p[i + 1] == '-') {
            if (pendingBlank && o > 0) {
                out[o++] = ' ';
            }
            while (i < len && o + 1 < outCch) {
                out[o++] = p[i++];
            }
            break;
        }
        if (ch == '/' && (i + 1) < len && p[i + 1] == '/') {
            if (pendingBlank && o > 0) {
                out[o++] = ' ';
            }
            while (i < len && o + 1 < outCch) {
                out[o++] = p[i++];
            }
            break;
        }
        if (_IsBlank(ch)) {
            pendingBlank = (o > 0);
            continue;
        }
        if (ch == ',' || ch == ')' || ch == ']' || ch == '}') {
            pendingBlank = false;
            out[o++] = ch;
            continue;
        }
        if (pendingBlank) {
            out[o++] = ' ';
            pendingBlank = false;
        }
        out[o++] = ch;
    }
    while (o > 0 && _IsBlank(out[o - 1])) {
        --o;
    }
    out[o] = '\0';
    return o;
}

// Cheat Engine's assembler does not accept blanks inside a memory operand:
// "[rdi - 18]" is rejected while "[rdi-18]" assembles.  Drop every blank
// between the brackets, but never touch strings or trailing comments.  A
// section marker like "[ENABLE]" has no blanks, so it stays unchanged.
static size_t _StripAsmBracketBlanks(const char* p, size_t len, char* out, size_t outCch) {
    size_t o = 0;
    char quote = '\0';
    int depth = 0;
    for (size_t i = 0; i < len && o + 1 < outCch; ++i) {
        char const ch = p[i];
        if (quote) {
            out[o++] = ch;
            if ((ch == '\\') && ((i + 1) < len) && (o + 1 < outCch)) {
                out[o++] = p[++i];
            }
            else if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if ((ch == '"') || (ch == '\'')) {
            quote = ch;
            out[o++] = ch;
            continue;
        }
        if ((ch == ';') || ((ch == '/') && ((i + 1) < len) && (p[i + 1] == '/')) ||
            ((ch == '-') && ((i + 1) < len) && (p[i + 1] == '-'))) {
            while ((i < len) && (o + 1 < outCch)) {
                out[o++] = p[i++];
            }
            break;
        }
        if (ch == '[') {
            ++depth;
            out[o++] = ch;
            continue;
        }
        if ((ch == ']') && (depth > 0)) {
            --depth;
            out[o++] = ch;
            continue;
        }
        if ((depth > 0) && _IsBlank(ch)) {
            continue;
        }
        out[o++] = ch;
    }
    out[o] = '\0';
    return o;
}

// Auto Assembler is the only section type the formatter indents.  Inside it,
// only labels, section markers, directives and comments stay at column zero:
// every function-like statement (alloc/label/registersymbol/...), instruction
// and data line is indented by one level, following the reference CE script.
static int _AaIndentLevels(const char* body, size_t len)
{
    if (len == 0) {
        return 0;
    }
    char const ch = body[0];
    if ((ch == '[') || (ch == '{') || (ch == ';') ||
        ((len >= 2) && (((ch == '-') && (body[1] == '-')) ||
                        ((ch == '/') && (body[1] == '/'))))) {
        return 0;
    }
    // "newmem:", "exit:" and "StoreCreditDisplay:Update+51:" are labels.  The
    // colon may be followed by blanks and/or a trailing comment.
    for (size_t i = 0; i < len; ++i) {
        if ((body[i] != ':') || ((i + 1 < len) && (body[i + 1] == ':'))) {
            continue;
        }
        size_t j = i + 1;
        while ((j < len) && ((body[j] == ' ') || (body[j] == '\t'))) {
            ++j;
        }
        if ((j >= len) || (body[j] == ';') ||
            ((j + 1 < len) && (((body[j] == '/') && (body[j + 1] == '/')) ||
                               ((body[j] == '-') && (body[j + 1] == '-'))))) {
            return 0;
        }
    }
    return 1;
}

// ----------------------------------------------------------------------------

bool CeFormat_Document(void) {
    if (!CeScript_IsActive()) {
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

    const char* const eol = CeScript_EolStr();
    size_t const eolLen = strlen(eol);

    CE_BUF out = { 0 };
    if (!CeBuf_Init(&out, (size_t)doc.len + 256)) {
        FreeMem(modes);
        CeScript_FreeDoc(&doc);
        return false;
    }

    char indentUnit[40];
    int const unitLen = CeSettings_IndentUnit(indentUnit, COUNTOF(indentUnit));
    bool inLuaLong = false;
    bool inCBlock = false;
    char line[CE_MAX_LINE_LEN];
    char body[CE_MAX_LINE_LEN];
    char operand[CE_MAX_LINE_LEN];

    for (DocLn l = 0; l < doc.lines; ++l) {
        DocPos n = 0;
        const char* const p = CeScript_LinePtr(&doc, l, &n);
        bool const tooLong = (n >= CE_MAX_LINE_LEN - 1);
        size_t const copyLen = tooLong ? (CE_MAX_LINE_LEN - 1) : (size_t)n;
        memcpy(line, p, copyLen);
        line[copyLen] = '\0';

        // Multi-line strings/comments are copied verbatim.
        if (inLuaLong) {
            CeBuf_Cat(&out, line, copyLen);
            CeBuf_Cat(&out, eol, eolLen);
            if (_Contains(line, copyLen, "]]")) {
                inLuaLong = false;
            }
            continue;
        }
        if (inCBlock) {
            CeBuf_Cat(&out, line, copyLen);
            CeBuf_Cat(&out, eol, eolLen);
            if (_Contains(line, copyLen, "*/")) {
                inCBlock = false;
            }
            continue;
        }

        CE_LANG const mode = (CE_LANG)modes[l];
        if (mode == CE_LANG_COMMENT) {
            // "{ ... }" is a comment block: keep its contents verbatim.
            size_t len = copyLen;
            while (len > 0 && _IsBlank(line[len - 1])) {
                --len;
            }
            CeBuf_Cat(&out, line, len);
            CeBuf_Cat(&out, eol, eolLen);
            continue;
        }
        if (tooLong || copyLen == 0) {
            // Blank line (or an unusually long line): keep it, minus blanks.
            size_t len = copyLen;
            while (len > 0 && _IsBlank(line[len - 1])) {
                --len;
            }
            if (len > 0) {
                CeBuf_Cat(&out, line, len);
            }
            CeBuf_Cat(&out, eol, eolLen);
            continue;
        }

        size_t first = 0;
        while (first < copyLen && _IsBlank(line[first])) {
            ++first;
        }

        if (mode == CE_LANG_AA) {
            size_t const used = _CollapseBlanks(line + first, copyLen - first, body, COUNTOF(body));
            if (used == 0) {
                CeBuf_Cat(&out, eol, eolLen);
                continue;
            }
            int const levels = _AaIndentLevels(body, used);
            for (int i = 0; i < levels; ++i) {
                CeBuf_Cat(&out, indentUnit, (size_t)unitLen);
            }
            size_t const clean = _StripAsmBracketBlanks(body, used, operand, COUNTOF(operand));
            CeBuf_Cat(&out, operand, clean);
            CeBuf_Cat(&out, eol, eolLen);
            continue;
        }

        if (mode == CE_LANG_C) {
            size_t len = copyLen;
            while (len > 0 && _IsBlank(line[len - 1])) {
                --len;
            }
            CeBuf_Cat(&out, line, len);
            CeBuf_Cat(&out, eol, eolLen);
            if (!_Contains(line, copyLen, "*/") && _Contains(line, copyLen, "/*")) {
                inCBlock = true;
            }
            continue;
        }

        // Lua: keep the original leading indentation (only Auto Assembler
        // lines are indented in a CE script); inner blanks are still collapsed
        // for tidiness, but the line's existing structure is preserved.
        size_t const used = _CollapseBlanks(line + first, copyLen - first, body, COUNTOF(body));
        if (used == 0) {
            CeBuf_Cat(&out, eol, eolLen);
            continue;
        }
        CeBuf_Cat(&out, line, first);   // preserve original leading whitespace
        CeBuf_Cat(&out, body, used);
        CeBuf_Cat(&out, eol, eolLen);

        if (!_Contains(line, copyLen, "]]")) {
            if (_Contains(line, copyLen, "--[[")) {
                inLuaLong = true;
            }
            else if (_Contains(line, copyLen, "[[")) {
                inLuaLong = true;
            }
        }
    }

    // Every line (including the trailing empty one CE_DOC adds for a file that
    // ends with an EOL) is emitted with a line break.  Drop the one extra break
    // so the document keeps its original trailing-newline shape; otherwise
    // each save would append another blank line.
    if (out.data && (out.len >= eolLen) &&
        (memcmp(out.data + out.len - eolLen, eol, eolLen) == 0)) {
        out.len -= eolLen;
        out.data[out.len] = '\0';
    }

    bool changed = false;
    if (out.data && out.len > 0) {
        // Compare against the current document to avoid a pointless undo step.
        if ((DocPos)out.len != doc.len || memcmp(out.data, doc.text, (size_t)doc.len) != 0) {
            CeScript_ReplaceAll(out.data);
            changed = true;
        }
    }

    CeBuf_Free(&out);
    FreeMem(modes);
    CeScript_FreeDoc(&doc);
    return changed;
}

// ----------------------------------------------------------------------------

bool CeFormat_StripAsmSpaces(void) {
    if (!CeScript_IsActive()) {
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

    CE_BUF out = { 0 };
    if (!CeBuf_Init(&out, (size_t)doc.len + 256)) {
        FreeMem(modes);
        CeScript_FreeDoc(&doc);
        return false;
    }

    char operand[CE_MAX_LINE_LEN];
    for (DocLn l = 0; l < doc.lines; ++l) {
        DocPos n = 0;
        const char* const p = CeScript_LinePtr(&doc, l, &n);
        DocPos const lineStart = doc.starts[l];
        // LinePtr() strips the EOL bytes; keep them verbatim so the saved text
        // differs from the buffer only where an operand was actually cleaned.
        DocPos const eolLen = doc.starts[l + 1] - lineStart - n;
        if ((modes[l] == (unsigned char)CE_LANG_AA) && (n > 0) && (n < CE_MAX_LINE_LEN - 1)) {
            size_t const clean = _StripAsmBracketBlanks(p, (size_t)n, operand, COUNTOF(operand));
            CeBuf_Cat(&out, operand, clean);
        }
        else {
            CeBuf_Cat(&out, p, (size_t)n);
        }
        if (eolLen > 0) {
            CeBuf_Cat(&out, doc.text + lineStart + n, (size_t)eolLen);
        }
    }

    bool changed = false;
    if (out.data && ((DocPos)out.len != doc.len ||
                     (memcmp(out.data, doc.text, (size_t)doc.len) != 0))) {
        CeScript_ReplaceAll(out.data);
        changed = true;
    }

    CeBuf_Free(&out);
    FreeMem(modes);
    CeScript_FreeDoc(&doc);
    return changed;
}
