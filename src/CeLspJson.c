// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspJson.c                                                                  *
*   Minimal self-contained JSON DOM.  See CeLspJson.h.                        *
*                                                                             *
*******************************************************************************/

#define _CRT_SECURE_NO_WARNINGS

#include "CeLspJson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CELSP_JSON_MAX_DEPTH 64
#define CELSP_JSON_MAX_SIZE  (64u * 1024u * 1024u)

// ----------------------------------------------------------------------------
// Growable byte buffer
// ----------------------------------------------------------------------------

typedef struct CELSP_BUF
{
    char*  data;
    size_t len;
    size_t cap;
} CELSP_BUF;

static bool _BufReserve(CELSP_BUF* b, size_t extra)
{
    if (b->cap == 0) {
        b->cap = 256;
        b->data = (char*)malloc(b->cap);
        if (!b->data) {
            b->cap = 0;
            return false;
        }
        b->data[0] = '\0';
        b->len = 0;
    }
    // +1 for the terminating NUL
    const size_t needed = b->len + extra + 1u;
    if (needed <= b->cap) {
        return true;
    }
    size_t cap = b->cap;
    while (cap < needed) {
        if (cap > (CELSP_JSON_MAX_SIZE / 2u)) {
            return false;
        }
        cap *= 2u;
    }
    char* const grown = (char*)realloc(b->data, cap);
    if (!grown) {
        return false;
    }
    b->data = grown;
    b->cap = cap;
    return true;
}

static bool _BufAppendN(CELSP_BUF* b, const char* src, size_t n)
{
    if (!_BufReserve(b, n)) {
        return false;
    }
    if (n > 0u) {
        memcpy(b->data + b->len, src, n);
        b->len += n;
    }
    b->data[b->len] = '\0';
    return true;
}

static bool _BufAppendCh(CELSP_BUF* b, char ch)
{
    return _BufAppendN(b, &ch, 1u);
}

static bool _BufAppendUtf8(CELSP_BUF* b, unsigned int cp)
{
    if (cp <= 0x7Fu) {
        return _BufAppendCh(b, (char)cp);
    }
    if (cp <= 0x7FFu) {
        const char seq[2] = { (char)(0xC0u | (cp >> 6)), (char)(0x80u | (cp & 0x3Fu)) };
        return _BufAppendN(b, seq, 2u);
    }
    if (cp <= 0xFFFFu) {
        const char seq[3] = { (char)(0xE0u | (cp >> 12)),
                              (char)(0x80u | ((cp >> 6) & 0x3Fu)),
                              (char)(0x80u | (cp & 0x3Fu)) };
        return _BufAppendN(b, seq, 3u);
    }
    const char seq[4] = { (char)(0xF0u | (cp >> 18)),
                          (char)(0x80u | ((cp >> 12) & 0x3Fu)),
                          (char)(0x80u | ((cp >> 6) & 0x3Fu)),
                          (char)(0x80u | (cp & 0x3Fu)) };
    return _BufAppendN(b, seq, 4u);
}

static bool _BufAppendEscaped(CELSP_BUF* b, const char* text)
{
    if (!_BufAppendCh(b, '"')) {
        return false;
    }
    if (!text) {
        return _BufAppendCh(b, '"');
    }
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        const unsigned char ch = *p;
        switch (ch) {
        case '"':
            if (!_BufAppendN(b, "\\\"", 2u)) { return false; }
            break;
        case '\\':
            if (!_BufAppendN(b, "\\\\", 2u)) { return false; }
            break;
        case '\b':
            if (!_BufAppendN(b, "\\b", 2u)) { return false; }
            break;
        case '\f':
            if (!_BufAppendN(b, "\\f", 2u)) { return false; }
            break;
        case '\n':
            if (!_BufAppendN(b, "\\n", 2u)) { return false; }
            break;
        case '\r':
            if (!_BufAppendN(b, "\\r", 2u)) { return false; }
            break;
        case '\t':
            if (!_BufAppendN(b, "\\t", 2u)) { return false; }
            break;
        default:
            if (ch < 0x20u) {
                char esc[8];
                const int n = snprintf(esc, sizeof(esc), "\\u%04x", (unsigned int)ch);
                if ((n <= 0) || !_BufAppendN(b, esc, (size_t)n)) { return false; }
            }
            else if (!_BufAppendCh(b, (char)ch)) {
                return false;
            }
            break;
        }
    }
    return _BufAppendCh(b, '"');
}

