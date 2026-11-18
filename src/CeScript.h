// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeScript.h                                                                  *
*   Shared document model for the Cheat Engine script tooling.                *
*                                                                             *
*   Everything works on the UTF-8 document buffer exposed by Scintilla, so    *
*   the lint, formatter, template generator and CE table converter all see    *
*   exactly the same text the user does, without extra copies.                *
*                                                                             *
*******************************************************************************/

#pragma once

#ifndef _CE_SCRIPT_H_
#define _CE_SCRIPT_H_

#include "TypeDefs.h"

#define CE_MAX_LINE_LEN     4096
#define CE_MAX_DEFS         256
#define CE_MAX_SECTIONS     32

typedef enum _ce_mode {
    CE_LANG_LUA = 0,
    CE_LANG_AA,
    CE_LANG_C,
    CE_LANG_COMMENT // line belongs to an Auto Assembler "{ ... }" block comment
} CE_LANG;

typedef struct _ce_doc {
    // INVARIANT: borrowed Scintilla buffer; valid only until the document is
    // modified or Scintilla otherwise moves/reallocates it.  Do not call any
    // SciCall_* mutator (including ones that trigger notifications) while a
    // CE_DOC is live.  Free the doc first, then edit.
    const char* text;   // UTF-8 buffer owned by Scintilla, never freed here
    DocPos      len;    // must still equal Sci_GetDocEndPosition() at FreeDoc
    DocLn       lines;
    DocPos*     starts; // lines + 1 offsets, starts[lines] == len
} CE_DOC;

typedef struct _ce_section {
    bool    enable;     // [ENABLE] vs [DISABLE]
    DocLn   line;       // 0-based marker line
    DocPos  start;      // offset of '['
    DocPos  bodyStart;  // first character after the marker line
    DocPos  end;        // offset of the next marker (or document end)
} CE_SECTION;

typedef struct _ce_def {
    char   name[64];
    char   kind[16];    // alloc / globalalloc / define / label / lua / assign / label:
    DocLn  line;        // 1-based, for diagnostics
} CE_DEF;

// True when the active document uses the Cheat Engine scheme.
bool CeScript_IsActive(void);

// Load the current Scintilla document.  Returns false when the editor is not
// available or the document is empty.
bool CeScript_InitDoc(CE_DOC* doc);
void CeScript_FreeDoc(CE_DOC* doc);

// Line helpers.  Line numbers are 0-based unless the name says otherwise.
DocLn CeScript_LineFromPos(const CE_DOC* doc, DocPos pos);
const char* CeScript_LinePtr(const CE_DOC* doc, DocLn line, DocPos* len);
// Returns a pointer to the first non-blank character of the line.
const char* CeScript_Trimmed(const CE_DOC* doc, DocLn line, DocPos* len);

// 0-based line -> 1-based line number used in messages.
#define CE_LINE_NO(line) ((int)(line) + 1)

// Fill modes[0..lines) with the language mode in effect on each line.  The
// caller owns the array (one byte per line).
void CeScript_LineModes(const CE_DOC* doc, unsigned char* modes);

// Language mode in effect on the caret line.  Returns CE_LANG_LUA when the CE
// scheme is not active or the document is empty.
CE_LANG CeScript_LangAtCaret(void);

// Track multi-line comments while scanning one line.  |block| is 0 (none),
// 1 (Lua --[[ ... ]]), 2 (C /* ... */) or 3 (AA "{ ... }"); it is updated for
// the next line.
void CeScript_UpdateBlockComment(const char* p, const char* end, int mode, int* block);

// Collect [ENABLE]/[DISABLE] blocks in document order.
int CeScript_CollectSections(const CE_DOC* doc, CE_SECTION* out, int max);

// Strict "[ section ]" marker parsing, shared by the section model and the
// diagnostics.  A marker is a whole line containing only "[ identifier ]"
// plus optional trailing whitespace/comment.  Lua table constructor keys
// ("[1] = 'x'") and index expressions ("t[foo] = 1") are NOT markers.  The
// returned |name| has the surrounding blanks trimmed.
bool CeScript_SectionMarker(const char* p, size_t len, char* name, size_t cchName);

// True when a line is a declaration/counterpart statement of the given
// section kind.  Shared by the counterpart generator and context-menu
// declaration generation so both insert at the same place.
bool CeScript_IsDeclarationLine(const char* p, size_t len, bool enable);

// End offset of the last such line inside |sec| (before its EOL), or -1.
DocPos CeScript_DeclarationAnchor(const CE_DOC* doc, const CE_SECTION* sec, bool enable);

// Collect user-defined symbols (alloc/label/define/function/local/assign).
int CeScript_CollectDefs(const CE_DOC* doc, CE_DEF* out, int max);

// Return the index of |name| in |defs| (case-insensitive), or -1.
int CeScript_FindDef(const CE_DEF* defs, int count, const char* name);

// Case-insensitive helpers on a length-delimited buffer.
bool CeScript_StartsWithCI(const char* text, DocPos len, const char* prefix);
bool CeScript_EqualsCI(const char* text, DocPos len, const char* word);
const char* CeScript_SkipBlanks(const char* p, const char* end);

