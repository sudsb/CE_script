// encoding: UTF-8
// Unit tests for CE autocompletion: notification routing on Tab acceptance,
// post-completion decoration (auto parentheses / [symbol] wrapping), candidate
// filtering for a fully typed word, and template variable resolution.
// TestCeCompletion.ps1 extracts the relevant functions verbatim from
// src/CeCompletion.c / src/CeTemplate.c / src/Notepad3.c; everything else is
// stubbed here so the test needs neither Scintilla nor a running editor.

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define CE_TEMPLATE_BODY_CCH 1024

typedef unsigned int UINT;
typedef long long DocPos;
typedef void* HWND;

// Only distinct values matter: the extracted routing helper compares codes.
#define SCN_MODIFIED             2008
#define SCN_UPDATEUI             2010
#define SCN_AUTOCSELECTIONCHANGE 2014
#define SCN_AUTOCSELECTION       2015
#define SCN_AUTOCCOMPLETED       2016
#define SCN_AUTOCCANCELLED       2017
#define SCN_AUTOCCHARDELETED     2018

enum SciEventMask {
    EVM_None = 0,
    EVM_Default = 0x3FF
};

typedef struct CE_COMPLETION_ENTRY {
    const char* name;
    const char* signature;
    const char* category;
    int isCall;
    int kind;
} CE_COMPLETION_ENTRY;

typedef struct CE_COMPLETION_DOC {
    const char* name;
    const char* doc;
} CE_COMPLETION_DOC;

// Mirrors src/StyleLexers/ce_completions_data.h and src/CeScript.h.
#define CE_KIND_NONE     0
#define CE_KIND_KEYWORD  1
#define CE_KIND_FUNCTION 2
#define CE_KIND_VARIABLE 3
#define CE_KIND_AA       4
#define CE_KIND_ASM      5
#define CE_KIND_TYPE     6
#define CE_KIND_CONSTANT 7
#define CE_KIND_SECTION  8
#define CE_KIND_COUNT    9

enum { CE_LANG_LUA = 0, CE_LANG_AA = 1, CE_LANG_C = 2, CE_LANG_COMMENT = 3 };

// The extracted _InKeywordList() consults the init-time keyword sets.  They are
// empty here, so the function takes its linear fallback and the tests exercise
// the same matching semantics as before.
typedef struct CE_KW_SET {
    const char* list;
    char** words;
    int count;
} CE_KW_SET;
static CE_KW_SET s_kwSets[5];
static int s_kwSetCount = 0;

// ... and _FindDoc() also consults the optional 4th dictionary field.
static CE_COMPLETION_ENTRY s_dictionary[1];
static char* s_dictionaryDocs[1];
static int s_dictionaryCount = 0;

// Name-sorted (strcmp order), just like the generated g_ceCompletionDocs[].
// ASCII-only: this harness is compiled without /utf-8.
static const CE_COMPLETION_DOC g_ceCompletionDocs[] = {
    { "[ENABLE]", "section" },
    { "alloc", "alloc note" },
    { "mov", "copy note\nsyntax: mov dst, src" },
    { "registersymbol", "symbol note" },
};
static const int g_ceCompletionDocCount = COUNTOF(g_ceCompletionDocs);

typedef struct CE_TEMPLATE { const char* name; const wchar_t* section; } CE_TEMPLATE;
typedef struct CE_BUF { char* data; } CE_BUF;
typedef struct SETTINGS2 { CE_TEMPLATE CeTemplates[1]; } SETTINGS2;
static SETTINGS2 Settings2;

static const char CE_KEYWORDS_LUA[] =
    "and break do else end for function if in local nil not or repeat return "
    "then true until while print";
static const char CE_KEYWORDS_ASM[] = "mov jmp je jne ret nop add eax rax call push pop";
static const char CE_KEYWORDS_TYPES[] = "dword qword byte ptr";
static const char CE_KEYWORDS_API[] = "readInteger writeInteger autoAssemble AOBScan showMessage";
static const char CE_KEYWORDS_AA[] = "alloc dealloc label define registersymbol";

static inline bool StrIsEmptyA(const char* s) { return !s || !*s; }

static inline void StringCchCopyA(char* dst, size_t cch, const char* src) {
    if (cch == 0) { return; }
    size_t i = 0;
    for (; (i + 1 < cch) && src[i]; ++i) { dst[i] = src[i]; }
    dst[i] = '\0';
}