// ----------------------------------------------------------------------------
// Node allocation
// ----------------------------------------------------------------------------

static CELSP_JSON* _NewNode(CELSP_JSON_TYPE type)
{
    CELSP_JSON* const v = (CELSP_JSON*)calloc(1u, sizeof(CELSP_JSON));
    if (v) {
        v->type = type;
    }
    return v;
}

static bool _NodeGrow(CELSP_JSON* v)
{
    if (v->count < v->capacity) {
        return true;
    }
    const size_t cap = (v->capacity == 0u) ? 8u : (v->capacity * 2u);
    CELSP_JSON** const items = (CELSP_JSON**)realloc(v->items, cap * sizeof(CELSP_JSON*));
    if (!items) {
        return false;
    }
    v->items = items;
    if (v->type == CELSP_JSON_OBJECT) {
        char** const keys = (char**)realloc(v->keys, cap * sizeof(char*));
        if (!keys) {
            return false;
        }
        v->keys = keys;
    }
    v->capacity = cap;
    return true;
}

void CeLspJson_Free(CELSP_JSON* value)
{
    if (!value) {
        return;
    }
    for (size_t i = 0; i < value->count; ++i) {
        CeLspJson_Free(value->items[i]);
        free(value->keys ? value->keys[i] : NULL);
    }
    free(value->items);
    free(value->keys);
    free(value->text);
    free(value);
}

CELSP_JSON_TYPE CeLspJson_Type(const CELSP_JSON* value)
{
    return value ? value->type : CELSP_JSON_NULL;
}

size_t CeLspJson_Count(const CELSP_JSON* value)
{
    return value ? value->count : 0u;
}

const CELSP_JSON* CeLspJson_At(const CELSP_JSON* value, size_t index)
{
    return (value && (index < value->count)) ? value->items[index] : NULL;
}

const CELSP_JSON* CeLspJson_Get(const CELSP_JSON* object, const char* key)
{
    if (!object || (object->type != CELSP_JSON_OBJECT) || !key) {
        return NULL;
    }
    for (size_t i = 0; i < object->count; ++i) {
        if (object->keys[i] && (strcmp(object->keys[i], key) == 0)) {
            return object->items[i];
        }
    }
    return NULL;
}

const char* CeLspJson_String(const CELSP_JSON* value)
{
    return (value && (value->type == CELSP_JSON_STRING)) ? value->text : NULL;
}

double CeLspJson_Number(const CELSP_JSON* value)
{
    return (value && (value->type == CELSP_JSON_NUMBER)) ? value->number : 0.0;
}

int CeLspJson_Int(const CELSP_JSON* value)
{
    return (int)CeLspJson_Number(value);
}

bool CeLspJson_Bool(const CELSP_JSON* value)
{
    return (value && (value->type == CELSP_JSON_BOOL)) ? value->boolean : false;
}

const char* CeLspJson_GetString(const CELSP_JSON* object, const char* key)
{
    return CeLspJson_String(CeLspJson_Get(object, key));
}

double CeLspJson_GetNumber(const CELSP_JSON* object, const char* key)
{
    return CeLspJson_Number(CeLspJson_Get(object, key));
}

int CeLspJson_GetInt(const CELSP_JSON* object, const char* key)
{
    return CeLspJson_Int(CeLspJson_Get(object, key));
}

bool CeLspJson_GetBool(const CELSP_JSON* object, const char* key)
{
    return CeLspJson_Bool(CeLspJson_Get(object, key));
}

CELSP_JSON* CeLspJson_NewNull(void)
{
    return _NewNode(CELSP_JSON_NULL);
}

CELSP_JSON* CeLspJson_NewBool(bool value)
{
    CELSP_JSON* const v = _NewNode(CELSP_JSON_BOOL);
    if (v) { v->boolean = value; }
    return v;
}

CELSP_JSON* CeLspJson_NewNumber(double value)
{
    CELSP_JSON* const v = _NewNode(CELSP_JSON_NUMBER);
    if (v) { v->number = value; }
    return v;
}

