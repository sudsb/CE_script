// encoding: UTF-8
/******************************************************************************
*                                                                             *
* CeLspFraming.h                                                               *
*   LSP base-protocol framing ("Content-Length") + JSON-RPC 2.0 envelopes.    *
*   Pure ISO C, shared by client and built-in server.                          *
*                                                                             *
*******************************************************************************/

#pragma once

#include "CeLspJson.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CELSP_MAX_MESSAGE (16u * 1024u * 1024u)

// Wrap one JSON document in a standard LSP frame.  Returns malloc'd bytes;
// |*outLength| is the exact frame length (the trailing NUL is not counted).
char* CeLspFraming_Encode(const char* json, size_t length, size_t* outLength);

// Index just past the "\r\n\r\n" header terminator, or -1 when incomplete.
int  CeLspFraming_HeaderEnd(const char* data, size_t length);
// Extract the Content-Length value from a complete header.
bool CeLspFraming_ContentLength(const char* data, size_t headerEnd, size_t* contentLength);

// JSON-RPC envelopes.  Return malloc'd UTF-8 JSON documents (free with free()).
char* CeLspFraming_Request(int id, const char* method, const CELSP_JSON* params);
char* CeLspFraming_Notification(const char* method, const CELSP_JSON* params);
char* CeLspFraming_Result(int id, const CELSP_JSON* result);
char* CeLspFraming_ResultRaw(int id, const char* rawResultJson);
char* CeLspFraming_Error(int id, int code, const char* message);

// Convenience: frame a request/notification directly (malloc'd bytes).
char* CeLspFraming_EncodeRequest(int id, const char* method, const CELSP_JSON* params, size_t* outLength);
char* CeLspFraming_EncodeNotification(const char* method, const CELSP_JSON* params, size_t* outLength);
char* CeLspFraming_EncodeResult(int id, const CELSP_JSON* result, size_t* outLength);

#ifdef __cplusplus
}
#endif
