// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspPos.c                                                                   *
*   LSP position / URI helpers.  See CeLspPos.h.                               *
*                                                                             *
*******************************************************************************/

#define _CRT_SECURE_NO_WARNINGS

#include "CeLspPos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Decode one UTF-8 code point.  Invalid bytes are treated as single-byte
// characters so a partially typed / mixed-encoding document never crashes us.
static size_t _Utf8Next(const unsigned char* p, size_t left, unsigned int* cp)
{
    const unsigned char b0 = p[0];
    if (b0 < 0x80u) {
        *cp = b0;
        return 1u;
    }
    const size_t need = ((b0 & 0xE0u) == 0xC0u) ? 2u :
                        ((b0 & 0xF0u) == 0xE0u) ? 3u :
                        ((b0 & 0xF8u) == 0xF0u) ? 4u : 1u;
    if ((need == 1u) || (left < need)) {
        *cp = b0;
        return 1u;
    }
    for (size_t i = 1u; i < need; ++i) {
        if ((p[i] & 0xC0u) != 0x80u) {
            *cp = b0;
            return 1u;
        }
    }
    unsigned int value = b0 & (unsigned char)(0xFFu >> (need + 1u));
    for (size_t i = 1u; i < need; ++i) {
        value = (value << 6) | (unsigned int)(p[i] & 0x3Fu);
    }
    *cp = value;
    return need;
}

static int _CharUnits(unsigned int cp, bool utf16)
{
    if (!utf16) {
        return 1;
    }
    return (cp > 0xFFFFu) ? 2 : 1;
}

void CeLspPos_FromOffset(const char* text, size_t length, size_t offset,
                         bool utf16, int* line, int* character)
{
    int ln = 0;
    int col = 0;
    if (text && (offset > length)) {
        offset = length;
    }
    size_t i = 0u;
    while (text && (i < offset)) {
        const unsigned char ch = (unsigned char)text[i];
        if (ch == '\n') {
            ++ln;
            col = 0;
            ++i;
            continue;
        }
        unsigned int cp = 0u;
        const size_t used = _Utf8Next((const unsigned char*)text + i, length - i, &cp);
        col += _CharUnits(cp, utf16);
        i += used;
    }
    if (line) { *line = ln; }
    if (character) { *character = col; }
}

size_t CeLspPos_ToOffset(const char* text, size_t length, int line, int character, bool utf16)
{
    if (!text) {
        return 0u;
    }
    if (line < 0) { line = 0; }
    if (character < 0) { character = 0; }
    size_t i = 0u;
    int ln = 0;
    while ((i < length) && (ln < line)) {
        if (text[i] == '\n') {
            ++ln;
        }
        ++i;
    }
    int col = 0;
    while ((i < length) && (col < character)) {
        const unsigned char ch = (unsigned char)text[i];
        if (ch == '\n') {
            break;
        }
        unsigned int cp = 0u;
        const size_t used = _Utf8Next((const unsigned char*)text + i, length - i, &cp);
        col += _CharUnits(cp, utf16);
        i += used;
    }
    return i;
}

bool CeLspPos_IsIdentChar(unsigned char ch)
{
    return ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) ||
           ((ch >= '0') && (ch <= '9')) || (ch == '_') || (ch == '.') ||
           (ch == '$') || (ch >= 0x80u);
}

size_t CeLspPos_WordStart(const char* text, size_t length, size_t offset)
{
    if (!text || (offset > length)) {
        return 0u;
    }
    size_t start = offset;
    while ((start > 0u) && CeLspPos_IsIdentChar((unsigned char)text[start - 1u])) {
        --start;
    }
    return start;
}

size_t CeLspPos_WordEnd(const char* text, size_t length, size_t offset)
{
    if (!text || (offset > length)) {
        return 0u;
    }
    size_t end = offset;
    while ((end < length) && CeLspPos_IsIdentChar((unsigned char)text[end])) {
        ++end;
    }
    return end;
}

