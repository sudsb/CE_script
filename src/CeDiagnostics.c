// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeDiagnostics.c                                                             *
*   Inline diagnostics for Cheat Engine scripts.                              *
*                                                                             *
*   Rendering rules:                                                          *
*   - the offending token gets a coloured squiggle (error: red, warning:      *
*     amber) so the position is obvious without adding any line height;      *
*   - the short message is drawn as ghost text at the end of that line using  *
*     a Scintilla EOL annotation (5.5+), styled by two dedicated extended     *
*     styles.  It never covers the code, never steals focus and disappears    *
*     with a single Clear().                                                  *
*                                                                             *
*   Everything is re-applied from the UI thread after the caller collected a  *
*   full hint set, so partially updated diagnostics are never painted.        *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <string.h>

#include "SciCall.h"
#include "CeDiagnostics.h"

// Muted colours: readable on both the light and the dark default themes.
#define CE_DIAG_ERR_COLOR   RGB(0xD0, 0x4A, 0x4A)
#define CE_DIAG_WARN_COLOR  RGB(0xB8, 0x86, 0x00)

typedef struct _ce_diag_hint {
    DocLn line;      // 0-based
    int   startCol;  // 0-based byte column, < 0 = whole line
    int   endCol;    // exclusive, <= startCol = whole line
    bool  error;
    char  text[CE_DIAG_TEXT_CCH];
} CE_DIAG_HINT;

static CE_DIAG_HINT s_hints[CE_DIAG_MAX_HINTS];
static int  s_count = 0;
static bool s_collecting = false;
static bool s_drawn = false;
static bool s_initialized = false;
static int  s_styleBase = -1;   // extended styles: +0 error, +1 warning

// ----------------------------------------------------------------------------

static void _ConfigureIndicators(void) {
    SciCall_IndicSetStyle(INDIC_NP3_CE_DIAG_ERROR, INDIC_SQUIGGLE);
    SciCall_IndicSetFore(INDIC_NP3_CE_DIAG_ERROR, CE_DIAG_ERR_COLOR);
    SciCall_IndicSetStyle(INDIC_NP3_CE_DIAG_WARNING, INDIC_SQUIGGLE);
    SciCall_IndicSetFore(INDIC_NP3_CE_DIAG_WARNING, CE_DIAG_WARN_COLOR);
}

// StyleClearAll() (called by Style_SetLexer) resets the font/size/colour of
// every style, extended ones included.  Re-applying them is cheap and keeps
// the ghost text matching the current theme and zoom level.
static void _ConfigureStyles(void) {
    if (s_styleBase < 0) {
        return;
    }
    char font[128] = { '\0' };
    SciCall_StyleGetFont(STYLE_DEFAULT, font);
    int const size = SciCall_StyleGetSize(STYLE_DEFAULT);
    COLORREF const back = SciCall_StyleGetBack(STYLE_DEFAULT);

    for (int i = 0; i < 2; ++i) {
        int const style = s_styleBase + i;
        if (font[0] != '\0') {
            SciCall_StyleSetFont(style, font);
        }
        if (size > 0) {
            SciCall_StyleSetSize(style, size);
        }
        SciCall_StyleSetBack(style, back);
        SciCall_StyleSetBold(style, false);
        SciCall_StyleSetItalic(style, false);
    }
    SciCall_StyleSetFore(s_styleBase + 0, CE_DIAG_ERR_COLOR);
    SciCall_StyleSetFore(s_styleBase + 1, CE_DIAG_WARN_COLOR);
    SciCall_EOLAnnotationSetStyleOffset(s_styleBase);
}

void CeDiagnostics_RefreshStyles(void) {
    if (!s_initialized || (s_styleBase < 0)) {
        return;
    }
    // StyleClearAll() copies STYLE_DEFAULT over every style, extended ones
    // included.  The eolAnnotationStyleOffset survives, but the two colours
    // do not: after a theme switch the error text would be painted in the
    // default colour until the findings happened to change.
    if (SciCall_StyleGetFore(s_styleBase) != CE_DIAG_ERR_COLOR) {
        _ConfigureStyles();
    }
}

// Only one ghost text fits on a line.  Errors take precedence over warnings
// even when the warning was collected first (the hint set is ordered by line,
// not by severity); equal severities keep the first hint.
static bool _IsPrimaryHint(const CE_DIAG_HINT* hints, int count, int index) {
    if (!hints || (index <= 0) || (index >= count)) {
        return true;
    }
    CE_DIAG_HINT const* const hint = &hints[index];
    for (int j = 0; j < index; ++j) {
        if (hints[j].line != hint->line) {
            continue;
        }
        if (hints[j].error || !hint->error) {
            return false;
        }
    }
    return true;
}