CELSP_JSON* CeLspJson_NewString(const char* text)
{
    CELSP_JSON* const v = _NewNode(CELSP_JSON_STRING);
    if (!v) { return NULL; }
    const size_t len = text ? strlen(text) : 0u;
    v->text = (char*)malloc(len + 1u);
    if (!v->text) {
        free(v);
        return NULL;
    }
    if (len > 0u) {
        memcpy(v->text, text, len);
    }
    v->text[len] = '\0';
    return v;
}

CELSP_JSON* CeLspJson_NewArray(void)
{
    return _NewNode(CELSP_JSON_ARRAY);
}

CELSP_JSON* CeLspJson_NewObject(void)
{
    return _NewNode(CELSP_JSON_OBJECT);
}

bool CeLspJson_Set(CELSP_JSON* object, const char* key, CELSP_JSON* child)
{
    if (!object || (object->type != CELSP_JSON_OBJECT) || !key || !child || !_NodeGrow(object)) {
        return false;
    }
    const size_t len = strlen(key);
    char* const copy = (char*)malloc(len + 1u);
    if (!copy) {
        return false;
    }
    memcpy(copy, key, len);
    copy[len] = '\0';
    object->keys[object->count] = copy;
    object->items[object->count] = child;
    ++object->count;
    return true;
}

bool CeLspJson_Add(CELSP_JSON* array, CELSP_JSON* child)
{
    if (!array || (array->type != CELSP_JSON_ARRAY) || !child || !_NodeGrow(array)) {
        return false;
    }
    array->items[array->count] = child;
    ++array->count;
    return true;
}

// ----------------------------------------------------------------------------
// Parser
// ----------------------------------------------------------------------------

typedef struct CELSP_PARSER
{
    const char* p;
    const char* end;
} CELSP_PARSER;

static void _SkipWs(CELSP_PARSER* ps)
{
    while ((ps->p < ps->end) && ((*ps->p == ' ') || (*ps->p == '\t') ||
                                 (*ps->p == '\r') || (*ps->p == '\n'))) {
        ++ps->p;
    }
}

static int _HexDigit(char ch)
{
    if ((ch >= '0') && (ch <= '9')) { return ch - '0'; }
    if ((ch >= 'a') && (ch <= 'f')) { return ch - 'a' + 10; }
    if ((ch >= 'A') && (ch <= 'F')) { return ch - 'A' + 10; }
    return -1;
}

static bool _ReadHex4(CELSP_PARSER* ps, unsigned int* out)
{
    if ((size_t)(ps->end - ps->p) < 4u) {
        return false;
    }
    unsigned int cp = 0u;
    for (int i = 0; i < 4; ++i) {
        const int d = _HexDigit(ps->p[i]);
        if (d < 0) {
            return false;
        }
        cp = (cp << 4) | (unsigned int)d;
    }
    ps->p += 4;
    *out = cp;
    return true;
}

static char* _ParseString(CELSP_PARSER* ps)
{
    if ((ps->p >= ps->end) || (*ps->p != '"')) {
        return NULL;
    }
    ++ps->p;
    CELSP_BUF b = { NULL, 0u, 0u };
    while (ps->p < ps->end) {
        const unsigned char ch = (unsigned char)*ps->p++;
        if (ch == '"') {
            if (!_BufReserve(&b, 0u)) { // guarantee a NUL-terminated empty string
                return NULL;
            }
            return b.data;
        }
        if (ch == '\\') {
            if (ps->p >= ps->end) { break; }
            const char esc = *ps->p++;
            switch (esc) {
            case '"':  if (!_BufAppendCh(&b, '"'))  { goto fail; } break;
            case '\\': if (!_BufAppendCh(&b, '\\')) { goto fail; } break;
            case '/':  if (!_BufAppendCh(&b, '/'))  { goto fail; } break;
            case 'b':  if (!_BufAppendCh(&b, '\b')) { goto fail; } break;
            case 'f':  if (!_BufAppendCh(&b, '\f')) { goto fail; } break;
            case 'n':  if (!_BufAppendCh(&b, '\n')) { goto fail; } break;
            case 'r':  if (!_BufAppendCh(&b, '\r')) { goto fail; } break;
            case 't':  if (!_BufAppendCh(&b, '\t')) { goto fail; } break;
            case 'u': {
                unsigned int cp = 0u;
                if (!_ReadHex4(ps, &cp)) { goto fail; }
                if ((cp >= 0xD800u) && (cp <= 0xDBFFu) &&
                    ((size_t)(ps->end - ps->p) >= 6u) && (ps->p[0] == '\\') && (ps->p[1] == 'u')) {
                    const char* const save = ps->p;
                    ps->p += 2;
                    unsigned int low = 0u;
                    if (_ReadHex4(ps, &low) && (low >= 0xDC00u) && (low <= 0xDFFFu)) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    }
                    else {
                        ps->p = save; // lone surrogate: keep the first code unit
                    }
                }
                if (!_BufAppendUtf8(&b, cp)) { goto fail; }
                break;
            }
            default:
                goto fail;
            }
        }
        else if (ch < 0x20u) {
            goto fail; // unescaped control character
        }
        else if (!_BufAppendCh(&b, (char)ch)) {
            goto fail;
        }
    }
