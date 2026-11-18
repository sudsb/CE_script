// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspFraming.c                                                               *
*   LSP base protocol + JSON-RPC 2.0 envelopes.  See CeLspFraming.h.          *
*                                                                             *
*******************************************************************************/

#define _CRT_SECURE_NO_WARNINGS

#include "CeLspFraming.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* _Format(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = vsnprintf(NULL, 0u, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return NULL;
    }
    char* const out = (char*)malloc((size_t)n + 1u);
    if (!out) {
        va_end(ap2);
        return NULL;
    }
    const int written = vsnprintf(out, (size_t)n + 1u, fmt, ap2);
    va_end(ap2);
    if (written < 0) {
        free(out);
        return NULL;
    }
    return out;
}

char* CeLspFraming_Encode(const char* json, size_t length, size_t* outLength)
{
    if (!json || !outLength || (length > CELSP_MAX_MESSAGE)) {
        return NULL;
    }
    char header[64];
    const int n = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", length);
    if ((n <= 0) || ((size_t)n >= sizeof(header))) {
        return NULL;
    }
    char* const frame = (char*)malloc((size_t)n + length + 1u);
    if (!frame) {
        return NULL;
    }
    memcpy(frame, header, (size_t)n);
    memcpy(frame + n, json, length);
    frame[(size_t)n + length] = '\0';
    *outLength = (size_t)n + length;
    return frame;
}

int CeLspFraming_HeaderEnd(const char* data, size_t length)
{
    if (!data || (length < 4u)) {
        return -1;
    }
    for (size_t i = 0u; (i + 3u) < length; ++i) {
        if ((data[i] == '\r') && (data[i + 1u] == '\n') &&
            (data[i + 2u] == '\r') && (data[i + 3u] == '\n')) {
            return (int)(i + 4u);
        }
    }
    // Tolerate servers that terminate headers with bare LF.
    for (size_t i = 0u; (i + 1u) < length; ++i) {
        if ((data[i] == '\n') && (data[i + 1u] == '\n')) {
            return (int)(i + 2u);
        }
    }
    return -1;
}

static char _Lower(char ch)
{
    return ((ch >= 'A') && (ch <= 'Z')) ? (char)(ch + ('a' - 'A')) : ch;
}

static bool _StartsWithNoCase(const char* text, const char* prefix)
{
    while (*prefix) {
        if (*text == '\0') {
            return false;
        }
        if (_Lower(*text) != _Lower(*prefix)) {
            return false;
        }
        ++text;
        ++prefix;
    }
    return true;
}

bool CeLspFraming_ContentLength(const char* data, size_t headerEnd, size_t* contentLength)
{
    if (!data || !contentLength || (headerEnd == 0u) || (headerEnd > 8192u)) {
        return false;
    }
    char* const header = (char*)malloc(headerEnd + 1u);
    if (!header) {
        return false;
    }
    memcpy(header, data, headerEnd);
    header[headerEnd] = '\0';
    bool found = false;
    size_t value = 0u;
    char* line = header;
    while (line && *line) {
        char* const eol = strchr(line, '\n');
        if (eol) {
            *eol = '\0';
        }
        if (_StartsWithNoCase(line, "Content-Length:")) {
            const char* p = line + 15;
            while ((*p == ' ') || (*p == '\t')) { ++p; }
            if ((*p >= '0') && (*p <= '9')) {
                char* endPtr = NULL;
                const unsigned long long parsed = strtoull(p, &endPtr, 10);
                while ((*endPtr == ' ') || (*endPtr == '\t') || (*endPtr == '\r')) {
                    ++endPtr;
                }
                if ((endPtr != p) && (*endPtr == '\0') &&
                    (parsed <= (unsigned long long)(size_t)-1)) {
                    value = (size_t)parsed;
                    found = true;
                }
            }
        }
        line = eol ? (eol + 1) : NULL;
    }
    free(header);
    if (!found || (value > CELSP_MAX_MESSAGE)) {
        return false;
    }
    *contentLength = value;
    return true;
}

char* CeLspFraming_Request(int id, const char* method, const CELSP_JSON* params)
{
    if (!method) {
        return NULL;
    }
    char* const quoted = CeLspJson_Quote(method);
    char* const rawParams = params ? CeLspJson_Write(params) : NULL;
    char* const out = _Format("{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":%s,\"params\":%s}",
                              id, quoted ? quoted : "\"\"", rawParams ? rawParams : "{}");
    free(rawParams);
    free(quoted);
    return out;
}

char* CeLspFraming_Notification(const char* method, const CELSP_JSON* params)
{
    if (!method) {
        return NULL;
    }
    char* const quoted = CeLspJson_Quote(method);
    char* const rawParams = params ? CeLspJson_Write(params) : NULL;
    char* const out = _Format("{\"jsonrpc\":\"2.0\",\"method\":%s,\"params\":%s}",
                              quoted ? quoted : "\"\"", rawParams ? rawParams : "{}");
    free(rawParams);
    free(quoted);
    return out;
}

char* CeLspFraming_ResultRaw(int id, const char* rawResultJson)
{
    return _Format("{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}",
                   id, (rawResultJson && *rawResultJson) ? rawResultJson : "null");
}

char* CeLspFraming_Result(int id, const CELSP_JSON* result)
{
    char* const raw = result ? CeLspJson_Write(result) : NULL;
    char* const out = CeLspFraming_ResultRaw(id, raw);
    free(raw);
    return out;
}

char* CeLspFraming_Error(int id, int code, const char* message)
{
    char* const quoted = CeLspJson_Quote(message ? message : "unknown error");
    char* const out = _Format("{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":%d,\"message\":%s}}",
                              id, code, quoted ? quoted : "\"unknown error\"");
    free(quoted);
    return out;
}

char* CeLspFraming_EncodeRequest(int id, const char* method, const CELSP_JSON* params, size_t* outLength)
{
    char* const json = CeLspFraming_Request(id, method, params);
    if (!json) {
        return NULL;
    }
    char* const frame = CeLspFraming_Encode(json, strlen(json), outLength);
    free(json);
    return frame;
}

char* CeLspFraming_EncodeNotification(const char* method, const CELSP_JSON* params, size_t* outLength)
{
    char* const json = CeLspFraming_Notification(method, params);
    if (!json) {
        return NULL;
    }
    char* const frame = CeLspFraming_Encode(json, strlen(json), outLength);
    free(json);
    return frame;
}

char* CeLspFraming_EncodeResult(int id, const CELSP_JSON* result, size_t* outLength)
{
    char* const json = CeLspFraming_Result(id, result);
    if (!json) {
        return NULL;
    }
    char* const frame = CeLspFraming_Encode(json, strlen(json), outLength);
    free(json);
    return frame;
}
