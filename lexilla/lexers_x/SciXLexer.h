// encoding: UTF-8
#pragma once

#include "SciLexer.h"    // Scintilla/Lexilla Lexer defines

#define SCLEX_AHK         200
#define SCLEX_CSV         201
#define SCLEX_KOTLIN      202
#define SCLEX_SYSVERILOG  203
#define SCLEX_JSON5       204
#define SCLEX_AHK2        205   /* AutoHotkey v2 — shares SCE_AHK_* style ids */
#define SCLEX_CE          206   /* Cheat Engine Lua / Auto Assembler / C */

// -----------------------------------------------------------------------------
// !!!!! ADD  Lexer Linkage in:  scintilla\src\Catalogue.cxx  !!!!!
// -----------------------------------------------------------------------------

#define SCE_AHK_DEFAULT 0
#define SCE_AHK_COMMENTLINE 1
#define SCE_AHK_COMMENTBLOCK 2
#define SCE_AHK_ESCAPE 3
#define SCE_AHK_SYNOPERATOR 4
#define SCE_AHK_EXPOPERATOR 5
#define SCE_AHK_STRING 6
#define SCE_AHK_NUMBER 7
#define SCE_AHK_IDENTIFIER 8
#define SCE_AHK_VARREF 9
#define SCE_AHK_LABEL 10
#define SCE_AHK_WORD_CF 11
#define SCE_AHK_WORD_CMD 12
#define SCE_AHK_WORD_FN 13
#define SCE_AHK_WORD_DIR 14
#define SCE_AHK_WORD_KB 15
#define SCE_AHK_WORD_VAR 16
#define SCE_AHK_WORD_SP 17
#define SCE_AHK_WORD_UD 18
#define SCE_AHK_VARREFKW 19
#define SCE_AHK_ERROR 20
#define SCE_AHK_HOTSTRINGOPT 21
#define SCE_AHK_COMMENTDOC 22


#define SCE_CSV_DEFAULT 0
#define SCE_CSV_COLUMN_0 1
#define SCE_CSV_COLUMN_1 2
#define SCE_CSV_COLUMN_2 3
#define SCE_CSV_COLUMN_3 4
#define SCE_CSV_COLUMN_4 5
#define SCE_CSV_COLUMN_5 6
#define SCE_CSV_COLUMN_6 7
#define SCE_CSV_COLUMN_7 8
#define SCE_CSV_COLUMN_8 9
#define SCE_CSV_COLUMN_9 10


#define SCE_KOTLIN_DEFAULT 0
#define SCE_KOTLIN_COMMENTLINE 1
#define SCE_KOTLIN_COMMENTLINEDOC 2
#define SCE_KOTLIN_COMMENTBLOCK 3
#define SCE_KOTLIN_COMMENTBLOCKDOC 4
#define SCE_KOTLIN_COMMENTDOCWORD 5
#define SCE_KOTLIN_TASKMARKER 6
#define SCE_KOTLIN_NUMBER 7
#define SCE_KOTLIN_OPERATOR 8
#define SCE_KOTLIN_OPERATOR2 9
#define SCE_KOTLIN_CHARACTER 10
#define SCE_KOTLIN_STRING 11
#define SCE_KOTLIN_RAWSTRING 12
#define SCE_KOTLIN_ESCAPECHAR 13
#define SCE_KOTLIN_VARIABLE 14
#define SCE_KOTLIN_LABEL 15
#define SCE_KOTLIN_IDENTIFIER 16
#define SCE_KOTLIN_ANNOTATION 17
#define SCE_KOTLIN_BACKTICKS 18
#define SCE_KOTLIN_WORD 21
#define SCE_KOTLIN_CLASS 22
#define SCE_KOTLIN_INTERFACE 23
#define SCE_KOTLIN_ENUM 24
#define SCE_KOTLIN_FUNCTION 25
#define SCE_KOTLIN_FUNCTION_DEFINITION 26


// JSON5 / JSON-LD homebrew lexer (SCLEX_JSON5).  Style values mirror the
// strict-JSON SCE_JSON_* range in SciLexer.h so visual themes can share
// colors, but the symbols live in their own namespace to keep the strict
// and JSON5 lexers independent.
#define SCE_JSON5_DEFAULT 0
#define SCE_JSON5_NUMBER 1
#define SCE_JSON5_STRING 2
#define SCE_JSON5_STRINGEOL 3
#define SCE_JSON5_PROPERTYNAME 4
#define SCE_JSON5_ESCAPESEQUENCE 5
#define SCE_JSON5_LINECOMMENT 6
#define SCE_JSON5_BLOCKCOMMENT 7
#define SCE_JSON5_OPERATOR 8
#define SCE_JSON5_URI 9
#define SCE_JSON5_COMPACTIRI 10
#define SCE_JSON5_KEYWORD 11
#define SCE_JSON5_LDKEYWORD 12
#define SCE_JSON5_ERROR 13


// Cheat Engine script lexer (SCLEX_CE).
//
// A CE script mixes three languages: Lua (trainer code), Auto Assembler
// ([ENABLE]/[DISABLE] sections, {$asm}) and inline C ({$ccode}).  The first
// twelve ids intentionally mirror the SCE_LUA_* range so existing theme files
// that use those numbers keep working; the rest are CE additions.
#define SCE_CE_DEFAULT 0
#define SCE_CE_COMMENT 1
#define SCE_CE_COMMENTLINE 2
#define SCE_CE_COMMENTDOC 3
#define SCE_CE_NUMBER 4
#define SCE_CE_WORD 5          /* Lua keyword / constant */
#define SCE_CE_STRING 6
#define SCE_CE_CHARACTER 7
#define SCE_CE_LITERALSTRING 8
#define SCE_CE_PREPROCESSOR 9  /* {$lua} {$asm} {$ccode} */
#define SCE_CE_OPERATOR 10
#define SCE_CE_IDENTIFIER 11
#define SCE_CE_STRINGEOL 12
#define SCE_CE_WORD2 13        /* Cheat Engine Lua API */
#define SCE_CE_WORD3 14        /* Auto Assembler command / assembler directive */
#define SCE_CE_WORD4 15        /* x86/x64 mnemonic, register, jump, condition */
#define SCE_CE_WORD5 16        /* type, size, class name */
#define SCE_CE_WORD6 17        /* [ENABLE] / [DISABLE] marker */
#define SCE_CE_WORD7 18        /* Auto Assembler label definition */
#define SCE_CE_WORD8 19        /* Lua function name */
#define SCE_CE_AOB 20          /* AOB wildcard: ?? / ? */
#define SCE_CE_MODULE 21       /* game.exe, module.dll */
#define SCE_CE_LABEL 22        /* Lua ::label:: */
#define SCE_CE_ESCAPE 23       /* escape sequence inside a string */
#define SCE_CE_ERROR 24        /* unterminated construct */
#define SCE_CE_COMMENTBRACE 25 /* Auto Assembler brace comment */