static inline void StringCchCopyNA(char* dst, size_t cch, const char* src, size_t count) {
    if (cch == 0) { return; }
    size_t const n = (count < cch - 1) ? count : cch - 1;
    for (size_t i = 0; i < n; ++i) { dst[i] = src[i]; }
    dst[n] = '\0';
}

static inline void StringCchPrintfA(char* dst, size_t cch, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dst, cch, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// Minimal Scintilla document simulation
// ---------------------------------------------------------------------------

static std::string g_text;
static DocPos g_caret = 0;
static DocPos g_anchor = 0;

static inline DocPos SciCall_GetCurrentPos() { return g_caret; }
static inline int SciCall_GetCharAt(DocPos pos) {
    if ((pos < 0) || (pos >= (DocPos)g_text.size())) { return 0; }
    return (unsigned char)g_text[(size_t)pos];
}
static inline void SciCall_SetSel(DocPos anchor, DocPos caret) {
    g_anchor = anchor;
    g_caret = caret;
}
static inline DocPos SciCall_GetSelectionStart() { return std::min(g_anchor, g_caret); }
static inline DocPos SciCall_GetSelectionEnd() { return std::max(g_anchor, g_caret); }
static inline void SciCall_ReplaceSel(const char* text) {
    DocPos const start = std::min(g_anchor, g_caret);
    DocPos const end = std::max(g_anchor, g_caret);
    g_text.replace((size_t)start, (size_t)(end - start), text);
    g_caret = g_anchor = start + (DocPos)strlen(text);
}
static inline const char* SciCall_GetRangePointer(DocPos start, DocPos len) {
    if ((start < 0) || (len < 0) || ((start + len) > (DocPos)g_text.size())) { return nullptr; }
    return g_text.c_str() + start;
}
static inline void SciCall_BeginUndoAction() {}
static inline void SciCall_EndUndoAction() {}

// ---------------------------------------------------------------------------
// Configurable environment stubs
// ---------------------------------------------------------------------------

static bool g_ceActive = true;
static bool g_isAA = false;
static bool g_asmInsnLine = false;
static bool g_wrapAsm = true;
static bool g_autoArgs = false;
static bool g_isAutoArgFn = false;
static bool g_resolveVar = false;
static const char* g_resolvedVar = "";

static bool s_pendingCompletion = false;
static char s_lastHighlight[240] = { '\0' };

static const CE_COMPLETION_ENTRY s_catalogue[] = {
    { "alloc",          "alloc(name,size)",      "AA",  1, 8 },
    { "registersymbol", "registersymbol(name)",  "AA",  1, 8 },
    { "readInteger",    "readInteger(addr)",     "API", 1, 2 },
    { "mov",            "mov dst,src",           "ASM", 0, 5 },
    { "newmem",         "",                      "VAR", 0, 3 },
};

static const CE_COMPLETION_ENTRY* CeCompletion_Find(const char* name) {
    if (StrIsEmptyA(name)) { return nullptr; }
    for (const CE_COMPLETION_ENTRY& entry : s_catalogue) {
        if (strcmp(entry.name, name) == 0) { return &entry; }
    }
    return nullptr;
}

static bool InWordList(const char* list, const char* word) {
    std::string const hay = std::string(" ") + list + " ";
    std::string const needle = std::string(" ") + word + " ";
    return hay.find(needle) != std::string::npos;
}

static bool CeCompletion_IsBuiltinKeyword(const char* name) {
    if (StrIsEmptyA(name) || CeCompletion_Find(name)) { return true; }
    return InWordList(CE_KEYWORDS_LUA, name) || InWordList(CE_KEYWORDS_API, name) ||
           InWordList(CE_KEYWORDS_AA, name) || InWordList(CE_KEYWORDS_ASM, name) ||
           InWordList(CE_KEYWORDS_TYPES, name);
}

static bool CeCompletion_IsActive() { return g_ceActive; }
static bool _IsAaLineAtCaret() { return g_isAA; }
static bool _LineStartsWithAsmInsn(DocPos, char* insn, size_t cchInsn) {
    if (cchInsn > 0) { insn[0] = '\0'; }
    return g_asmInsnLine;
}
static bool CeSettings_WrapAsmSymbols() { return g_wrapAsm; }
static bool CeSettings_AutoArguments() { return g_autoArgs; }
static bool CeSettings_IsAutoArgFunction(const char*) { return g_isAutoArgFn; }
static bool CeTemplate_IsTrigger(const char*) { return false; }
static bool CeCompletion_ResolveVariable(char* out, size_t cch) {
    if (!g_resolveVar) { return false; }
    StringCchCopyA(out, cch, g_resolvedVar);
    return true;
}
static void _HideCeTip() {}
static bool _TryInsertCodeBlock(const char*) { return false; } // exercised by TestCeCodeBlock
static void _ShowCallTip(const CE_COMPLETION_ENTRY*, DocPos) {}

// --- Variable-source stubs.  The real _IsDefinedVariable() scans the document;
//     here it just records what the selection resolver passed in.
static bool g_definedVar = true;
static bool g_clipboardVar = false;
static char g_lastVarCandidate[480] = { '\0' };
static bool _IsDefinedVariable(char* text) {
    StringCchCopyA(g_lastVarCandidate, COUNTOF(g_lastVarCandidate), text);
    return g_definedVar;
}
static bool CeCompletion_GetClipboardVariable(char* out, size_t cch) {
    if (!g_clipboardVar) { return false; }
    StringCchCopyA(out, cch, "fromClipboard");
    return true;
}

// --- Template insertion stubs.  _RenderTemplate() records which variable the
//     template body would be rendered with.
static bool g_renderedHaveVar = false;
static char g_renderedVar[240] = { '\0' };
static bool CeScript_IsActive() { return true; }
static int CeTemplate_Count() { return 1; }
static bool CeBuf_Init(CE_BUF* out, size_t) { out->data = nullptr; return true; }
static void CeBuf_Free(CE_BUF*) {}
static bool _IsNewmem(const char* name) { return _stricmp(name, "newmem") == 0; }
static void _RenderTemplate(const CE_TEMPLATE*, const char* var, const bool, CE_BUF*) {
    g_renderedHaveVar = (var != nullptr);
    g_renderedVar[0] = '\0';
    if (var) { StringCchCopyA(g_renderedVar, COUNTOF(g_renderedVar), var); }
}
static bool _InsertIntoSection(const wchar_t*, const char*) { return true; }
static bool _IsAllocDocumentSymbol(const char*) { return true; }

// --- Minimal AutoC state for the "stale highlighted item" regression --------
static bool         g_autoCActive = false;
static int          g_autoCCurrentTextLen = 0;
static const char*  g_autoCCurrentText = "";

static bool SciCall_AutoCActive() { return g_autoCActive; }
static void SciCall_AutoCCancel() {}
static int SciCall_AutoCGetCurrentText(char* out) {
    if (g_autoCCurrentTextLen <= 0) { return 0; }
    StringCchCopyA(out, 240, g_autoCCurrentText);
    return g_autoCCurrentTextLen;
}
static bool _WrapTypedAsmSymbol(int) { return false; }
static bool _ShowAllMembers() { return false; }
static void _ShowCallTipForFunctionBeforeCaret() {}
static void CeCompletion_HandleAutoCSelectionChange(const char* name) {
    StringCchCopyA(s_lastHighlight, COUNTOF(s_lastHighlight), name);
}

#include "CeCompletionHelpers.inc"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

static void Reset(const char* text, DocPos caret) {
    g_text = text;
    g_caret = g_anchor = caret;
    s_pendingCompletion = false;
    s_lastHighlight[0] = '\0';
    g_ceActive = true;
    g_isAA = false;
    g_asmInsnLine = false;
    g_wrapAsm = true;
    g_autoArgs = false;
    g_isAutoArgFn = false;
    g_resolveVar = false;
    g_resolvedVar = "";
    g_definedVar = true;
    g_clipboardVar = false;
    g_lastVarCandidate[0] = '\0';
    g_renderedHaveVar = false;
    g_renderedVar[0] = '\0';
}

int main()
{
    Settings2.CeTemplates[0].section = L"CARET";

    // --- Completion detail pane geometry: measuring and painting must agree on
    //     the body rectangle.  WM_PAINT draws the body at
    //     {pad+4, ..., pad+4 + _CeTipBodyWidth(clientCx, pad), ...}; the old
    //     measurement (clientCx - 2*pad) was wider than that rectangle, so a
    //     long signature wrapped once more at paint time and lost its line.
    int const pads[] = { 10, 13, 20 };
    int const clientWidths[] = { 360, 420, 660, 718, 966 };
    for (int pad : pads) {
        for (int cx : clientWidths) {
            int const bodyW = _CeTipBodyWidth(cx, pad);
            int const paintLeft = pad + 4;
            CHECK(bodyW == (cx - 2 * pad - 6));
            CHECK((paintLeft + bodyW) <= (cx - pad));
        }
        int const headH = 12;
        int const bodyHeights[] = { 12, 24, 36, 132 };
        for (int bodyH : bodyHeights) {
            int const clientCy = _CeTipClientHeight(pad, headH, bodyH);
            int const paintBodyTop = (2 * pad) + headH; // sepY + pad/2
            CHECK((clientCy - pad) - paintBodyTop == bodyH);
        }
    }
    CHECK(_CeTipBodyWidth(660, 10) < (662 - 2 * 10)); // old, wrong width

    // --- Usage-note lookup: binary search over the name-sorted sparse table.
    CHECK(CeCompletion_FindDoc("mov") != nullptr);
    CHECK(strstr(CeCompletion_FindDoc("mov"), "copy note") != nullptr);
    CHECK(strstr(CeCompletion_FindDoc("mov"), "\nsyntax: mov dst, src") != nullptr);
    CHECK(strcmp(CeCompletion_FindDoc("alloc"), "alloc note") == 0);
    CHECK(strcmp(CeCompletion_FindDoc("[ENABLE]"), "section") == 0);
    CHECK(strcmp(CeCompletion_FindDoc("registersymbol"), "symbol note") == 0);
    CHECK(CeCompletion_FindDoc("noSuchKeyword") == nullptr);
    CHECK(CeCompletion_FindDoc("") == nullptr);
    CHECK(CeCompletion_FindDoc(nullptr) == nullptr);
    CHECK(CeCompletion_FindDoc("MOV") == nullptr);  // byte-exact, like the table

    // Optional 4th dictionary field: user notes for user words.
    s_dictionary[0].name = "myWord";
    s_dictionary[0].signature = "myWord -- user entry";
    s_dictionary[0].category = "custom";
    s_dictionary[0].isCall = 0;
    s_dictionary[0].kind = 3;  // CE_KIND_VARIABLE
    s_dictionaryDocs[0] = (char*)"doc line 1\ndoc line 2";
    s_dictionaryCount = 1;
    CHECK(CeCompletion_FindDoc("myWord") != nullptr);
    CHECK(strcmp(CeCompletion_FindDoc("myWord"), "doc line 1\ndoc line 2") == 0);
    s_dictionaryDocs[0] = nullptr;
    s_dictionaryCount = 0;
    CHECK(CeCompletion_FindDoc("myWord") == nullptr);

    // --- Notification routing: Tab acceptance runs inside an undo transaction
    //     (EVM_None), but AutoC notifications must still reach _MsgNotifyFromEdit.
    CHECK(_IsAutoCNotification(SCN_AUTOCSELECTION));
    CHECK(_IsAutoCNotification(SCN_AUTOCCOMPLETED));
    CHECK(_IsAutoCNotification(SCN_AUTOCSELECTIONCHANGE));
    CHECK(_IsAutoCNotification(SCN_AUTOCCANCELLED));
    CHECK(_IsAutoCNotification(SCN_AUTOCCHARDELETED));
    CHECK(!_IsAutoCNotification(SCN_MODIFIED));
    CHECK(!_IsAutoCNotification(SCN_UPDATEUI));
    CHECK(_UseFullNotifyHandler(SCN_AUTOCSELECTION, EVM_None));
    CHECK(_UseFullNotifyHandler(SCN_AUTOCCOMPLETED, EVM_None));
    CHECK(_UseFullNotifyHandler(SCN_AUTOCCHARDELETED, EVM_None));
    CHECK(!_UseFullNotifyHandler(SCN_MODIFIED, EVM_None));
    CHECK(_UseFullNotifyHandler(SCN_MODIFIED, EVM_Default));

    // --- Accepting a function completion appends () and puts the caret inside.
    Reset("alloc", 5);
    CeCompletion_HandleAutoCCompleted("alloc", 0);
    CHECK(g_text == "alloc()");
    CHECK(g_caret == 6);

    Reset("registersymbol", 14);
    CeCompletion_HandleAutoCCompleted("registersymbol", 0);
    CHECK(g_text == "registersymbol()");
    CHECK(g_caret == 15);

    // A generated keyword without a catalogue entry is still an AA call.
    Reset("createthread", 12);
    CeCompletion_HandleAutoCCompleted("createthread", 0);
    CHECK(g_text == "createthread()");

    // With "auto arguments" enabled the call is still fully bracketed.
    Reset("registersymbol", 14);
    g_autoArgs = true;
    g_isAutoArgFn = true;
    g_resolveVar = true;
    g_resolvedVar = "hp";
    CeCompletion_HandleAutoCCompleted("registersymbol", 0);
    CHECK(g_text == "registersymbol(hp)");

    // A '(' fill-up already inserts its own bracket; never double it.
    Reset("alloc", 5);
    CeCompletion_HandleAutoCCompleted("alloc", '(');
    CHECK(g_text == "alloc");
    Reset("alloc(", 5);
    CeCompletion_HandleAutoCCompleted("alloc", 0);
    CHECK(g_text == "alloc(");

    // --- Accepting a user variable in AA code wraps it in [].
    Reset("mov hp", 6);
    g_isAA = true;
    CeCompletion_HandleAutoCCompleted("hp", 0);
    CHECK(g_text == "mov [hp]");

    // Bare AA fragments without an [ENABLE] marker use the instruction fallback.
    Reset("mov hp", 6);
    g_asmInsnLine = true;
    CeCompletion_HandleAutoCCompleted("hp", 0);
    CHECK(g_text == "mov [hp]");

    // Lua lines, built-ins and an already wrapped operand stay untouched.
    Reset("hp", 2);
    CeCompletion_HandleAutoCCompleted("hp", 0);
    CHECK(g_text == "hp");
    Reset("mov newmem", 10);
    g_isAA = true;
    CeCompletion_HandleAutoCCompleted("newmem", 0);
    CHECK(g_text == "mov newmem");
    Reset("mov mov", 7);
    g_isAA = true;
    CeCompletion_HandleAutoCCompleted("mov", 0);
    CHECK(g_text == "mov mov");
    Reset("mov [hp]", 7);
    g_isAA = true;
    CeCompletion_HandleAutoCCompleted("hp", 0);
    CHECK(g_text == "mov [hp]");

    // The setting really gates the behaviour.
    Reset("mov hp", 6);
    g_isAA = true;
    g_wrapAsm = false;
    CeCompletion_HandleAutoCCompleted("hp", 0);
    CHECK(g_text == "mov hp");

    // --- A fully typed word must stay in the list; only a longer root removes
    //     the candidate, so Tab can still accept it and append "()".
    CHECK(_CandidateMatches("alloc", "a", 1));
    CHECK(_CandidateMatches("alloc", "allo", 4));
    CHECK(_CandidateMatches("alloc", "alloc", 5));
    CHECK(!_CandidateMatches("alloc", "allocx", 6));
    CHECK(_CandidateMatches("registersymbol", "symbol", 6));
    CHECK(_CandidateMatches("ALLOC", "alloc", 5));
    CHECK(!_CandidateMatches("alloc", "xyz", 3));

    // --- Language context ranking: the LSP server answers in generated
    //     catalogue order, so the client sorts by the language at the caret.
    //     Auto Assembler code must rank mnemonics above the Lua API.
    CHECK(CeCompletion_ContextRank("mov", CE_KIND_ASM, CE_LANG_AA) >
          CeCompletion_ContextRank("mono_enumDomains", CE_KIND_FUNCTION, CE_LANG_AA));
    CHECK(CeCompletion_ContextRank("mono_enumDomains", CE_KIND_FUNCTION, CE_LANG_AA) <
          CeCompletion_ContextRank("newmem", CE_KIND_VARIABLE, CE_LANG_AA));
    // Lua code is the mirror image.
    CHECK(CeCompletion_ContextRank("readInteger", CE_KIND_FUNCTION, CE_LANG_LUA) >
          CeCompletion_ContextRank("mov", CE_KIND_ASM, CE_LANG_LUA));
    // C keywords win inside {$ccode}.
    CHECK(CeCompletion_ContextRank("DWORD", CE_KIND_KEYWORD, CE_LANG_C) >
          CeCompletion_ContextRank("mov", CE_KIND_ASM, CE_LANG_C));

    // np3watch comment vocabulary: directives outrank their arguments, the
    // arguments outrank ordinary items, and unrelated names get no boost.
    CHECK(CeCompletion_WatchRank("np3watch") > CeCompletion_WatchRank("dword"));
    CHECK(CeCompletion_WatchRank("dword") > 0);
    CHECK(CeCompletion_WatchRank("hexadecimal") > 0);
    CHECK(CeCompletion_WatchRank("off") > 0);
    CHECK(CeCompletion_WatchRank("mov") == 0);
    // condition/block words join the vocabulary; registers rank below words.
    CHECK(CeCompletion_WatchRank("if") > 0);
    CHECK(CeCompletion_WatchRank("begin") > 0);
    CHECK(CeCompletion_WatchRank("end") > 0);
    CHECK(CeCompletion_WatchRank("reg") > 0);
    CHECK(CeCompletion_WatchRank("rax") > 0);
    CHECK(CeCompletion_WatchRank("r12d") > 0);
    CHECK(CeCompletion_WatchRank("flags") > 0);
    CHECK(CeCompletion_WatchRank("dword") > CeCompletion_WatchRank("rax"));
    CHECK(CeCompletion_WatchRank("np3watch") > CeCompletion_WatchRank("begin"));
    CHECK(CeCompletion_WatchRank("r16d") == 0);  // out of range

    // --- Selecting an identifier in the editor names the template variable.
    char var[240];
    Reset("alloc(hp,8)", 8);
    SciCall_SetSel(6, 8); // "hp"
    CHECK(CeCompletion_GetSelectedVariable(var, COUNTOF(var)));
    CHECK(strcmp(var, "hp") == 0);
    CHECK(strcmp(g_lastVarCandidate, "hp") == 0);

    Reset("alloc(hp,8)", 8); // no selection
    CHECK(!CeCompletion_GetSelectedVariable(var, COUNTOF(var)));

    Reset("alloc(hp,8)", 8);
    SciCall_SetSel(6, 8);
    g_definedVar = false; // selection is not a defined symbol
    CHECK(!CeCompletion_GetSelectedVariable(var, COUNTOF(var)));

    Reset("alloc(hp,8)", 8);
    SciCall_SetSel(0, 300); // too long for a single variable name
    CHECK(!CeCompletion_GetSelectedVariable(var, COUNTOF(var)));

    // --- Template insertion: selection wins, clipboard is the fallback.
    Reset("alloc(hp,8)", 8);
    SciCall_SetSel(6, 8);
    g_clipboardVar = true;
    CHECK(CeTemplate_Insert(0, nullptr));
    CHECK(g_renderedHaveVar);
    CHECK(strcmp(g_renderedVar, "hp") == 0);

    Reset("alloc(hp,8)", 8);
    g_clipboardVar = true;
    CHECK(CeTemplate_Insert(0, nullptr));
    CHECK(g_renderedHaveVar);
    CHECK(strcmp(g_renderedVar, "fromClipboard") == 0);

    Reset("alloc(hp,8)", 8);
    CHECK(CeTemplate_Insert(0, nullptr));
    CHECK(!g_renderedHaveVar);

    // "newmem" is CE's built-in variable: never used by user templates.
    Reset("alloc(newmem,8)", 13);
    SciCall_SetSel(6, 12);
    CHECK(CeTemplate_Insert(0, nullptr));
    CHECK(!g_renderedHaveVar);

    // --- A dismissed AutoC list must not leave its highlighted item behind.
    // EditAutoCompleteWord() calls AutoCCancel(), which does not emit
    // SCN_AUTOCCANCELLED; HandleAutoCCompleted() would otherwise "correct" the
    // accepted word to that stale name.
    g_autoCActive = false;
    g_autoCCurrentTextLen = 0;
    StringCchCopyA(s_lastHighlight, COUNTOF(s_lastHighlight), "oldItem");
    CeCompletion_ShowCurrentItemTip();
    CHECK(s_lastHighlight[0] == 0);

    g_autoCActive = true;
    StringCchCopyA(s_lastHighlight, COUNTOF(s_lastHighlight), "oldItem");
    CeCompletion_ShowCurrentItemTip(); // fresh list, no current item yet
    CHECK(s_lastHighlight[0] == 0);

    g_autoCCurrentText = "newItem";
    g_autoCCurrentTextLen = 7;
    CeCompletion_ShowCurrentItemTip();
    CHECK(strcmp(s_lastHighlight, "newItem") == 0);

    StringCchCopyA(s_lastHighlight, COUNTOF(s_lastHighlight), "oldItem");
    CeCompletion_HandleCharAdded(nullptr, 'a');
    CHECK(s_lastHighlight[0] == 0);
    StringCchCopyA(s_lastHighlight, COUNTOF(s_lastHighlight), "oldItem");
    CeCompletion_OnCharAddedForLsp('a');
    CHECK(s_lastHighlight[0] == 0);
    g_autoCActive = false;
    g_autoCCurrentTextLen = 0;

    if (g_failures == 0) {
        std::printf("CE completion tests passed\n");
    }
    return (g_failures == 0) ? 0 : 1;
}