static const char* _FallbackText(bool error) {
    return error ? "错误" : "警告";
}

// Append one finding to the line's ghost text, separated by " | ".  A cut is
// pulled back to a UTF-8 lead byte so the annotation never contains a broken
// multibyte sequence (which Scintilla renders as an empty box - or nothing).
static void _AppendDiagText(char* dst, size_t cchDst, const char* src, bool first) {
    if (!dst || (cchDst == 0) || !src) {
        return;
    }
    size_t n = strlen(dst);
    if (!first) {
        if ((n + 4) >= cchDst) {
            return;
        }
        dst[n++] = ' ';
        dst[n++] = '|';
        dst[n++] = ' ';
        dst[n] = '\0';
    }
    while ((n + 1 < cchDst) && (*src != '\0') && (*src != '\r') && (*src != '\n')) {
        dst[n++] = *src++;
    }
    dst[n] = '\0';
    size_t lead = n;
    while ((lead > 0) && (((unsigned char)dst[lead - 1] & 0xC0) == 0x80)) {
        --lead;
    }
    if (lead > 0) {
        unsigned char const b = (unsigned char)dst[lead - 1];
        size_t need = 1;
        if ((b & 0xE0) == 0xC0) {
            need = 2;
        }
        else if ((b & 0xF0) == 0xE0) {
            need = 3;
        }
        else if ((b & 0xF8) == 0xF0) {
            need = 4;
        }
        if (((lead - 1) + need) > n) {
            dst[lead - 1] = '\0';
        }
    }
}

// Every squiggle on a line must be explained.  The old renderer showed only
// the primary hint's text and silently dropped the others, which looked like
// "an error is shown but no message".  Combine up to three findings per line.
static void _MergeLineMessage(const CE_DIAG_HINT* hints, int count, int primary,
                              char* out, size_t cchOut) {
    if (!hints || !out || (cchOut == 0) || (primary < 0) || (primary >= count)) {
        return;
    }
    out[0] = '\0';
    CE_DIAG_HINT const* const main = &hints[primary];
    _AppendDiagText(out, cchOut,
                    main->text[0] ? main->text : _FallbackText(main->error), true);
    int shown = 1;
    for (int j = 0; (j < count) && (shown < 3); ++j) {
        if ((j == primary) || (hints[j].line != main->line)) {
            continue;
        }
        _AppendDiagText(out, cchOut,
                        hints[j].text[0] ? hints[j].text : _FallbackText(hints[j].error), false);
        ++shown;
    }
}