// Read an identifier at |p| (letters, digits, '_').  Returns the end pointer,
// or |p| when there is no identifier.
const char* CeScript_ReadIdent(const char* p, const char* end, char* out, size_t outCch);

// Replace the whole document as a single undoable action, keeping the caret
// as close to its old position as possible.
void CeScript_ReplaceAll(const char* text);

// Current document end-of-line sequence as a NUL terminated string.
const char* CeScript_EolStr(void);

// Small growable string used by the generators.  Always NUL terminated.
typedef struct _ce_buf {
    char*  data;
    size_t len;
    size_t cap;
} CE_BUF;

bool CeBuf_Init(CE_BUF* buf, size_t initialCap);
bool CeBuf_Cat(CE_BUF* buf, const char* text, size_t len);
bool CeBuf_CatZ(CE_BUF* buf, const char* text);
void CeBuf_Free(CE_BUF* buf);

// ----------------------------------------------------------------------------
// Lightweight Lua lexical scope model
//
// Shared by the diagnostics and the completion word list so both agree on what
// a name is and where it is visible.  This is a token-level approximation
// rather than a Lua parser: block keywords delimit scopes, and every symbol
// carries the offset of its declaring token plus the [start,end) range of the
// block owning it.  A name is visible when a declaration precedes the use and
// the use lies inside that declaration's block; globals are document-wide.
//
// The scanner is line based and its state lives entirely in CE_LUA_MODEL, so it
// can be driven without Scintilla (and is exercised directly by the tests).
// ----------------------------------------------------------------------------

#define CE_LUA_MAX_SYMS     768
#define CE_LUA_MAX_BLOCKS   64
#define CE_LUA_MAX_TOKENS   256

typedef enum _ce_lua_symkind {
    CE_LUA_SYM_NONE = 0,
    CE_LUA_SYM_LOCAL,
    CE_LUA_SYM_PARAM,
    CE_LUA_SYM_LOCALFN,    // "local function f(...)" — block-local function
    CE_LUA_SYM_GLOBAL,
    CE_LUA_SYM_FUNCTION,
    CE_LUA_SYM_FIELD,      // table key / field: not a variable in scope
} CE_LUA_SYMKIND;

typedef struct _ce_lua_symbol {
    char   name[64];
    int    kind;
    DocPos pos;            // absolute offset of the declaring token
    DocPos scopeStart;     // absolute offset of the owning block's opener
    DocPos scopeEnd;       // exclusive; -1 while the block is still open
    DocLn  line;           // 1-based declaration line
} CE_LUA_SYMBOL;

typedef enum _ce_lua_pending {
    CE_LUA_P_NONE = 0,
    CE_LUA_P_LOCAL,        // after "local": collecting declared names
    CE_LUA_P_FOR,          // after "for": collecting loop variables
    CE_LUA_P_FUNCNAME,     // after "function": collecting the (dotted) name
    CE_LUA_P_PARAMS,       // inside a function signature's "(" ... ")"
} CE_LUA_PENDING;

typedef struct _ce_lua_model {
    CE_LUA_SYMBOL syms[CE_LUA_MAX_SYMS];
    int           count;
    bool          truncated;   // capacity hit: callers must stay conservative

    int           blockTop;
    int           blockUsed;   // high-water mark of blocks ever opened
    DocPos        blockStart[CE_LUA_MAX_BLOCKS];
    DocPos        blockEnd[CE_LUA_MAX_BLOCKS]; // exclusive; -1 while open
    int           blockFirstSym[CE_LUA_MAX_BLOCKS];
    bool          skipDo;      // "do" of for/while must not open a second block
    int           tableDepth;

    int           pending;
    int           pendingKind; // kind to use for a pending function name
    int           paramDepth;
    const char*   funcNameText;
    size_t        funcNameLen;
    DocPos        funcNamePos;

    int           longLevel;   // -1 outside a --[=[ ]=] comment or [[ ]] string
} CE_LUA_MODEL;

void CeLuaScope_Init(CE_LUA_MODEL* model);
// Feed one Lua line.  |lineBase| is the absolute offset of |p|, |line| is
// 1-based.  Callers skip AA/C lines; the scope state simply carries over.
void CeLuaScope_Line(CE_LUA_MODEL* model, const char* p, const char* end,
                     DocPos lineBase, DocLn line);
void CeLuaScope_Finish(CE_LUA_MODEL* model, DocPos docEnd);
// Symbol kind visible at |pos|, or CE_LUA_SYM_NONE.
int  CeLuaScope_Resolve(const CE_LUA_MODEL* model, const char* name, DocPos pos);
// True when |name| is declared as a local/parameter anywhere in the document.
bool CeLuaScope_DeclaredAnywhere(const CE_LUA_MODEL* model, const char* name);
// Number of Lua blocks (function/if/for/while/do/repeat) containing |pos|.
// Used to rank block-closing keywords in the completion list.
int  CeLuaScope_BlockDepthAt(const CE_LUA_MODEL* model, DocPos pos);

#endif // _CE_SCRIPT_H_
