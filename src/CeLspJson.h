// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspJson.h                                                                  *
*   Minimal self-contained JSON DOM (parse + build + serialize).              *
*                                                                             *
*   Shared by the Notepad3 LSP client and the built-in CE language server.     *
*   Pure ISO C: no Win32, no Scintilla, no third-party dependency.             *
*                                                                             *
*******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CELSP_JSON_TYPE
{
    CELSP_JSON_NULL = 0,
    CELSP_JSON_BOOL,
    CELSP_JSON_NUMBER,
    CELSP_JSON_STRING,
    CELSP_JSON_ARRAY,
    CELSP_JSON_OBJECT
} CELSP_JSON_TYPE;

typedef struct CELSP_JSON
{
    CELSP_JSON_TYPE type;
    bool            boolean;
    double          number;
    char*           text;     // string value
    struct CELSP_JSON** items; // array elements / object values
    char**          keys;     // object keys (NULL for arrays)
    size_t          count;
    size_t          capacity;
} CELSP_JSON;

// Parse one complete JSON value. |length| may be 0 to mean strlen(text).
// Returns NULL on malformed input, excessive nesting or out-of-memory.
CELSP_JSON* CeLspJson_Parse(const char* text, size_t length);
void        CeLspJson_Free(CELSP_JSON* value);

// Read-only accessors.  All "Get" helpers are NULL-safe and type-safe.
CELSP_JSON_TYPE CeLspJson_Type(const CELSP_JSON* value);
size_t          CeLspJson_Count(const CELSP_JSON* value);
const CELSP_JSON* CeLspJson_At(const CELSP_JSON* value, size_t index);
const CELSP_JSON* CeLspJson_Get(const CELSP_JSON* object, const char* key);
const char*       CeLspJson_String(const CELSP_JSON* value);
double            CeLspJson_Number(const CELSP_JSON* value);
int               CeLspJson_Int(const CELSP_JSON* value);
bool              CeLspJson_Bool(const CELSP_JSON* value);
const char*       CeLspJson_GetString(const CELSP_JSON* object, const char* key);
double            CeLspJson_GetNumber(const CELSP_JSON* object, const char* key);
int               CeLspJson_GetInt(const CELSP_JSON* object, const char* key);
bool              CeLspJson_GetBool(const CELSP_JSON* object, const char* key);

// Builders.  CeLspJson_Set/Add take ownership of |child| on success only.
CELSP_JSON* CeLspJson_NewNull(void);
CELSP_JSON* CeLspJson_NewBool(bool value);
CELSP_JSON* CeLspJson_NewNumber(double value);
CELSP_JSON* CeLspJson_NewString(const char* text);
CELSP_JSON* CeLspJson_NewArray(void);
CELSP_JSON* CeLspJson_NewObject(void);
bool        CeLspJson_Set(CELSP_JSON* object, const char* key, CELSP_JSON* child);
bool        CeLspJson_Add(CELSP_JSON* array, CELSP_JSON* child);

// Returns a malloc'd NUL-terminated UTF-8 string, or NULL on failure.
char* CeLspJson_Write(const CELSP_JSON* value);
// Serialize a raw string value (malloc'd, including the surrounding quotes).
char* CeLspJson_Quote(const char* text);

#ifdef __cplusplus
}
#endif
