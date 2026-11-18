#include "StyleLexers.h"
#include "ce_completions_data.h"

// ----------------------------------------------------------------------------
// Cheat Engine Lua / Auto Assembler / inline C script.
//
// The homebrew LexCe lexer (lexilla\lexers_x\LexCe.cxx) tokenizes the three
// languages a CE script mixes.  Cheat Engine adds Auto Assembler commands,
// x86 registers/mnemonics and CE-specific Lua API names, distributed over the
// available keyword sets.
//
// The style row names are part of the user INI/theme contract: keep the
// existing names and append new rows at the end.
// ----------------------------------------------------------------------------

KEYWORDLIST KeyWords_CE =
{
    CE_KEYWORDS_LUA,    // Lua keywords, standard libraries and CE globals
    CE_KEYWORDS_API,    // Cheat Engine Lua API
    CE_KEYWORDS_AA,     // Auto Assembler commands and assembler directives
    CE_KEYWORDS_ASM,    // x86/x64 registers and mnemonics
    CE_KEYWORDS_TYPES,  // CE classes, value types and object names
    "",
    "",
    "",
    NULL,
};


EDITLEXER lexCE =
{
    SCLEX_CE, "ce", IDS_LEX_CE, L"Cheat Engine Script", L"cea", L"cea; aa; cet; ct; lua",
    &KeyWords_CE, {
        // Light-theme defaults mirror the --c-* palette of the reference CE
        // editor (ce-lua-editor.html); dark themes override the same rows.
        { {STYLE_DEFAULT}, IDS_LEX_STR_Default, L"Default", L"", L"" },
        { {MULTI_STYLE(SCE_CE_COMMENT,SCE_CE_COMMENTLINE,SCE_CE_COMMENTDOC,SCE_CE_COMMENTBRACE)}, IDS_LEX_STR_Comment, L"Comment", L"italic; fore:#6E7781", L"" },
        { {SCE_CE_NUMBER}, IDS_LEX_STR_Number, L"Number", L"fore:#0550AE", L"" },
        { {SCE_CE_WORD}, IDS_LEX_STR_Keyword, L"Keyword", L"bold; fore:#CF222E", L"" },
        { {SCE_CE_WORD2}, IDS_LEX_STR_CE_API, L"CE API", L"fore:#1F883D", L"" },
        { {SCE_CE_WORD3}, IDS_LEX_STR_CE_AA, L"Auto Assembler", L"bold; fore:#CF222E; back:#FFE9E9", L"" },
        { {SCE_CE_WORD4}, IDS_LEX_STR_CE_ASM, L"Assembly & Registers", L"bold; fore:#0969DA", L"" },
        { {SCE_CE_WORD5}, IDS_LEX_STR_CE_TYPES, L"Types & Classes", L"italic; fore:#8250DF", L"" },
        { {MULTI_STYLE(SCE_CE_STRING,SCE_CE_STRINGEOL,SCE_CE_CHARACTER,0)}, IDS_LEX_STR_String, L"String", L"fore:#0A3069", L"" },
        { {MULTI_STYLE(SCE_CE_LITERALSTRING,SCE_CE_ESCAPE,0,0)}, IDS_LEX_STR_LitStrg, L"Literal String", L"bold; fore:#0A3069", L"" },
        { {SCE_CE_PREPROCESSOR}, IDS_LEX_STR_PreProc, L"Preprocessor", L"bold; fore:#8250DF; back:#EFE6FF", L"" },
        { {SCE_CE_OPERATOR}, IDS_LEX_STR_Operator, L"Operator", L"fore:#0550AE", L"" },
        { {SCE_CE_IDENTIFIER}, IDS_LEX_STR_Identifier, L"Identifier", L"", L"" },
        { {MULTI_STYLE(SCE_CE_LABEL,SCE_CE_WORD7,0,0)}, IDS_LEX_STR_Label, L"Label", L"bold; fore:#1F883D", L"" },
        // Rows below are CE additions; INI/theme files without them fall back
        // to the defaults given here.
        { {SCE_CE_WORD8}, IDS_LEX_STR_CE_FUNCTION, L"Function", L"bold; fore:#8250DF", L"" },
        { {SCE_CE_MODULE}, IDS_LEX_STR_CE_MODULE, L"Module", L"fore:#953800; underline", L"" },
        { {SCE_CE_AOB}, IDS_LEX_STR_CE_WILDCARD, L"Wildcard", L"bold; fore:#6E7781", L"" },
        { {SCE_CE_WORD6}, IDS_LEX_STR_CE_SECTION, L"Section Marker", L"bold; fore:#CF222E; back:#FFE9E9", L"" },
        EDITLEXER_SENTINEL
    }
};