// Keep the ghost text short and valid UTF-8 even when a caller passes a long
// message: a multi-byte sequence cut by the length limit is removed as a whole.
static void _CopyShortText(char* dst, size_t cchDst, const char* src) {
    if (!dst || cchDst == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    size_t n = 0;
    while (src[n] != '\0' && n + 1 < cchDst) {
        char const ch = src[n];
        if (ch == '\r' || ch == '\n') {
            break;
        }
        dst[n] = ch;
        ++n;
    }
    // Walk back over continuation bytes to the lead byte of the last sequence
    // and drop the whole sequence when it did not fit.
    size_t lead = n;
    while ((lead > 0) && (((unsigned char)dst[lead - 1] & 0xC0) == 0x80)) {
        --lead;
    }
    if (lead > 0) {
        unsigned char const b = (unsigned char)dst[lead - 1];
        size_t need = 1;
        if ((b & 0xE0) == 0xC0) {
            need = 2;
        }
        else if ((b & 0xF0) == 0xE0) {
            need = 3;
        }
        else if ((b & 0xF8) == 0xF0) {
            need = 4;
        }
        if (((lead - 1) + need) > n) {
            n = lead - 1;
        }
    }
    dst[n] = '\0';
}

static void _ClearMarks(void) {
    DocPos const len = SciCall_GetTextLength();
    SciCall_SetIndicatorCurrent(INDIC_NP3_CE_DIAG_ERROR);
    SciCall_IndicatorClearRange(0, len);
    SciCall_SetIndicatorCurrent(INDIC_NP3_CE_DIAG_WARNING);
    SciCall_IndicatorClearRange(0, len);
    SciCall_EOLAnnotationClearAll();
    s_drawn = false;
}

// ----------------------------------------------------------------------------

void CeDiagnostics_Init(void) {
    if (s_styleBase < 0) {
        s_styleBase = SciCall_AllocateExtendedStyles(2);
    }
    s_initialized = true;
    s_count = 0;
    s_collecting = false;
    s_drawn = false;
    _ConfigureIndicators();
    _ConfigureStyles();
    SciCall_EOLAnnotationSetVisible(EOLANNOTATION_STANDARD);
}

void CeDiagnostics_Clear(void) {
    if (!s_initialized) {
        s_count = 0;
        s_collecting = false;
        s_drawn = false;
        return;
    }
    // Always clear.  Scintilla keeps EOL annotations per document line, so a
    // stale annotation from a previous edit can survive when our bookkeeping
    // says "nothing drawn" - that is exactly the "yellow bar with no text on
    // a line that has no error" symptom.
    _ClearMarks();
    s_count = 0;
    s_collecting = false;
}

void CeDiagnostics_Begin(void) {
    s_count = 0;
    s_collecting = true;
}

void CeDiagnostics_AddLine(DocLn line, bool error, const char* utf8Text) {
    CeDiagnostics_Add(line, -1, -1, error, utf8Text);
}

void CeDiagnostics_Add(DocLn line, int startCol, int endCol, bool error, const char* utf8Text) {
    if (!s_collecting || (s_count >= CE_DIAG_MAX_HINTS) || (line < 0)) {
        return;
    }
    CE_DIAG_HINT* const hint = &s_hints[s_count++];
    hint->line = line;
    hint->startCol = startCol;
    hint->endCol = endCol;
    hint->error = error;
    _CopyShortText(hint->text, COUNTOF(hint->text), utf8Text);
    if (hint->text[0] == '\0') {
        StringCchCopyA(hint->text, COUNTOF(hint->text), _FallbackText(error));
    }
}

void CeDiagnostics_End(void) {
    if (!s_collecting) {
        return;
    }
    s_collecting = false;
    if (!s_initialized) {
        s_count = 0;
        return;
    }
    _ClearMarks();
    if (s_count == 0) {
        return;
    }
    // Style_SetLexer()/StyleClearAll() discards indicator attributes just like
    // it discards style fonts, so both are re-applied with every update.
    _ConfigureIndicators();
    _ConfigureStyles();

    DocLn const lineCount = SciCall_GetLineCount();
    for (int i = 0; i < s_count; ++i) {
        CE_DIAG_HINT const* const hint = &s_hints[i];
        if (hint->line >= lineCount) {
            continue;
        }
        DocPos const lineStart = SciCall_PositionFromLine(hint->line);
        DocPos const lineEnd = SciCall_GetLineEndPosition(hint->line);

        // One ghost text per line, written by the first hint of that line.
        // An error wins the colour; every finding on the line contributes its
        // message, so no squiggle is left unexplained.
        bool firstForLine = true;
        for (int j = 0; j < i; ++j) {
            if (s_hints[j].line == hint->line) {
                firstForLine = false;
                break;
            }
        }
        if (firstForLine && _IsPrimaryHint(s_hints, s_count, i)) {
            int primary = i;
            for (int j = 0; j < s_count; ++j) {
                if ((s_hints[j].line == hint->line) && s_hints[j].error) {
                    primary = j;
                    break;
                }
            }
            char merged[CE_DIAG_TEXT_CCH * 3];
            _MergeLineMessage(s_hints, s_count, primary, merged, COUNTOF(merged));
            SciCall_EOLAnnotationSetText(hint->line, merged);
            SciCall_EOLAnnotationSetStyle(hint->line, s_hints[primary].error ? 0 : 1);
        }

        // An empty line has no token to underline, but its ghost text above is
        // still useful (findings are reported on the line after a closing brace).
        if (lineEnd <= lineStart) {
            continue;
        }

        DocPos start = lineStart;
        DocPos end = lineEnd;
        if ((hint->startCol >= 0) && (hint->endCol > hint->startCol)) {
            start = lineStart + (DocPos)hint->startCol;
            end = lineStart + (DocPos)hint->endCol;
        }
        if (start < lineStart) {
            start = lineStart;
        }
        // A column computed from an older document state may sit past the
        // current line end; never let the squiggle wrap onto the next line.
        if (start > lineEnd) {
            start = lineEnd;
        }
        if (end <= start) {
            end = (start < lineEnd) ? (start + 1) : lineEnd;
        }
        if (end > lineEnd) {
            end = lineEnd;
        }
        if (end <= start) {
            continue;
        }
        SciCall_SetIndicatorCurrent(hint->error ? INDIC_NP3_CE_DIAG_ERROR
                                                : INDIC_NP3_CE_DIAG_WARNING);
        SciCall_IndicatorFillRange(start, end - start);
    }

    SciCall_EOLAnnotationSetVisible(EOLANNOTATION_STANDARD);
    s_drawn = true;
}