fail:
    free(b.data);
    return NULL;
}

static CELSP_JSON* _ParseValue(CELSP_PARSER* ps, int depth);

static CELSP_JSON* _ParseObject(CELSP_PARSER* ps, int depth)
{
    CELSP_JSON* const obj = CeLspJson_NewObject();
    if (!obj) { return NULL; }
    ++ps->p; // '{'
    _SkipWs(ps);
    if ((ps->p < ps->end) && (*ps->p == '}')) {
        ++ps->p;
        return obj;
    }
    while (ps->p < ps->end) {
        char* const key = _ParseString(ps);
        if (!key) { goto fail; }
        _SkipWs(ps);
        if ((ps->p >= ps->end) || (*ps->p != ':')) { free(key); goto fail; }
        ++ps->p;
        _SkipWs(ps);
        CELSP_JSON* const child = _ParseValue(ps, depth + 1);
        if (!child || !CeLspJson_Set(obj, key, child)) {
            CeLspJson_Free(child);
            free(key);
            goto fail;
        }
        free(key); // Set() copies the key
        _SkipWs(ps);
        if (ps->p >= ps->end) { goto fail; }
        if (*ps->p == '}') { ++ps->p; return obj; }
        if (*ps->p != ',') { goto fail; }
        ++ps->p;
        _SkipWs(ps);
    }
fail:
    CeLspJson_Free(obj);
    return NULL;
}

static CELSP_JSON* _ParseArray(CELSP_PARSER* ps, int depth)
{
    CELSP_JSON* const arr = CeLspJson_NewArray();
    if (!arr) { return NULL; }
    ++ps->p; // '['
    _SkipWs(ps);
    if ((ps->p < ps->end) && (*ps->p == ']')) {
        ++ps->p;
        return arr;
    }
    while (ps->p < ps->end) {
        CELSP_JSON* const child = _ParseValue(ps, depth + 1);
        if (!child || !CeLspJson_Add(arr, child)) {
            CeLspJson_Free(child);
            goto fail;
        }
        _SkipWs(ps);
        if (ps->p >= ps->end) { goto fail; }
        if (*ps->p == ']') { ++ps->p; return arr; }
        if (*ps->p != ',') { goto fail; }
        ++ps->p;
        _SkipWs(ps);
    }
fail:
    CeLspJson_Free(arr);
    return NULL;
}

static bool _ParseLiteral(CELSP_PARSER* ps, const char* word, size_t len)
{
    if ((size_t)(ps->end - ps->p) < len) {
        return false;
    }
    if (memcmp(ps->p, word, len) != 0) {
        return false;
    }
    ps->p += len;
    return true;
}