int CeLspPos_SectionOpenerWidth(char left2, char left1, char first)
{
    if ((left2 == '{') && (left1 == '$')) {
        return 2; // word start is right after "{$"
    }
    if ((left1 == '{') && (first == '$')) {
        return 1; // '$' was scanned as part of the word
    }
    return 0;
}

// ----------------------------------------------------------------------------
// file URI codec
// ----------------------------------------------------------------------------

static bool _UriSafe(unsigned char ch)
{
    return ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) ||
           ((ch >= '0') && (ch <= '9')) ||
           (ch == '-') || (ch == '.') || (ch == '_') || (ch == '~') ||
           (ch == '/') || (ch == ':');
}

static bool _Append(char** buf, size_t* len, size_t* cap, const char* src, size_t n)
{
    if (*len + n + 1u > *cap) {
        size_t newCap = (*cap == 0u) ? 256u : *cap;
        while (newCap < (*len + n + 1u)) {
            newCap *= 2u;
        }
        char* const grown = (char*)realloc(*buf, newCap);
        if (!grown) {
            return false;
        }
        *buf = grown;
        *cap = newCap;
    }
    if (n > 0u) {
        memcpy(*buf + *len, src, n);
        *len += n;
    }
    (*buf)[*len] = '\0';
    return true;
}

char* CeLspPos_UriFromPath(const char* utf8Path)
{
    if (!utf8Path) {
        return NULL;
    }
    const bool unc = (utf8Path[0] == '\\') && (utf8Path[1] == '\\');
    const char* p = utf8Path + (unc ? 2u : 0u);
    char* out = NULL;
    size_t len = 0u;
    size_t cap = 0u;
    if (!_Append(&out, &len, &cap, unc ? "file://" : "file:///", unc ? 7u : 8u)) {
        free(out);
        return NULL;
    }
    for (; *p; ++p) {
        const unsigned char ch = (unsigned char)*p;
        if (ch == '\\') {
            if (!_Append(&out, &len, &cap, "/", 1u)) { break; }
        }
        else if (_UriSafe(ch)) {
            if (!_Append(&out, &len, &cap, (const char*)&ch, 1u)) { break; }
        }
        else {
            char esc[4];
            const int n = snprintf(esc, sizeof(esc), "%%%02X", (unsigned int)ch);
            if ((n <= 0) || !_Append(&out, &len, &cap, esc, (size_t)n)) { break; }
        }
    }
    if (!out || (*p != '\0')) {
        free(out);
        return NULL;
    }
    return out;
}

static int _HexVal(char ch)
{
    if ((ch >= '0') && (ch <= '9')) { return ch - '0'; }
    if ((ch >= 'a') && (ch <= 'f')) { return ch - 'a' + 10; }
    if ((ch >= 'A') && (ch <= 'F')) { return ch - 'A' + 10; }
    return -1;
}

bool CeLspPos_PathFromUri(const char* uri, char* out, size_t capacity)
{
    if (!uri || !out || (capacity == 0u) || (strncmp(uri, "file:", 5u) != 0)) {
        return false;
    }
    const char* p = uri + 5u;
    size_t n = 0u;
    size_t slashes = 0u;
    while (p[slashes] == '/') { ++slashes; }
    if (slashes >= 2u) {
        // "file://server/share" is UNC: restore the two leading backslashes.
        if (n + 2u >= capacity) {
            return false;
        }
        out[n++] = '\\';
        out[n++] = '\\';
    }
    p += slashes;
    while (*p) {
        if (n + 1u >= capacity) {
            return false;
        }
        if ((*p == '%') && (p[1] != '\0') && (p[2] != '\0')) {
            const int hi = _HexVal(p[1]);
            const int lo = _HexVal(p[2]);
            if ((hi >= 0) && (lo >= 0)) {
                out[n++] = (char)((hi << 4) | lo);
                p += 3;
                continue;
            }
        }
        out[n++] = (*p == '/') ? '\\' : *p;
        ++p;
    }
    out[n] = '\0';
    return true;
}
