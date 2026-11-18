// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspPos.h                                                                   *
*   LSP Position / Range helpers and file-URI codec.                           *
*                                                                             *
*   Scintilla documents are UTF-8 byte buffers.  LSP positions are            *
*   (line, character) pairs in either UTF-8 code points or UTF-16 code        *
*   units, selected during initialize.  These functions are pure ISO C so     *
*   both the client and the built-in server use exactly the same arithmetic.   *
*                                                                             *
*******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Byte offset -> LSP position.  |utf16| selects UTF-16 code units, otherwise
// UTF-8 code points (our built-in server negotiates "utf-8").
void   CeLspPos_FromOffset(const char* text, size_t length, size_t offset,
                           bool utf16, int* line, int* character);
// LSP position -> byte offset (clamped to the document).
size_t CeLspPos_ToOffset(const char* text, size_t length, int line, int character, bool utf16);

// Word helpers shared by completion, hover and signatureHelp.
bool   CeLspPos_IsIdentChar(unsigned char ch);
size_t CeLspPos_WordStart(const char* text, size_t length, size_t offset);
size_t CeLspPos_WordEnd(const char* text, size_t length, size_t offset);

// Number of extra characters that belong to a CE state-switch completion root.
// |left2| / |left1| are the two bytes before the word start and |first| is the
// byte at the word start.  "{$" is not identifier shaped and '$' is a word
// character in some Scintilla word sets, so WordStart() may stop either right
// after the opener or right after '{'.  Returns 2, 1 or 0; the caller subtracts
// the result from its word-start position so the accepted item replaces the
// whole "{$" opener instead of leaving a stray '{' behind.
int    CeLspPos_SectionOpenerWidth(char left2, char left1, char first);

// UTF-8 Windows path -> "file://" URI (malloc'd, free with free()).
char*  CeLspPos_UriFromPath(const char* utf8Path);
// Reverse: percent-decode a file URI into a UTF-8 path.  Returns false when
// the URI is not a file URI or the output buffer is too small.
bool   CeLspPos_PathFromUri(const char* uri, char* out, size_t capacity);

#ifdef __cplusplus
}
#endif