static CELSP_JSON* _ParseValue(CELSP_PARSER* ps, int depth)
{
    if (depth > CELSP_JSON_MAX_DEPTH) {
        return NULL;
    }
    _SkipWs(ps);
    if (ps->p >= ps->end) {
        return NULL;
    }
    const char ch = *ps->p;
    if (ch == '{') { return _ParseObject(ps, depth); }
    if (ch == '[') { return _ParseArray(ps, depth); }
    if (ch == '"') {
        char* const text = _ParseString(ps);
        if (!text) { return NULL; }
        CELSP_JSON* const v = _NewNode(CELSP_JSON_STRING);
        if (!v) { free(text); return NULL; }
        v->text = text;
        return v;
    }
    if (ch == 't') {
        return _ParseLiteral(ps, "true", 4u) ? CeLspJson_NewBool(true) : NULL;
    }
    if (ch == 'f') {
        return _ParseLiteral(ps, "false", 5u) ? CeLspJson_NewBool(false) : NULL;
    }
    if (ch == 'n') {
        return _ParseLiteral(ps, "null", 4u) ? CeLspJson_NewNull() : NULL;
    }
    if ((ch == '-') || ((ch >= '0') && (ch <= '9'))) {
        char num[64];
        size_t n = 0u;
        const char* q = ps->p;
        while ((q < ps->end) && (n < (sizeof(num) - 1u)) &&
               ((*q == '-') || (*q == '+') || (*q == '.') || (*q == 'e') || (*q == 'E') ||
                ((*q >= '0') && (*q <= '9')))) {
            num[n++] = *q++;
        }
        num[n] = '\0';
        if ((n == 0u) || (n >= (sizeof(num) - 1u))) {
            return NULL;
        }
        char* endPtr = NULL;
        const double d = strtod(num, &endPtr);
        if (!endPtr || (*endPtr != '\0')) {
            return NULL;
        }
        ps->p = q;
        return CeLspJson_NewNumber(d);
    }
    return NULL;
}

CELSP_JSON* CeLspJson_Parse(const char* text, size_t length)
{
    if (!text) {
        return NULL;
    }
    if (length == 0u) {
        length = strlen(text);
    }
    CELSP_PARSER ps = { text, text + length };
    CELSP_JSON* const v = _ParseValue(&ps, 0);
    if (v) {
        _SkipWs(&ps);
        if (ps.p != ps.end) {
            CeLspJson_Free(v);
            return NULL;
        }
    }
    return v;
}

// ----------------------------------------------------------------------------
// Serializer
// ----------------------------------------------------------------------------

static bool _WriteValue(CELSP_BUF* b, const CELSP_JSON* v)
{
    if (!v) {
        return _BufAppendN(b, "null", 4u);
    }
    switch (v->type) {
    case CELSP_JSON_NULL:
        return _BufAppendN(b, "null", 4u);
    case CELSP_JSON_BOOL:
        return _BufAppendN(b, v->boolean ? "true" : "false", v->boolean ? 4u : 5u);
    case CELSP_JSON_NUMBER: {
        char num[64];
        const int n = snprintf(num, sizeof(num), "%.15g", v->number);
        return (n > 0) && _BufAppendN(b, num, (size_t)n);
    }
    case CELSP_JSON_STRING:
        return _BufAppendEscaped(b, v->text);
    case CELSP_JSON_ARRAY:
        if (!_BufAppendCh(b, '[')) { return false; }
        for (size_t i = 0; i < v->count; ++i) {
            if ((i > 0u) && !_BufAppendCh(b, ',')) { return false; }
            if (!_WriteValue(b, v->items[i])) { return false; }
        }
        return _BufAppendCh(b, ']');
    case CELSP_JSON_OBJECT:
        if (!_BufAppendCh(b, '{')) { return false; }
        for (size_t i = 0; i < v->count; ++i) {
            if ((i > 0u) && !_BufAppendCh(b, ',')) { return false; }
            if (!_BufAppendEscaped(b, v->keys[i]) || !_BufAppendCh(b, ':') ||
                !_WriteValue(b, v->items[i])) {
                return false;
            }
        }
        return _BufAppendCh(b, '}');
    default:
        return false;
    }
}

char* CeLspJson_Write(const CELSP_JSON* value)
{
    CELSP_BUF b = { NULL, 0u, 0u };
    if (!_WriteValue(&b, value)) {
        free(b.data);
        return NULL;
    }
    if (!b.data) {
        return _strdup("null");
    }
    return b.data;
}

char* CeLspJson_Quote(const char* text)
{
    CELSP_BUF b = { NULL, 0u, 0u };
    if (!_BufAppendEscaped(&b, text)) {
        free(b.data);
        return NULL;
    }
    return b.data;
}
