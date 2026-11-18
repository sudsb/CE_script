// encoding: UTF-8
/******************************************************************************
*                                                                             *
* Notepad3                                                                    *
*                                                                             *
* CeCompletion.c                                                              *
*   Cheat Engine script completion, calltips and template expansion.         *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"

#include <commctrl.h>
#include <ctype.h>
#include <string.h>

#include "PathLib.h"
#include "Styles.h"
#include "SciCall.h"
#include "CeCompletion.h"
#include "CeScript.h"
#include "CeSettings.h"
#include "CeTemplate.h"

// ----------------------------------------------------------------------------

#define CE_MEMBERLIST_MAX 1200  // cap for the "show all members" list
#define CE_DICTIONARY_MAX  512

// ----------------------------------------------------------------------------
// Common CE variable/label names.  They are ordinary completion entries, so
// they also appear in the reference panel, the detail pane and the local
// matching pass.  External dictionaries use exactly the same shape.
// ----------------------------------------------------------------------------

static const CE_COMPLETION_ENTRY s_builtinWords[] = {
    { "playerBase",      "playerBase -- 玩家对象基地址",             "常用变量", 0, CE_KIND_VARIABLE },
    { "playerPtr",       "playerPtr -- 玩家对象指针",                 "常用变量", 0, CE_KIND_VARIABLE },
    { "playerHealth",    "playerHealth -- 玩家生命值",               "常用变量", 0, CE_KIND_VARIABLE },
    { "playerHp",        "playerHp -- 玩家生命值",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "playerMana",      "playerMana -- 玩家魔力值",                 "常用变量", 0, CE_KIND_VARIABLE },
    { "playerMoney",     "playerMoney -- 玩家金钱",                  "常用变量", 0, CE_KIND_VARIABLE },
    { "playerGold",      "playerGold -- 玩家金币",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "playerLevel",     "playerLevel -- 玩家等级",                  "常用变量", 0, CE_KIND_VARIABLE },
    { "playerExp",       "playerExp -- 玩家经验值",                  "常用变量", 0, CE_KIND_VARIABLE },
    { "playerSpeed",     "playerSpeed -- 玩家移动速度",              "常用变量", 0, CE_KIND_VARIABLE },
    { "playerPos",       "playerPos -- 玩家坐标结构",               "常用变量", 0, CE_KIND_VARIABLE },
    { "playerX",         "playerX -- 玩家 X 坐标",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "playerY",         "playerY -- 玩家 Y 坐标",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "playerZ",         "playerZ -- 玩家 Z 坐标",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "health",          "health -- 生命值",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "hp",              "hp -- 生命值",                             "常用变量", 0, CE_KIND_VARIABLE },
    { "maxHp",           "maxHp -- 最大生命值",                      "常用变量", 0, CE_KIND_VARIABLE },
    { "mana",            "mana -- 魔力值",                           "常用变量", 0, CE_KIND_VARIABLE },
    { "mp",              "mp -- 魔力值",                             "常用变量", 0, CE_KIND_VARIABLE },
    { "gold",            "gold -- 金币数量",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "coins",           "coins -- 金币数量",                        "常用变量", 0, CE_KIND_VARIABLE },
    { "ammo",            "ammo -- 弹药数量",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "score",           "score -- 分数",                            "常用变量", 0, CE_KIND_VARIABLE },
    { "level",           "level -- 等级",                            "常用变量", 0, CE_KIND_VARIABLE },
    { "exp",             "exp -- 经验值",                            "常用变量", 0, CE_KIND_VARIABLE },
    { "damage",          "damage -- 伤害值",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "cooldown",        "cooldown -- 冷却时间",                     "常用变量", 0, CE_KIND_VARIABLE },
    { "timer",           "timer -- 计时器",                          "常用变量", 0, CE_KIND_VARIABLE },
    { "counter",         "counter -- 计数器",                        "常用变量", 0, CE_KIND_VARIABLE },
    { "flag",            "flag -- 标志位",                           "常用变量", 0, CE_KIND_VARIABLE },
    { "baseAddr",        "baseAddr -- 模块基地址",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "baseAddress",     "baseAddress -- 模块基地址",                "常用变量", 0, CE_KIND_VARIABLE },
    { "moduleBase",      "moduleBase -- 模块基地址",                 "常用变量", 0, CE_KIND_VARIABLE },
    { "hookAddr",        "hookAddr -- 挂钩地址",                     "常用变量", 0, CE_KIND_VARIABLE },
    { "hookAddress",     "hookAddress -- 挂钩地址",                  "常用变量", 0, CE_KIND_VARIABLE },
    { "origBytes",       "origBytes -- 原始字节",                    "常用变量", 0, CE_KIND_VARIABLE },
    { "originalBytes",   "originalBytes -- 原始字节",                "常用变量", 0, CE_KIND_VARIABLE },
    { "originalcode",    "originalcode -- 原始代码标签",             "常用变量", 0, CE_KIND_VARIABLE },
    { "returnhere",      "returnhere -- 返回地址标签",               "常用变量", 0, CE_KIND_VARIABLE },
    { "newmem",          "newmem -- CE 默认分配内存标签",            "常用变量", 0, CE_KIND_VARIABLE },
    { "codeCave",        "codeCave -- 代码洞地址",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "caveAddr",        "caveAddr -- 代码洞地址",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "symbolName",      "symbolName -- 注册符号名",                 "常用变量", 0, CE_KIND_VARIABLE },
    { "symbolAddr",      "symbolAddr -- 注册符号地址",               "常用变量", 0, CE_KIND_VARIABLE },
    { "value",           "value -- 数值",                            "常用变量", 0, CE_KIND_VARIABLE },
    { "oldValue",        "oldValue -- 旧数值",                       "常用变量", 0, CE_KIND_VARIABLE },
    { "newValue",        "newValue -- 新数值",                       "常用变量", 0, CE_KIND_VARIABLE },
    { "address",         "address -- 地址",                          "常用变量", 0, CE_KIND_VARIABLE },
    { "offset",          "offset -- 偏移量",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "size",            "size -- 大小",                             "常用变量", 0, CE_KIND_VARIABLE },
    { "count",           "count -- 计数",                            "常用变量", 0, CE_KIND_VARIABLE },
    { "buffer",          "buffer -- 缓冲区",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "bufferSize",      "bufferSize -- 缓冲区大小",                 "常用变量", 0, CE_KIND_VARIABLE },
    { "index",           "index -- 索引",                            "常用变量", 0, CE_KIND_VARIABLE },
    { "xCoord",          "xCoord -- X 坐标",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "yCoord",          "yCoord -- Y 坐标",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "zCoord",          "zCoord -- Z 坐标",                         "常用变量", 0, CE_KIND_VARIABLE },
    { "floatValue",      "floatValue -- 浮点数值",                   "常用变量", 0, CE_KIND_VARIABLE },
    { "intValue",        "intValue -- 整数值",                       "常用变量", 0, CE_KIND_VARIABLE },
    { "byteValue",       "byteValue -- 字节值",                      "常用变量", 0, CE_KIND_VARIABLE },
    { "aobPattern",      "aobPattern -- AOB 特征码",                 "常用变量", 0, CE_KIND_VARIABLE },
    { "aobResult",       "aobResult -- AOB 扫描结果",                "常用变量", 0, CE_KIND_VARIABLE },
    { "scanAddr",        "scanAddr -- 扫描起始地址",                 "常用变量", 0, CE_KIND_VARIABLE },
    { "allocSize",       "allocSize -- 分配内存大小",                "常用变量", 0, CE_KIND_VARIABLE },
    { "allocAddr",       "allocAddr -- 分配内存地址",                "常用变量", 0, CE_KIND_VARIABLE },
    { "hookLabel",       "hookLabel -- 挂钩标签",                    "常用变量", 0, CE_KIND_VARIABLE },
    { "jumpBack",        "jumpBack -- 跳回标签",                     "常用变量", 0, CE_KIND_VARIABLE },
    { "jumpLabel",       "jumpLabel -- 跳转标签",                    "常用变量", 0, CE_KIND_VARIABLE },
    { "customVar",       "customVar -- 自定义变量",                  "常用变量", 0, CE_KIND_VARIABLE },
    { "customSymbol",    "customSymbol -- 自定义符号",               "常用变量", 0, CE_KIND_VARIABLE },
    { "customLabel",     "customLabel -- 自定义标签",                "常用变量", 0, CE_KIND_VARIABLE },
    { "userVar",         "userVar -- 用户变量",                      "常用变量", 0, CE_KIND_VARIABLE },
    { "tempVar",         "tempVar -- 临时变量",                      "常用变量", 0, CE_KIND_VARIABLE },
    { "tempAddr",        "tempAddr -- 临时地址",                     "常用变量", 0, CE_KIND_VARIABLE },
    { "savedValue",      "savedValue -- 已保存的数值",               "常用变量", 0, CE_KIND_VARIABLE },
    { "savedBytes",      "savedBytes -- 已保存的字节",               "常用变量", 0, CE_KIND_VARIABLE },
    { "isEnabled",       "isEnabled -- 是否启用",                    "常用变量", 0, CE_KIND_VARIABLE },
};

static CE_COMPLETION_ENTRY* s_dictionary = NULL;
// Parallel per-entry usage note (the entry struct itself is generated data).
static char* s_dictionaryDocs[CE_DICTIONARY_MAX];
static int  s_dictionaryCount = 0;
static bool s_dictionaryLoaded = false;

// Bumped whenever the inputs of the completion list change; the list cache
// compares these instead of hashing the whole candidate set.
static unsigned s_docSymGen = 0;
static unsigned s_dictGen = 0;

// SCN_AUTOCCOMPLETED is not guaranteed (a cancelled list short-circuits it).
static bool s_pendingCompletion = false;
static char s_pendingName[240];
static int  s_pendingFillUp = 0;

// Last item highlighted in the list.  Some Scintilla/accelerator paths accept
// the first item instead of the arrow-key selection; this lets the completion
// handler honour what the user actually saw highlighted.
static char s_lastHighlight[240];

static const char* _SubstringCI(const char* text, const char* needle)
{
    if (!text || !needle || !*needle) {
        return text;
    }
    for (const char* p = text; *p; ++p) {
        const char* a = p;
        const char* b = needle;
        while (*a && *b && (tolower((unsigned char)*a) == tolower((unsigned char)*b))) {
            ++a;
            ++b;
        }
        if (!*b) {
            return p;
        }
    }
    return NULL;
}

static bool _MatchesRoot(const char* text, const char* root)
{
    return (_SubstringCI(text, root) != NULL);
}

// A candidate stays in the list until the typed root is longer than the word:
// typing "alloc" must still offer "alloc" so Tab can accept it (and append the
// parentheses).  _MatchesRoot() is substring based, so a longer root can never
// match anyway; the explicit length check keeps that rule obvious and cheap.
static bool _CandidateMatches(const char* name, const char* root, size_t rootLen)
{
    return (strlen(name) >= rootLen) && _MatchesRoot(name, root);
}

static int _MatchBonus(const char* name, const char* root, size_t rootLen)
{
    if (rootLen == 0) {
        return 0;
    }
    if (_strnicmp(name, root, rootLen) == 0) {
        return 320; // exact prefix
    }
    const char* const hit = _SubstringCI(name, root);
    if (!hit) {
        return -1000;
    }
    if (hit == name) {
        return 320;
    }
    char const prev = hit[-1];
    return ((prev == '_') || (prev == '.') || (prev == '$')) ? 170 : 0;
}

// ----------------------------------------------------------------------------
// Display-only Chinese glossary: the generated catalogue keeps its English
// signatures, but the detail pane and calltips show a Chinese rendering of
// the "--" description while parameter names are left untouched.
// ----------------------------------------------------------------------------

typedef struct _ce_gloss {
    const char* en;
    const char* zh;
} CE_GLOSS;

static const CE_GLOSS s_gloss[] = {
    { "adds", "添加" }, { "add", "添加" }, { "multiple", "多个" },
    { "strings", "字符串" }, { "string", "字符串" }, { "once", "一次" },
    { "inserts", "插入" }, { "insert", "插入" }, { "given", "指定的" },
    { "index", "索引" }, { "returns", "返回" }, { "return", "返回" },
    { "gets", "获取" }, { "get", "获取" }, { "sets", "设置" }, { "set", "设置" },
    { "value", "值" }, { "values", "值" }, { "address", "地址" },
    { "addresses", "地址" }, { "size", "大小" }, { "offset", "偏移" },
    { "memory", "内存" }, { "process", "进程" }, { "thread", "线程" },
    { "symbol", "符号" }, { "symbols", "符号" }, { "register", "寄存器" },
    { "registers", "寄存器" }, { "function", "函数" }, { "functions", "函数" },
    { "table", "表" }, { "list", "列表" }, { "array", "数组" },
    { "number", "数字" }, { "integer", "整数" }, { "float", "浮点数" },
    { "double", "双精度" }, { "byte", "字节" }, { "bytes", "字节" },
    { "word", "字" }, { "dword", "双字" }, { "qword", "四字" },
    { "boolean", "布尔值" }, { "true", "真" }, { "false", "假" },
    { "current", "当前" }, { "selected", "选中的" }, { "active", "活动的" },
    { "open", "打开" }, { "close", "关闭" }, { "create", "创建" },
    { "delete", "删除" }, { "remove", "移除" }, { "clear", "清除" },
    { "read", "读取" }, { "write", "写入" }, { "writes", "写入" },
    { "find", "查找" }, { "search", "搜索" }, { "scan", "扫描" },
    { "show", "显示" }, { "hide", "隐藏" }, { "enable", "启用" },
    { "disable", "禁用" }, { "wait", "等待" }, { "sleep", "休眠" },
    { "start", "开始" }, { "stop", "停止" }, { "pause", "暂停" },
    { "load", "加载" }, { "save", "保存" }, { "reload", "重载" },
    { "copy", "复制" }, { "paste", "粘贴" }, { "convert", "转换" },
    { "execute", "执行" }, { "run", "运行" }, { "call", "调用" },
    { "script", "脚本" }, { "code", "代码" }, { "text", "文本" },
    { "name", "名称" }, { "type", "类型" }, { "result", "结果" },
    { "results", "结果" }, { "data", "数据" }, { "buffer", "缓冲区" },
    { "length", "长度" }, { "count", "数量" }, { "position", "位置" },
    { "from", "从" }, { "to", "到" }, { "at", "在" }, { "the", "" },
    { "a", "一个" }, { "an", "一个" }, { "of", "的" }, { "in", "在" },
    { "into", "到" }, { "with", "使用" }, { "without", "不使用" },
    { "for", "用于" }, { "and", "和" }, { "or", "或" }, { "if", "如果" },
    { "is", "是" }, { "are", "是" }, { "will", "将" }, { "can", "可以" },
    { "be", "被" }, { "this", "此" }, { "that", "该" }, { "all", "所有" },
    { "new", "新建" }, { "old", "旧的" }, { "default", "默认" },
    { "specified", "指定的" }, { "optional", "可选" }, { "parameter", "参数" },
    { "parameters", "参数" }, { "object", "对象" }, { "pointer", "指针" },
    { "handle", "句柄" }, { "module", "模块" }, { "file", "文件" },
    { "window", "窗口" }, { "message", "消息" }, { "error", "错误" },
    { "success", "成功" }, { "failed", "失败" }, { "failure", "失败" },
    { "available", "可用" }, { "valid", "有效" }, { "invalid", "无效" },
    { "true", "真" }, { "false", "假" }, { "nil", "空值" },
    { "client", "客户端" }, { "server", "服务端" }, { "connect", "连接" },
    { "connection", "连接" }, { "disconnect", "断开" }, { "split", "分隔" },
    { "format", "格式" }, { "formatted", "格式化" }, { "lines", "行" },
    { "line", "行" }, { "item", "项" }, { "items", "项" },
    { "first", "首个" }, { "last", "最后一个" }, { "next", "下一个" },
    { "previous", "上一个" }, { "given", "给定的" }, { "specified", "指定的" },
    { "after", "之后" }, { "before", "之前" }, { "when", "当" },
    { "while", "当" }, { "until", "直到" }, { "then", "然后" },
    { "only", "仅" }, { "also", "也" }, { "both", "两者" },
    { "single", "单个" }, { "multi", "多个" }, { "user", "用户" },
    { "custom", "自定义" }, { "internal", "内部" }, { "external", "外部" },
    { "value", "值" }, { "pointer", "指针" }, { "signature", "签名" },
};

static const char* _LocalizeSignature(const CE_COMPLETION_ENTRY* pEntry, char* out, size_t cch)
{
    if (!pEntry || StrIsEmptyA(pEntry->signature)) {
        return "";
    }
    const char* const sig = pEntry->signature;
    const char* const sep = strstr(sig, " -- ");
    if (!sep) {
        return sig;
    }
    size_t const headLen = (size_t)(sep - sig);
    if (headLen + 5 >= cch) {
        return sig;
    }
    memcpy(out, sig, headLen);
    out[headLen] = '\0';
    StringCchCatA(out, cch, " -- ");
    size_t o = strlen(out);
    const char* p = sep + 4;
    char word[128];
    while (*p) {
        unsigned char const ch = (unsigned char)*p;
        if ((ch < 0x80) && (isalnum(ch) || *p == '_')) {
            size_t n = 0;
            while (((unsigned char)*p < 0x80) &&
                   (isalnum((unsigned char)*p) || *p == '_') && (n + 1 < COUNTOF(word))) {
                word[n++] = *p++;
            }
            word[n] = '\0';
            const char* add = word;
            for (int i = 0; i < (int)COUNTOF(s_gloss); ++i) {
                if (_stricmp(s_gloss[i].en, word) == 0) {
                    add = s_gloss[i].zh;
                    break;
                }
            }
            size_t const addLen = strlen(add);
            if (o + addLen + 1 >= cch) {
                break;
            }
            memcpy(out + o, add, addLen);
            o += addLen;
            out[o] = '\0';
        }
        else {
            if (o + 2 >= cch) {
                break;
            }
            out[o++] = *p++;
            out[o] = '\0';
        }
    }
    return out;
}

static bool _IsIdentifierLike(const char* pszName)
{
    if (StrIsEmptyA(pszName)) {
        return false;
    }
    if (!(isalpha((unsigned char)pszName[0]) || pszName[0] == '_')) {
        return false;
    }
    for (const char* p = pszName; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == ':' || *p == '$')) {
            return false;
        }
    }
    return true;
}

// Section markers such as {$lua}/{$asm}/{$ccode} are catalogue entries but not
// identifier-shaped; they are offered while typing the "{$" opener.
static bool _IsSectionMarkerName(const char* pszName)
{
    if (StrIsEmptyA(pszName) || (pszName[0] != '{') || (pszName[1] != '$')) {
        return false;
    }
    const size_t len = strlen(pszName);
    return (len > 3u) && (pszName[len - 1u] == '}');
}

static bool _IsCompletableName(const char* pszName)
{
    return _IsIdentifierLike(pszName) || _IsSectionMarkerName(pszName);
}

static bool _IsIdentifier(const char* pszName)
{
    if (StrIsEmptyA(pszName)) {
        return false;
    }
    if (!(isalpha((unsigned char)pszName[0]) || pszName[0] == '_')) {
        return false;
    }
    for (const char* p = pszName + 1; *p; ++p) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) {
            return false;
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// Pre-parsed lookup tables
//
// The five generated keyword lists are ~23-25 KB of space separated text and
// the catalogue holds 2146 entries.  Completion ranking touches both on every
// keystroke, so each is parsed once at init into a sorted array and the hot
// paths become binary searches.  Anything not registered here (a caller passing
// some other list) still falls back to the original linear scan.
// ----------------------------------------------------------------------------

typedef struct _ce_kw_set {
    const char* list;   // identity of the generated CE_KEYWORDS_* string
    char**      words;
    int         count;
} CE_KW_SET;

static CE_KW_SET s_kwSets[5];
static int       s_kwSetCount = 0;

static const CE_COMPLETION_ENTRY** s_catalogueByName = NULL;

static int __cdecl _CompareKwWord(const void* pA, const void* pB)
{
    return _stricmp(*(const char* const*)pA, *(const char* const*)pB);
}

static int __cdecl _CompareEntryPtr(const void* pA, const void* pB)
{
    const CE_COMPLETION_ENTRY* const a = *(const CE_COMPLETION_ENTRY* const*)pA;
    const CE_COMPLETION_ENTRY* const b = *(const CE_COMPLETION_ENTRY* const*)pB;
    return strcmp(a->name, b->name);
}

static void _FreeKeywordSets(void)
{
    for (int i = 0; i < s_kwSetCount; ++i) {
        for (int j = 0; j < s_kwSets[i].count; ++j) {
            FreeMem(s_kwSets[i].words[j]);
        }
        FreeMem(s_kwSets[i].words);
        s_kwSets[i].words = NULL;
        s_kwSets[i].count = 0;
    }
    s_kwSetCount = 0;
}

static void _AddKeywordSet(const char* list)
{
    if (!list || (s_kwSetCount >= (int)COUNTOF(s_kwSets))) {
        return;
    }
    int words = 0;
    for (const char* p = list; *p;) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            break;
        }
        while (*p && (*p != ' ')) {
            ++p;
        }
        ++words;
    }
    if (words == 0) {
        return;
    }
    char** const array = AllocMem(sizeof(char*) * (size_t)words, HEAP_ZERO_MEMORY);
    if (!array) {
        return;
    }
    int n = 0;
    for (const char* p = list; *p;) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            break;
        }
        const char* const start = p;
        while (*p && (*p != ' ')) {
            ++p;
        }
        size_t const len = (size_t)(p - start);
        char* const copy = AllocMem(len + 1, HEAP_ZERO_MEMORY);
        if (!copy) {
            break;
        }
        memcpy(copy, start, len);
        copy[len] = '\0';
        array[n++] = copy;
    }
    qsort(array, (size_t)n, sizeof(char*), _CompareKwWord);
    s_kwSets[s_kwSetCount].list = list;
    s_kwSets[s_kwSetCount].words = array;
    s_kwSets[s_kwSetCount].count = n;
    ++s_kwSetCount;
}

static void _BuildKeywordSets(void)
{
    _FreeKeywordSets();
    _AddKeywordSet(CE_KEYWORDS_LUA);
    _AddKeywordSet(CE_KEYWORDS_API);
    _AddKeywordSet(CE_KEYWORDS_AA);
    _AddKeywordSet(CE_KEYWORDS_ASM);
    _AddKeywordSet(CE_KEYWORDS_TYPES);
}

static void _BuildCatalogueIndex(void)
{
    FreeMem(s_catalogueByName);
    s_catalogueByName = NULL;
    if (g_ceCompletionCount <= 0) {
        return;
    }
    size_t const bytes = sizeof(*s_catalogueByName) * (size_t)g_ceCompletionCount;
    const CE_COMPLETION_ENTRY** const index = AllocMem(bytes, HEAP_ZERO_MEMORY);
    if (!index) {
        return;
    }
    for (int i = 0; i < g_ceCompletionCount; ++i) {
        index[i] = &g_ceCompletions[i];
    }
    qsort(index, (size_t)g_ceCompletionCount, sizeof(*index), _CompareEntryPtr);
    s_catalogueByName = index;
}

// Generated keyword lists are space separated; a CE API/AA command that has
// no catalogue entry still needs parentheses when it is accepted.
static bool _InKeywordListLinear(const char* pszList, const char* pszWord)
{
    const char* p = pszList;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char* const start = p;
        while (*p && *p != ' ') {
            ++p;
        }
        if (((size_t)(p - start) == strlen(pszWord)) &&
            (_strnicmp(start, pszWord, (size_t)(p - start)) == 0)) {
            return true;
        }
    }
    return false;
}

static bool _InKeywordList(const char* pszList, const char* pszWord)
{
    if (!pszList || !pszWord || !*pszWord) {
        return false;
    }
    for (int i = 0; i < s_kwSetCount; ++i) {
        if (s_kwSets[i].list == pszList) {
            int lo = 0;
            int hi = s_kwSets[i].count - 1;
            while (lo <= hi) {
                int const mid = lo + ((hi - lo) / 2);
                int const cmp = _stricmp(s_kwSets[i].words[mid], pszWord);
                if (cmp == 0) {
                    return true;
                }
                if (cmp < 0) {
                    lo = mid + 1;
                }
                else {
                    hi = mid - 1;
                }
            }
            return false;
        }
    }
    return _InKeywordListLinear(pszList, pszWord);
}

static bool _LooksLikeCall(const char* pszName)
{
    static const char* const s_aaCalls[] = {
        "alloc", "globalalloc", "sharedalloc", "dealloc", "label",
        "registersymbol", "unregistersymbol", "define", "aobscan",
        "aobscanmodule", "aobscanregion", "aobscanpattern", "fullaccess",
        "assert", "createthread", "createthreadandwait", "loadlibrary",
        "include", "luacall", "readmem", "loadbinary", "kalloc",
        "reassemble", "savenop", "exports", "unprotect",
        "createthreadandwaitfinish",
    };
    if (!_IsIdentifierLike(pszName) || _InKeywordList(CE_KEYWORDS_LUA, pszName) ||
        _InKeywordList(CE_KEYWORDS_ASM, pszName) || _InKeywordList(CE_KEYWORDS_TYPES, pszName)) {
        return false;
    }
    for (int i = 0; i < (int)COUNTOF(s_aaCalls); ++i) {
        if (_stricmp(pszName, s_aaCalls[i]) == 0) {
            return true;
        }
    }
    return _InKeywordList(CE_KEYWORDS_API, pszName);
}

// ----------------------------------------------------------------------------

bool CeCompletion_IsActive(void)
{
    return (Style_GetCurrentLexerPtr() == &lexCE);
}

// ----------------------------------------------------------------------------
// External dictionary: UTF-8, one "name|signature|category" entry per line.
// Lines starting with '#' or ';' are comments; a missing signature/category
// is filled in.  Empty CeDictionaryPath means <module dir>\CeDictionary.txt.
// ----------------------------------------------------------------------------

static char* _DupStr(const char* src)
{
    size_t const n = strlen(src) + 1;
    char* const p = AllocMem(n, HEAP_ZERO_MEMORY);
    if (p) {
        memcpy(p, src, n);
    }
    return p;
}

static char* _TrimA(char* s)
{
    while ((*s == ' ') || (*s == '\t')) {
        ++s;
    }
    size_t n = strlen(s);
    while ((n > 0) && ((s[n - 1] == ' ') || (s[n - 1] == '\t') || (s[n - 1] == '\r'))) {
        s[--n] = '\0';
    }
    return s;
}

static void _FreeDictionary(void)
{
    for (int i = 0; i < s_dictionaryCount; ++i) {
        FreeMem((LPVOID)s_dictionary[i].name);
        FreeMem((LPVOID)s_dictionary[i].signature);
        FreeMem((LPVOID)s_dictionary[i].category);
        FreeMem(s_dictionaryDocs[i]);
        s_dictionaryDocs[i] = NULL;
    }
    FreeMem(s_dictionary);
    s_dictionary = NULL;
    s_dictionaryCount = 0;
}

// Dictionary notes are single-line by nature; "\n" (two characters) is turned
// into a real line break so the detail pane can show the usual multi-angle
// sections from one dictionary line.
static char* _UnescapeDoc(const char* src)
{
    size_t const len = strlen(src);
    char* const out = AllocMem(len + 1, HEAP_ZERO_MEMORY);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; ++i) {
        if ((src[i] == '\\') && (src[i + 1] == 'n')) {
            out[o++] = '\n';
            ++i;
        }
        else {
            out[o++] = src[i];
        }
    }
    out[o] = '\0';
    return out;
}

static void _AddDictionaryWord(const char* name, const char* sig, const char* cat, const char* doc)
{
    if (StrIsEmptyA(name) || !_IsIdentifierLike(name) || (s_dictionaryCount >= CE_DICTIONARY_MAX)) {
        return;
    }
    for (int i = 0; i < s_dictionaryCount; ++i) {
        if (_stricmp(s_dictionary[i].name, name) == 0) {
            return;
        }
    }
    char signature[512];
    if (StrIsEmptyA(sig)) {
        StringCchPrintfA(signature, COUNTOF(signature), "%s -- 自定义词典词条", name);
    }
    else {
        StringCchCopyA(signature, COUNTOF(signature), sig);
    }
    CE_COMPLETION_ENTRY* const e = &s_dictionary[s_dictionaryCount];
    s_dictionaryDocs[s_dictionaryCount] = NULL;
    e->name = _DupStr(name);
    e->signature = _DupStr(signature);
    e->category = _DupStr(StrIsEmptyA(cat) ? "自定义词典" : cat);
    if (!e->name || !e->signature || !e->category) {
        FreeMem((LPVOID)e->name);
        FreeMem((LPVOID)e->signature);
        FreeMem((LPVOID)e->category);
        ZeroMemory(e, sizeof(*e));
        return;
    }
    e->isCall = (strchr(signature, '(') != NULL) ? 1 : 0;
    e->kind = CE_KIND_VARIABLE;
    s_dictionaryDocs[s_dictionaryCount] = StrIsEmptyA(doc) ? NULL : _UnescapeDoc(doc);
    ++s_dictionaryCount;
}

static void _LoadDictionary(void)
{
    _FreeDictionary();
    ++s_dictGen;
    s_dictionaryLoaded = true;

    WCHAR path[MAX_PATH];
    LPCWSTR const configured = CeSettings_DictionaryPath();
    if (configured && configured[0]) {
        StringCchCopyW(path, COUNTOF(path), configured);
    }
    else {
        StringCchPrintfW(path, COUNTOF(path), L"%s\\CeDictionary.txt", Path_Get(Paths.ModuleDirectory));
    }
    HANDLE const hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD const size = GetFileSize(hFile, NULL);
    if ((size == 0) || (size == INVALID_FILE_SIZE) || (size > (1024 * 1024))) {
        CloseHandle(hFile);
        return;
    }
    char* const data = AllocMem((size_t)size + 3, HEAP_ZERO_MEMORY);
    if (!data) {
        CloseHandle(hFile);
        return;
    }
    DWORD read = 0;
    BOOL const ok = ReadFile(hFile, data, size, &read, NULL);
    CloseHandle(hFile);
    if (!ok) {
        FreeMem(data);
        return;
    }
    data[read] = '\0';

    s_dictionary = AllocMem(sizeof(CE_COMPLETION_ENTRY) * CE_DICTIONARY_MAX, HEAP_ZERO_MEMORY);
    if (!s_dictionary) {
        FreeMem(data);
        return;
    }

    char* p = data;
    if ((read >= 3) && ((BYTE)data[0] == 0xEF) && ((BYTE)data[1] == 0xBB) && ((BYTE)data[2] == 0xBF)) {
        p += 3; // UTF-8 BOM
    }
    while (*p) {
        char* const end = strchr(p, '\n');
        char* const stop = end ? end : (p + strlen(p));
        char line[1024];
        size_t const len = min_s((size_t)(stop - p), COUNTOF(line) - 1);
        memcpy(line, p, len);
        line[len] = '\0';
        p = end ? (end + 1) : stop;

        char* const first = _TrimA(line);
        if (!first[0] || (first[0] == '#') || (first[0] == ';')) {
            continue;
        }
        char* bar1 = strchr(first, '|');
        char* f1 = bar1 ? _TrimA(bar1 + 1) : NULL;
        if (bar1) {
            *bar1 = '\0';
        }
        char* bar2 = f1 ? strchr(f1, '|') : NULL;
        char* f2 = bar2 ? _TrimA(bar2 + 1) : NULL;
        if (bar2) {
            *bar2 = '\0';
        }
        // Optional 4th field: a usage note (use "\n" for a line break).
        char* bar3 = f2 ? strchr(f2, '|') : NULL;
        char* f3 = bar3 ? _TrimA(bar3 + 1) : NULL;
        if (bar3) {
            *bar3 = '\0';
        }
        _AddDictionaryWord(_TrimA(first), f1, f2, f3);
    }
    FreeMem(data);
}

// ----------------------------------------------------------------------------
// Code-block completions
//
// These are additional candidates, never replacements: every original keyword
// in g_ceCompletions[] stays in the list.  AutoC items are single whitespace
// separated tokens, so a block is offered under a short identifier-like
// trigger and expands into a multi-line skeleton only when it is accepted.
//
// Body syntax: '\n' = new line with the caller's indentation, '\t' = one extra
// indent step, "${N:text}" = placeholder (lowest N is selected), '$0' = final
// caret stop.  Placeholders are plain text, so the result is valid Lua/AA even
// when the user just keeps typing.
// ----------------------------------------------------------------------------

#define CE_BLOCK_LUA  (1 << CE_LANG_LUA)
#define CE_BLOCK_AA   (1 << CE_LANG_AA)
#define CE_BLOCK_C    (1 << CE_LANG_C)
// {$lua}/{$asm}/{$c} switch the language of the following lines, so their
// skeletons must be offered in every region -- including from inside a Lua or
// inline-C block, where the directive is the only way back to AA.
#define CE_BLOCK_ALL  (CE_BLOCK_LUA | CE_BLOCK_AA | CE_BLOCK_C)

typedef struct _ce_code_block {
    CE_COMPLETION_ENTRY entry;
    const char*         body;
    int                 modeMask;
} CE_CODE_BLOCK;

static const CE_CODE_BLOCK s_codeBlocks[] = {
    { { "ifBlock",       "if ... then ... end 代码块",       "Lua 代码块", 0, CE_KIND_KEYWORD },
      "if ${1:condition} then\n\t$0\nend", CE_BLOCK_LUA },
    { { "ifElseBlock",   "if ... then ... else ... end 代码块", "Lua 代码块", 0, CE_KIND_KEYWORD },
      "if ${1:condition} then\n\t\nelse\n\t$0\nend", CE_BLOCK_LUA },
    { { "forBlock",      "for i = 1, 10 do ... end 代码块", "Lua 代码块", 0, CE_KIND_KEYWORD },
      "for ${1:i} = 1, 10 do\n\t$0\nend", CE_BLOCK_LUA },
    { { "forPairsBlock", "for k, v in pairs(t) do ... end 代码块", "Lua 代码块", 0, CE_KIND_KEYWORD },
      "for k, v in pairs(${1:t}) do\n\t$0\nend", CE_BLOCK_LUA },
    { { "whileBlock",    "while ... do ... end 代码块",     "Lua 代码块", 0, CE_KIND_KEYWORD },
      "while ${1:condition} do\n\t$0\nend", CE_BLOCK_LUA },
    { { "repeatBlock",   "repeat ... until ... 代码块",     "Lua 代码块", 0, CE_KIND_KEYWORD },
      "repeat\n\t$0\nuntil ${1:condition}", CE_BLOCK_LUA },
    { { "funcBlock",     "local function name(...) ... end 代码块", "Lua 代码块", 0, CE_KIND_KEYWORD },
      "local function ${1:name}(...)\n\t$0\nend", CE_BLOCK_LUA },
    { { "methodBlock",   "function obj:method(...) ... end 代码块", "Lua 代码块", 0, CE_KIND_KEYWORD },
      "function ${1:obj}:method(...)\n\t$0\nend", CE_BLOCK_LUA },
    { { "tableBlock",    "local t = { ... } 表构造代码块",  "Lua 代码块", 0, CE_KIND_KEYWORD },
      "local ${1:t} = {\n\t$0\n}", CE_BLOCK_LUA },
    { { "pcallBlock",    "pcall(function() ... end) 保护调用代码块", "Lua 代码块", 0, CE_KIND_KEYWORD },
      "local ok, err = pcall(function()\n\t$0\nend)\nif not ok then print(err) end", CE_BLOCK_LUA },
    { { "enableBlock",   "[ENABLE] / [DISABLE] 结构代码块", "AA 代码块", 0, CE_KIND_AA },
      "[ENABLE]\n{$asm}\n$0\n\n[DISABLE]\n{$asm}\n", CE_BLOCK_ALL },
    { { "luaBlock",      "{$lua} ... {$asm} 切换代码块",    "AA 代码块", 0, CE_KIND_SECTION },
      "{$lua}\n$0\n{$asm}\n", CE_BLOCK_ALL },
    { { "{$lua}Block",   "{$lua} ... {$asm} 成对代码块",    "AA 代码块", 0, CE_KIND_SECTION },
      "{$lua}\n$0\n{$asm}\n", CE_BLOCK_ALL },
    { { "{$asm}Block",   "{$lua} ... {$asm} 成对代码块",    "AA 代码块", 0, CE_KIND_SECTION },
      "{$lua}\n$0\n{$asm}\n", CE_BLOCK_ALL },
};

static const CE_CODE_BLOCK* _FindCodeBlock(const char* name)
{
    if (StrIsEmptyA(name)) {
        return NULL;
    }
    for (int i = 0; i < (int)COUNTOF(s_codeBlocks); ++i) {
        if (_stricmp(s_codeBlocks[i].entry.name, name) == 0) {
            return &s_codeBlocks[i];
        }
    }
    return NULL;
}

bool CeCompletion_IsCodeBlockName(const char* pszName)
{
    return _FindCodeBlock(pszName) != NULL;
}

static void _BlockAppend(char* out, size_t cchOut, size_t* n, const char* text, size_t len)
{
    if (!text) {
        return;
    }
    for (size_t i = 0; (i < len) && ((*n + 1) < cchOut); ++i) {
        out[(*n)++] = text[i];
    }
    out[*n] = '\0';
}

// Pure renderer: no Scintilla calls, so the expansion is unit testable.
static void _RenderCodeBlock(const char* body, const char* indent, const char* eol,
                             const char* indentStep, char* out, size_t cchOut,
                             int* selStart, int* selEnd)
{
    if (selStart) { *selStart = -1; }
    if (selEnd) { *selEnd = -1; }
    if (!out || (cchOut == 0)) {
        return;
    }
    out[0] = '\0';
    if (!body) {
        return;
    }
    size_t n = 0;
    int best = 1000000;
    const char* p = body;
    while (*p && ((n + 1) < cchOut)) {
        if (*p == '\n') {
            _BlockAppend(out, cchOut, &n, eol ? eol : "\r\n", eol ? strlen(eol) : 2);
            _BlockAppend(out, cchOut, &n, indent, indent ? strlen(indent) : 0);
            ++p;
            continue;
        }
        if (*p == '\t') {
            _BlockAppend(out, cchOut, &n, indentStep, indentStep ? strlen(indentStep) : 0);
            ++p;
            continue;
        }
        if ((p[0] == '$') && (p[1] == '{')) {
            const char* const close = strchr(p + 2, '}');
            const char* q = p + 2;
            int num = 0;
            bool haveNum = false;
            while ((*q >= '0') && (*q <= '9')) {
                num = (num * 10) + (*q - '0');
                ++q;
                haveNum = true;
            }
            if (close && haveNum && (*q == ':')) {
                const char* const text = q + 1;
                size_t const start = n;
                _BlockAppend(out, cchOut, &n, text, (size_t)(close - text));
                int const priority = (num == 0) ? 0 : num;
                if (priority < best) {
                    best = priority;
                    if (selStart) { *selStart = (int)start; }
                    if (selEnd) { *selEnd = (int)n; }
                }
                p = close + 1;
                continue;
            }
        }
        if ((p[0] == '$') && (p[1] == '0') &&
            !((p[2] >= '0') && (p[2] <= '9'))) { // only reject "$01"
            if (999 < best) {
                best = 999;
                if (selStart) { *selStart = (int)n; }
                if (selEnd) { *selEnd = (int)n; }
            }
            p += 2;
            continue;
        }
        out[n++] = *p++;
        out[n] = '\0';
    }
}

// Surrounding-code context for the word being completed.  A block skeleton is
// only meaningful where a statement may start; offering it after '.'/':' or
// inside a table constructor is exactly the kind of noise that makes the list
// feel inaccurate.
typedef struct _ce_stmt_ctx {
    bool atStatementStart;
    bool afterMember;
    bool inTable;
} CE_STMT_CTX;

static CE_STMT_CTX _StatementContext(const char* prefix, size_t len)
{
    CE_STMT_CTX ctx = { true, false, false };
    int braces = 0;
    char last = '\0';
    for (size_t i = 0; i < len; ++i) {
        char const ch = prefix[i];
        if ((ch == ' ') || (ch == '\t') || (ch == '\r')) {
            continue;
        }
        if (ch == '{') {
            ++braces;
        }
        else if ((ch == '}') && (braces > 0)) {
            --braces;
        }
        last = ch;
    }
    ctx.inTable = (braces > 0);
    ctx.afterMember = (last == '.') || (last == ':');
    ctx.atStatementStart =
        (last == '\0') || (last == ';') || (last == '{') || (last == '}') ||
        (last == '(') || (last == ',') || (last == '=');
    return ctx;
}

// Shared by the local word list and the LSP extra-word merge so both paths
// offer skeletons under exactly the same conditions.
static bool _CodeBlocksAllowed(DocPos rootStart, size_t rootLen, int mode)
{
    if ((rootLen < 2) || !((mode == (int)CE_LANG_LUA) || (mode == (int)CE_LANG_AA) ||
                           (mode == (int)CE_LANG_C))) {
        return false;
    }
    DocLn const line = SciCall_LineFromPosition(rootStart);
    DocPos const lineStart = SciCall_PositionFromLine(line);
    CE_STMT_CTX ctx = { true, false, false };
    if (rootStart > lineStart) {
        const char* const p = SciCall_GetRangePointer(lineStart, rootStart - lineStart);
        if (p) {
            ctx = _StatementContext(p, (size_t)(rootStart - lineStart));
        }
    }
    return ctx.atStatementStart && !ctx.afterMember && !ctx.inTable;
}

static bool _IsLuaBlockCloser(const char* name)
{
    return (_stricmp(name, "end") == 0) || (_stricmp(name, "until") == 0) ||
           (_stricmp(name, "else") == 0) || (_stricmp(name, "elseif") == 0);
}

void CeCompletion_ReloadDictionary(void)
{
    _LoadDictionary();
    CeCompletion_InvalidateDocSymbols();
}

bool CeCompletion_IsBuiltinKeyword(const char* pszName)
{
    if (StrIsEmptyA(pszName)) {
        return false;
    }
    for (int i = 0; i < s_dictionaryCount; ++i) {
        if (_stricmp(s_dictionary[i].name, pszName) == 0) {
            return false;
        }
    }
    for (int i = 0; i < (int)COUNTOF(s_builtinWords); ++i) {
        if (_stricmp(s_builtinWords[i].name, pszName) == 0) {
            return false;
        }
    }
    if (CeCompletion_Find(pszName)) {
        return true;
    }
    return _InKeywordList(CE_KEYWORDS_LUA, pszName) || _InKeywordList(CE_KEYWORDS_API, pszName) ||
           _InKeywordList(CE_KEYWORDS_AA, pszName) || _InKeywordList(CE_KEYWORDS_ASM, pszName) ||
           _InKeywordList(CE_KEYWORDS_TYPES, pszName);
}

const CE_COMPLETION_ENTRY* CeCompletion_Find(const char* pszName)
{
    if (StrIsEmptyA(pszName)) {
        return NULL;
    }
    // Code blocks are not in the generated catalogue, but the detail pane and
    // the "is this a known word?" checks must still recognise them.
    const CE_CODE_BLOCK* const block = _FindCodeBlock(pszName);
    if (block) {
        return &block->entry;
    }
    for (int i = 0; i < s_dictionaryCount; ++i) {
        if (strcmp(s_dictionary[i].name, pszName) == 0) {
            return &s_dictionary[i];
        }
    }
    if (s_catalogueByName) {
        int lo = 0;
        int hi = g_ceCompletionCount - 1;
        while (lo <= hi) {
            int const mid = lo + ((hi - lo) / 2);
            int const cmp = strcmp(s_catalogueByName[mid]->name, pszName);
            if (cmp == 0) {
                return s_catalogueByName[mid];
            }
            if (cmp < 0) {
                lo = mid + 1;
            }
            else {
                hi = mid - 1;
            }
        }
    }
    else {
        // Init has not run yet (settings load, unit tests).
        for (int i = 0; i < g_ceCompletionCount; ++i) {
            if (strcmp(g_ceCompletions[i].name, pszName) == 0) {
                return &g_ceCompletions[i];
            }
        }
    }
    for (int i = 0; i < (int)COUNTOF(s_builtinWords); ++i) {
        if (strcmp(s_builtinWords[i].name, pszName) == 0) {
            return &s_builtinWords[i];
        }
    }
    return NULL;
}

const char* CeCompletion_FindDoc(const char* pszName)
{
    if (StrIsEmptyA(pszName)) {
        return NULL;
    }
    int lo = 0;
    int hi = g_ceCompletionDocCount - 1;
    while (lo <= hi) {
        int const mid = lo + ((hi - lo) / 2);
        int const cmp = strcmp(g_ceCompletionDocs[mid].name, pszName);
        if (cmp == 0) {
            return g_ceCompletionDocs[mid].doc;
        }
        if (cmp < 0) {
            lo = mid + 1;
        }
        else {
            hi = mid - 1;
        }
    }
    // Optional 4th dictionary field: user notes for user words.
    for (int i = 0; i < s_dictionaryCount; ++i) {
        if (strcmp(s_dictionary[i].name, pszName) == 0) {
            return s_dictionaryDocs[i];
        }
    }
    return NULL;
}

// ----------------------------------------------------------------------------
// The unified CE word list is built below the document-symbol scanner: it needs
// _ScanDefinitions() to collect user-defined variables from the current file.
// ----------------------------------------------------------------------------

static bool _ReadNameBefore(DocPos pos, char* pszName, size_t cchName, DocPos* pStart)
{
    DocPos p = pos;
    while (p > 0 && isspace((unsigned char)SciCall_GetCharAt(p - 1))) {
        --p;
    }
    DocPos const end = p;
    while (p > 0) {
        char const ch = SciCall_GetCharAt(p - 1);
        if (isalnum((unsigned char)ch) || ch == '_' || ch == '.' || ch == ':' || ch == '$') {
            --p;
        } else {
            break;
        }
    }
    if (p == end) {
        return false;
    }
    size_t len = (size_t)(end - p);
    if (len >= cchName) {
        len = cchName - 1;
    }
    for (size_t i = 0; i < len; ++i) {
        pszName[i] = SciCall_GetCharAt(p + (DocPos)i);
    }
    pszName[len] = '\0';
    *pStart = p;
    return true;
}

static void _ShowCallTip(const CE_COMPLETION_ENTRY* pEntry, DocPos pos)
{
    char localized[2048];
    const char* const sig = _LocalizeSignature(pEntry, localized, COUNTOF(localized));
    SciCall_CallTipShow(pos, sig);
    const char* const hit = strstr(sig, pEntry->name);
    if (hit) {
        int const beg = (int)(hit - sig);
        SciCall_CallTipSetHlt(beg, beg + (int)strlen(pEntry->name));
    }
}

static bool _ShowCallTipForFunctionBeforeCaret(void)
{
    DocPos const pos = SciCall_GetCurrentPos();
    if (pos <= 0 || SciCall_GetCharAt(pos - 1) != '(') {
        return false;
    }
    char name[MAX_PATH];
    DocPos start = 0;
    if (!_ReadNameBefore(pos - 1, name, COUNTOF(name), &start)) {
        return false;
    }
    const CE_COMPLETION_ENTRY* const pEntry = CeCompletion_Find(name);
    if (!pEntry || !pEntry->isCall) {
        return false;
    }
    _ShowCallTip(pEntry, start);
    return true;
}

static int __cdecl _CompareEntries(const void* pA, const void* pB)
{
    const CE_COMPLETION_ENTRY* const a = *(const CE_COMPLETION_ENTRY* const*)pA;
    const CE_COMPLETION_ENTRY* const b = *(const CE_COMPLETION_ENTRY* const*)pB;
    return _stricmp(a->name, b->name);
}

static bool _ShowAllMembers(void)
{
    if (SciCall_IsIMEModeCJK()) {
        return false;
    }

    static const char* const s_nonMembers[] = {
        "Lua 关键字", "表脚本标记", "汇编指令", "寄存器", "虚拟键码",
        "汇编尺寸", "运算符", "条件断点", "键鼠",
    };

    const CE_COMPLETION_ENTRY** ppEntries =
        AllocMem(sizeof(CE_COMPLETION_ENTRY*) * (size_t)g_ceCompletionCount, HEAP_ZERO_MEMORY);
    if (!ppEntries) {
        return false;
    }

    int count = 0;
    for (int i = 0; (i < g_ceCompletionCount) && (count < CE_MEMBERLIST_MAX); ++i) {
        const CE_COMPLETION_ENTRY* const pEntry = &g_ceCompletions[i];
        if (!_IsIdentifierLike(pEntry->name)) {
            continue;
        }
        bool skip = false;
        for (int j = 0; j < (int)COUNTOF(s_nonMembers); ++j) {
            if (strcmp(pEntry->category, s_nonMembers[j]) == 0) {
                skip = true;
                break;
            }
        }
        if (!skip) {
            ppEntries[count++] = pEntry;
        }
    }

    bool shown = false;
    if (count > 0) {
        qsort(ppEntries, (size_t)count, sizeof(CE_COMPLETION_ENTRY*), _CompareEntries);
        size_t size = 1;
        for (int i = 0; i < count; ++i) {
            size += (strlen(ppEntries[i]->name) + 4); // separator + name + "?N"
        }
        char* const pList = AllocMem(size, HEAP_ZERO_MEMORY);
        if (pList) {
            for (int i = 0; i < count; ++i) {
                if (i > 0) {
                    StringCchCatA(pList, size, " ");
                }
                StringCchCatA(pList, size, ppEntries[i]->name);
                char suffix[8];
                StringCchPrintfA(suffix, COUNTOF(suffix), "?%d", ppEntries[i]->kind);
                StringCchCatA(pList, size, suffix);
            }
            SciCall_AutoCCancel();
            SciCall_AutoCSetChooseSingle(false);
            SciCall_AutoCSetSeparator(' ');
            SciCall_AutoCShow(0, pList);
            FreeMem(pList);
            shown = true;
        }
    }

    FreeMem(ppEntries);
    return shown;
}

// ----------------------------------------------------------------------------
// Document symbol / template variable scanning
// ----------------------------------------------------------------------------

typedef bool (*DEF_CALLBACK)(const char* pszName, const char* pszKind, void* pCtx);

static const char* _MatchKeyword(const char* p, const char* pszKeyword)
{
    const char* s = p;
    while (*pszKeyword) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pszKeyword)) {
            return NULL;
        }
        ++s;
        ++pszKeyword;
    }
    if (*s && !isspace((unsigned char)*s) && *s != '(' && *s != '=' && *s != ':') {
        return NULL;
    }
    return s;
}

static const char* _ReadIdent(const char* p, const char* pEnd, char* pszName, size_t cchName)
{
    if ((p >= pEnd) || !(isalpha((unsigned char)*p) || *p == '_')) {
        return NULL;
    }
    const char* start = p;
    while ((p < pEnd) && (isalnum((unsigned char)*p) || *p == '_')) {
        ++p;
    }
    size_t len = (size_t)(p - start);
    if (len >= cchName) {
        len = cchName - 1;
    }
    memcpy(pszName, start, len);
    pszName[len] = '\0';
    return p;
}

static bool _ScanLine(const char* pszLine, DEF_CALLBACK pfnCallback, void* pCtx)
{
    const char* p = pszLine;
    const char* const pEnd = pszLine + strlen(pszLine);
    while ((p < pEnd) && isspace((unsigned char)*p)) {
        ++p;
    }
    if (p >= pEnd) {
        return false;
    }
    if ((p[0] == '-' && p + 1 < pEnd && p[1] == '-') ||
        (p[0] == '/' && p + 1 < pEnd && p[1] == '/')) {
        return false;
    }

    static const char* const s_callKeywords[] = {
        "alloc", "globalalloc", "sharedalloc", "aobscanmodule", "aobscanregion",
        "aobscan", "define", "label", "registersymbol", "unregistersymbol", "dealloc",
    };

    for (int i = 0; i < (int)COUNTOF(s_callKeywords); ++i) {
        const char* r = _MatchKeyword(p, s_callKeywords[i]);
        if (!r) {
            continue;
        }
        while ((r < pEnd) && isspace((unsigned char)*r)) {
            ++r;
        }
        if ((r < pEnd) && (*r == '(')) {
            ++r;
            while ((r < pEnd) && isspace((unsigned char)*r)) {
                ++r;
            }
            char name[240];
            if (_ReadIdent(r, pEnd, name, COUNTOF(name))) {
                return pfnCallback(name, s_callKeywords[i], pCtx);
            }
        }
        return false;
    }

    const char* r = _MatchKeyword(p, "function");
    if (r) {
        while ((r < pEnd) && isspace((unsigned char)*r)) {
            ++r;
        }
        char name[240];
        if (_ReadIdent(r, pEnd, name, COUNTOF(name))) {
            return pfnCallback(name, "function", pCtx);
        }
        return false;
    }

    r = _MatchKeyword(p, "local");
    if (r) {
        while ((r < pEnd) && isspace((unsigned char)*r)) {
            ++r;
        }
        const char* f = _MatchKeyword(r, "function");
        if (f) {
            r = f;
            while ((r < pEnd) && isspace((unsigned char)*r)) {
                ++r;
            }
        }
        char name[240];
        if (_ReadIdent(r, pEnd, name, COUNTOF(name))) {
            return pfnCallback(name, "local", pCtx);
        }
        return false;
    }

    char name[240];
    const char* const afterName = _ReadIdent(p, pEnd, name, COUNTOF(name));
    if (afterName) {
        r = afterName;
        while ((r < pEnd) && isspace((unsigned char)*r)) {
            ++r;
        }
        if ((r < pEnd) && (*r == '=') && ((r + 1 >= pEnd) || (r[1] != '='))) {
            return pfnCallback(name, "assign", pCtx);
        }
        if ((r < pEnd) && (*r == ':') && ((r + 1 >= pEnd) || (r[1] != ':'))) {
            return pfnCallback(name, "label", pCtx);
        }
    }
    return false;
}

static bool _EmitLuaSymbol(const char* p, size_t len, const char* pszKind,
                           DEF_CALLBACK pfnCallback, void* pCtx)
{
    if ((len == 0) || (len >= 240)) {
        return false;
    }
    char name[240];
    memcpy(name, p, len);
    name[len] = '\0';
    return pfnCallback(name, pszKind, pCtx);
}

// Lua declarations the AA-oriented _ScanLine() does not see: multiple locals,
// function parameters, for-loop variables and "local function" definitions.
static bool _ScanExtraSymbols(const char* pszLine, DEF_CALLBACK pfnCallback, void* pCtx)
{
    const char* p = pszLine;
    const char* const end = p + strlen(pszLine);
    while (p < end) {
        if ((p[0] == '-') && (p + 1 < end) && (p[1] == '-')) {
            return false;
        }
        if ((p[0] == '/') && (p + 1 < end) && (p[1] == '/')) {
            return false;
        }
        if ((p[0] == '{') && (p + 1 < end) && (p[1] == '$')) {
            return false;
        }
        if ((p[0] == '"') || (p[0] == '\'')) {
            char const quote = *p++;
            while ((p < end) && (*p != quote)) {
                if ((*p == '\\') && (p + 1 < end)) {
                    ++p;
                }
                ++p;
            }
            if ((p < end) && (*p == quote)) {
                ++p;
            }
            continue;
        }
        if (!(isalpha((unsigned char)*p) || (*p == '_'))) {
            ++p;
            continue;
        }
        const char* const start = p;
        while ((p < end) && (isalnum((unsigned char)*p) || (*p == '_'))) {
            ++p;
        }
        size_t const len = (size_t)(p - start);
        char kw[24] = { '\0' };
        if (len >= COUNTOF(kw)) {
            continue;
        }
        memcpy(kw, start, len);
        kw[len] = '\0';
        bool const isFunction = (_stricmp(kw, "function") == 0);
        bool const isLocal = (_stricmp(kw, "local") == 0);
        bool const isFor = (_stricmp(kw, "for") == 0);
        if (!isFunction && !isLocal && !isFor) {
            continue;
        }
        const char* q = p;
        while ((q < end) && isspace((unsigned char)*q)) {
            ++q;
        }
        bool localFunction = false;
        if (isLocal && ((size_t)(end - q) >= 8) && (_strnicmp(q, "function", 8) == 0) &&
            ((q + 8 == end) || !(isalnum((unsigned char)q[8]) || (q[8] == '_')))) {
            localFunction = true;
            q += 8;
            while ((q < end) && isspace((unsigned char)*q)) {
                ++q;
            }
        }
        if (isFunction || localFunction) {
            if ((q < end) && (isalpha((unsigned char)*q) || (*q == '_'))) {
                const char* seg = q;
                while ((q < end) && (isalnum((unsigned char)*q) || (*q == '_') ||
                                    (*q == '.') || (*q == ':'))) {
                    if ((*q == '.') || (*q == ':')) {
                        seg = q + 1;
                    }
                    ++q;
                }
                if (_EmitLuaSymbol(seg, (size_t)(q - seg), "function", pfnCallback, pCtx)) {
                    return true;
                }
            }
            while ((q < end) && (*q != '(')) {
                ++q;
            }
            if (q < end) {
                ++q;
                while ((q < end) && (*q != ')')) {
                    if (isalpha((unsigned char)*q) || (*q == '_')) {
                        const char* const s = q;
                        while ((q < end) && (isalnum((unsigned char)*q) || (*q == '_'))) {
                            ++q;
                        }
                        if (_EmitLuaSymbol(s, (size_t)(q - s), "param", pfnCallback, pCtx)) {
                            return true;
                        }
                    }
                    else {
                        ++q;
                    }
                }
            }
            continue;
        }
        // "local a, b = ..." / "for i = ..." / "for k, v in ..."
        while (q < end) {
            while ((q < end) && isspace((unsigned char)*q)) {
                ++q;
            }
            if ((q >= end) || !(isalpha((unsigned char)*q) || (*q == '_'))) {
                break;
            }
            const char* const s = q;
            while ((q < end) && (isalnum((unsigned char)*q) || (*q == '_'))) {
                ++q;
            }
            size_t const n = (size_t)(q - s);
            if ((n == 2) && (_strnicmp(s, "in", 2) == 0)) {
                break;
            }
            if (_EmitLuaSymbol(s, n, "local", pfnCallback, pCtx)) {
                return true;
            }
            while ((q < end) && isspace((unsigned char)*q)) {
                ++q;
            }
            if ((q < end) && (*q == ',')) {
                ++q;
                continue;
            }
            break;
        }
    }
    return false;
}

static void _ScanDefinitions(DEF_CALLBACK pfnCallback, void* pCtx)
{
    DocPos const len = Sci_GetDocEndPosition();
    if (len <= 0) {
        return;
    }
    const char* const doc = SciCall_GetRangePointer(0, len);
    if (!doc) {
        return;
    }
    const char* p = doc;
    const char* const end = doc + len;
    while (p < end) {
        const char* lineEnd = memchr(p, '\n', (size_t)(end - p));
        if (!lineEnd) {
            lineEnd = end;
        }
        const char* scanEnd = lineEnd;
        if ((scanEnd > p) && (scanEnd[-1] == '\r')) {
            --scanEnd;
        }
        size_t const lineLen = (size_t)(scanEnd - p);
        char line[1024];
        size_t const copyLen = (lineLen < (COUNTOF(line) - 1)) ? lineLen : (COUNTOF(line) - 1);
        memcpy(line, p, copyLen);
        line[copyLen] = '\0';
        if (_ScanLine(line, pfnCallback, pCtx) ||
            _ScanExtraSymbols(line, pfnCallback, pCtx)) {
            return;
        }
        p = (lineEnd < end) ? (lineEnd + 1) : end;
    }
}

// ----------------------------------------------------------------------------
// Cached document-defined symbols
//
// The document is walked once after it changed; each keystroke afterwards is a
// binary search over the sorted symbols, so even a one character prefix stays
// cheap in a large script.  Scintilla's GetRangePointer() result is copied into
// owned buffers immediately, before any further editor call.
// ----------------------------------------------------------------------------

#define CE_DOC_SYM_MAX    4000
#define CE_DOC_SCAN_MAX   (1024 * 1024)
#define CE_WORDLIST_MAX   1200
#define CE_DOC_SYM_LEN    240
#define CE_DOC_REBUILD_MS 1000 // never rescan the file more than once a second

typedef struct _ce_doc_sym {
    char*  name;
    int    kind;      // CE_KIND_*
    bool   isAlloc;   // true for alloc / globalalloc / sharedalloc definitions
    bool   scoped;    // Lua local/parameter: only visible inside its block
    DocPos declPos;   // absolute offset of the declaration
    DocPos scopeEnd;  // exclusive end of the owning block; -1 = document-wide
} CE_DOC_SYM;

static CE_DOC_SYM* s_docSyms = NULL;
// Snapshot of the shared Lua scope model, kept so the completion list can ask
// about block depth at the caret without rebuilding it on every keystroke.
static CE_LUA_MODEL* s_luaModel = NULL;
static int    s_docSymCount = 0;
static bool   s_docSymDirty = true;
static ULONGLONG s_docSymLastBuild = 0;

// Per-line language mode, refreshed together with the symbol index.  Scintilla
// owns the document buffer, so the modes are copied into owned memory.
static unsigned char* s_ceLineModes = NULL;
static DocLn          s_ceLineModeCount = 0;

void CeCompletion_InvalidateDocSymbols(void)
{
    s_docSymDirty = true; // cheap: this runs for every SCN_MODIFIED
}

static void _RebuildLineModes(void)
{
    FreeMem(s_ceLineModes);
    s_ceLineModes = NULL;
    s_ceLineModeCount = 0;
    if (Sci_GetDocEndPosition() > CE_DOC_SCAN_MAX) {
        return;
    }
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return;
    }
    if (doc.lines > 0) {
        unsigned char* const modes = AllocMem((size_t)doc.lines, HEAP_ZERO_MEMORY);
        if (modes) {
            CeScript_LineModes(&doc, modes);
            s_ceLineModes = modes;
            s_ceLineModeCount = doc.lines;
        }
    }
    CeScript_FreeDoc(&doc);
}

// Language in effect at the caret: Lua, Auto Assembler or C.
static int _CurrentModeAtCaret(void)
{
    if (!s_ceLineModes || (s_ceLineModeCount <= 0)) {
        return CE_LANG_LUA;
    }
    DocLn const line = SciCall_LineFromPosition(SciCall_GetCurrentPos());
    if ((line < 0) || (line >= s_ceLineModeCount)) {
        return CE_LANG_LUA;
    }
    return (int)s_ceLineModes[line];
}

// Language at the caret for callers outside this file (the LSP completion path
// ranks the server list with it).  The symbol index rebuild keeps the mode map
// fresh; it is rate limited the same way the local completion list is.
static bool _EnsureDocSymbols(void);

int CeCompletion_CurrentMode(void)
{
    (void)_EnsureDocSymbols();
    return _CurrentModeAtCaret();
}

bool CeCompletion_AllowAutoIndent(void)
{
    if (!CeCompletion_IsActive()) {
        return true;
    }
    // The per-line mode map is normally refreshed by the rate-limited symbol
    // index, which is far too coarse for a decision taken right after Enter:
    // typing a {$lua}/{$asm} directive must switch the next line immediately.
    if (s_docSymDirty || !s_ceLineModes || (s_ceLineModeCount != SciCall_GetLineCount())) {
        _RebuildLineModes();
    }
    return (_CurrentModeAtCaret() == (int)CE_LANG_AA);
}

static int __cdecl _CompareDocSym(const void* pA, const void* pB)
{
    const CE_DOC_SYM* const a = (const CE_DOC_SYM*)pA;
    const CE_DOC_SYM* const b = (const CE_DOC_SYM*)pB;
    return _stricmp(a->name, b->name);
}

static void _AddDocSym(const char* pszName, int kind, bool isAlloc)
{
    size_t const len = strlen(pszName);
    if ((len == 0) || (len >= CE_DOC_SYM_LEN) || (s_docSymCount >= CE_DOC_SYM_MAX)) {
        return;
    }
    char* const pCopy = AllocMem(len + 1, HEAP_ZERO_MEMORY);
    if (!pCopy) {
        return;
    }
    memcpy(pCopy, pszName, len);
    pCopy[len] = '\0';
    s_docSyms[s_docSymCount].name = pCopy;
    s_docSyms[s_docSymCount].kind = kind;
    s_docSyms[s_docSymCount].isAlloc = isAlloc;
    s_docSyms[s_docSymCount].scoped = false;
    s_docSyms[s_docSymCount].declPos = -1;
    s_docSyms[s_docSymCount].scopeEnd = -1;
    ++s_docSymCount;
}

static bool _CollectDocSym(const char* pszName, const char* pszKind, void* pCtx)
{
    UNREFERENCED_PARAMETER(pCtx);
    if (!_IsIdentifier(pszName)) {
        return false;
    }
    int kind = CE_KIND_VARIABLE;
    bool isAlloc = false;
    if (_stricmp(pszKind, "function") == 0) {
        kind = CE_KIND_FUNCTION;
    }
    else if ((_stricmp(pszKind, "alloc") == 0) || (_stricmp(pszKind, "globalalloc") == 0) ||
             (_stricmp(pszKind, "sharedalloc") == 0) || (_stricmp(pszKind, "label") == 0) ||
             (_stricmp(pszKind, "define") == 0) || (_stricmp(pszKind, "registersymbol") == 0) ||
             (_stricmp(pszKind, "aobscan") == 0) || (_stricmp(pszKind, "aobscanmodule") == 0) ||
             (_stricmp(pszKind, "aobscanregion") == 0)) {
        kind = CE_KIND_AA;
        // 只有 alloc 家族的内存变量才需要自动包裹 []；label/define/registersymbol/aobscan 不包裹
        if ((_stricmp(pszKind, "alloc") == 0) || (_stricmp(pszKind, "globalalloc") == 0) ||
            (_stricmp(pszKind, "sharedalloc") == 0)) {
            isAlloc = true;
        }
    }
    _AddDocSym(pszName, kind, isAlloc);
    return false; // keep scanning every line
}

// Feed the shared Lua scope model into the completion index.  _ScanDefinitions()
// cannot see function parameters, generic-for variables, dotted function names
// or table constructor keys; the model also marks which names are block-local
// so the word list can be filtered at the caret instead of offering every
// local declared anywhere in the file.
static void _AddLuaScopeSym(const CE_LUA_SYMBOL* sym)
{
    if (!sym || (sym->name[0] == '\0') || (sym->kind == CE_LUA_SYM_FIELD) ||
        !_IsIdentifier(sym->name)) {
        return;
    }
    int const kind = ((sym->kind == CE_LUA_SYM_FUNCTION) || (sym->kind == CE_LUA_SYM_LOCALFN)) ?
        CE_KIND_FUNCTION : CE_KIND_VARIABLE;
    int const before = s_docSymCount;
    _AddDocSym(sym->name, kind, false);
    if (s_docSymCount <= before) {
        return;
    }
    CE_DOC_SYM* const added = &s_docSyms[s_docSymCount - 1];
    bool const scoped = (sym->kind == CE_LUA_SYM_LOCAL) ||
        (sym->kind == CE_LUA_SYM_PARAM) || (sym->kind == CE_LUA_SYM_LOCALFN);
    added->scoped = scoped;
    added->declPos = sym->pos;
    added->scopeEnd = scoped ? sym->scopeEnd : -1;
}

static void _CollectLuaModelSymbols(void)
{
    CE_LUA_MODEL* const model = AllocMem(sizeof(CE_LUA_MODEL), HEAP_ZERO_MEMORY);
    if (!model) {
        return;
    }
    CeLuaScope_Init(model);
    CE_DOC doc = { 0 };
    if (CeScript_InitDoc(&doc)) {
        for (DocLn line = 0; line < doc.lines; ++line) {
            if (s_ceLineModes && (line < s_ceLineModeCount) &&
                (s_ceLineModes[line] != (unsigned char)CE_LANG_LUA)) {
                continue;
            }
            DocPos n = 0;
            const char* const p = CeScript_LinePtr(&doc, line, &n);
            CeLuaScope_Line(model, p, p + n, doc.starts[line], line + 1);
        }
        CeLuaScope_Finish(model, doc.len);
        CeScript_FreeDoc(&doc);
    }
    if (!model->truncated) {
        for (int i = 0; i < model->count; ++i) {
            _AddLuaScopeSym(&model->syms[i]);
        }
    }
    // Keep the model: CeLuaScope_BlockDepthAt() is queried at completion time,
    // when the Scintilla range pointer may already have moved.
    FreeMem(s_luaModel);
    s_luaModel = model;
}

static void _RebuildDocSymbols(void)
{
    for (int i = 0; i < s_docSymCount; ++i) {
        FreeMem(s_docSyms[i].name);
    }
    FreeMem(s_docSyms);
    s_docSyms = NULL;
    s_docSymCount = 0;
    s_docSymDirty = false;
    ++s_docSymGen;

    // Keep the language-mode map in sync with the symbol index.  It must run
    // before _ScanDefinitions(), which re-acquires Scintilla's range pointer.
    _RebuildLineModes();

    DocPos const len = Sci_GetDocEndPosition();
    if ((len <= 0) || (len > CE_DOC_SCAN_MAX)) {
        return; // huge documents keep the catalogue + keyword lists only
    }
    s_docSyms = AllocMem(sizeof(CE_DOC_SYM) * CE_DOC_SYM_MAX, HEAP_ZERO_MEMORY);
    if (!s_docSyms) {
        return;
    }
    _ScanDefinitions(_CollectDocSym, NULL);
    _CollectLuaModelSymbols();
    if (s_docSymCount > 1) {
        qsort(s_docSyms, (size_t)s_docSymCount, sizeof(CE_DOC_SYM), _CompareDocSym);
        int write = 0;
        for (int i = 0; i < s_docSymCount; ++i) {
            if ((write > 0) && (_stricmp(s_docSyms[i].name, s_docSyms[write - 1].name) == 0)) {
                // A function definition is more informative than a plain word.
                if (s_docSyms[i].kind == CE_KIND_FUNCTION) {
                    s_docSyms[write - 1].kind = CE_KIND_FUNCTION;
                }
                // alloc 家族优先：同名同时出现 alloc 与 label 时按 alloc 处理（包裹 []）
                if (s_docSyms[i].isAlloc) {
                    s_docSyms[write - 1].isAlloc = true;
                }
                // The plain line scan also sees Lua locals but has no scope
                // information.  When the model marked the name block-local,
                // that (more precise) view wins; keep the widest block among
                // several locals with the same name.
                if (s_docSyms[i].scoped) {
                    if (!s_docSyms[write - 1].scoped) {
                        s_docSyms[write - 1].scoped = true;
                        s_docSyms[write - 1].declPos = s_docSyms[i].declPos;
                        s_docSyms[write - 1].scopeEnd = s_docSyms[i].scopeEnd;
                    }
                    else if (s_docSyms[i].scopeEnd > s_docSyms[write - 1].scopeEnd) {
                        s_docSyms[write - 1].declPos = s_docSyms[i].declPos;
                        s_docSyms[write - 1].scopeEnd = s_docSyms[i].scopeEnd;
                    }
                }
                FreeMem(s_docSyms[i].name);
                continue;
            }
            s_docSyms[write++] = s_docSyms[i];
        }
        s_docSymCount = write;
    }
}

// ----------------------------------------------------------------------------

static void _AppendWordItem(char* pList, size_t cchList, const char* pszName, int kind)
{
    StringCchCatA(pList, cchList, " ");
    StringCchCatA(pList, cchList, pszName);
    char suffix[8];
    StringCchPrintfA(suffix, COUNTOF(suffix), "?%d", kind);
    StringCchCatA(pList, cchList, suffix);
}

// ----------------------------------------------------------------------------
// Words that exist only in the generated CE_KEYWORDS_* lexer sets and not in
// g_ceCompletions (e.g. "table", "align", "createthread").  They are collected
// once at init so the ranked list stays complete without merging the generic
// keyword scan on every keystroke.
// ----------------------------------------------------------------------------

typedef struct _ce_extra_word {
    char name[64];
    int  kind;
} CE_EXTRA_WORD;

#define CE_EXTRA_WORD_MAX 512

static CE_EXTRA_WORD* s_extraWords = NULL;
static int            s_extraCount = 0;

static int __cdecl _CompareNamePtr(const void* pA, const void* pB)
{
    return _stricmp(*(const char* const*)pA, *(const char* const*)pB);
}

static bool _CatalogueContainsCI(const char* const* sorted, int count, const char* name)
{
    int lo = 0;
    int hi = count;
    while (lo < hi) {
        int const mid = lo + (hi - lo) / 2;
        if (_stricmp(sorted[mid], name) < 0) {
            lo = mid + 1;
        }
        else {
            hi = mid;
        }
    }
    return (lo < count) && (_stricmp(sorted[lo], name) == 0);
}

static void _AddExtraWords(const char* list, int kind, const char* const* sorted, int sortedCount)
{
    const char* p = list;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        const char* const start = p;
        while (*p && (*p != ' ')) {
            ++p;
        }
        size_t const len = (size_t)(p - start);
        if ((len == 0) || (len >= COUNTOF(s_extraWords[0].name))) {
            continue;
        }
        char name[64];
        memcpy(name, start, len);
        name[len] = '\0';
        if (!_IsIdentifierLike(name) || _CatalogueContainsCI(sorted, sortedCount, name)) {
            continue;
        }
        bool duplicate = false;
        for (int i = 0; i < s_extraCount; ++i) {
            if (_stricmp(s_extraWords[i].name, name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate || (s_extraCount >= CE_EXTRA_WORD_MAX)) {
            continue;
        }
        memcpy(s_extraWords[s_extraCount].name, name, len + 1);
        s_extraWords[s_extraCount].kind = kind;
        ++s_extraCount;
    }
}

static void _BuildExtraWords(void)
{
    FreeMem(s_extraWords);
    s_extraWords = AllocMem(sizeof(CE_EXTRA_WORD) * CE_EXTRA_WORD_MAX, HEAP_ZERO_MEMORY);
    s_extraCount = 0;
    if (!s_extraWords) {
        return;
    }
    const char** const sorted = AllocMem(sizeof(char*) * (size_t)g_ceCompletionCount, HEAP_ZERO_MEMORY);
    if (!sorted) {
        return;
    }
    for (int i = 0; i < g_ceCompletionCount; ++i) {
        sorted[i] = g_ceCompletions[i].name;
    }
    qsort(sorted, (size_t)g_ceCompletionCount, sizeof(char*), _CompareNamePtr);

    static const struct {
        const char* list;
        int kind;
    } sets[] = {
        { CE_KEYWORDS_LUA,   CE_KIND_KEYWORD },
        { CE_KEYWORDS_API,   CE_KIND_FUNCTION },
        { CE_KEYWORDS_AA,    CE_KIND_AA },
        { CE_KEYWORDS_ASM,   CE_KIND_ASM },
        { CE_KEYWORDS_TYPES, CE_KIND_TYPE },
    };
    for (int i = 0; i < (int)COUNTOF(sets); ++i) {
        _AddExtraWords(sets[i].list, sets[i].kind, sorted, g_ceCompletionCount);
    }
    FreeMem(sorted);
}

// ----------------------------------------------------------------------------
// Completion ranking.
//
// The generated catalogue plus the extra keyword words covers all five keyword
// sets, so the CE list is handed to Scintilla verbatim (SC_ORDER_CUSTOM).  The
// score combines prefix quality, the language in effect at the caret, how
// common an item is in real CE scripts and user-defined symbols.
// ----------------------------------------------------------------------------

typedef enum _ce_group {
    CE_GROUP_LUA = 0,
    CE_GROUP_AA,
    CE_GROUP_C,
    CE_GROUP_NEUTRAL
} CE_GROUP;

typedef struct _ce_cand {
    const char* name;
    int kind;
    int score;
} CE_CAND;

// High-use CE script items, grounded in the generated catalogue: AA commands,
// x86 mnemonics/registers and the CE Lua API functions used by most scripts.
static const char* const s_commonCeNames[] = {
    "alloc", "dealloc", "label", "registersymbol", "unregistersymbol",
    "define", "aobscanmodule", "aobscan", "globalalloc", "readmem",
    "assert", "createthread", "fullaccess", "jmp", "call", "ret", "nop",
    "mov", "cmp", "add", "sub", "lea", "push", "pop", "je", "jne", "jz", "jnz",
    "eax", "ebx", "ecx", "edx", "rax", "rbx", "rcx", "rdx", "rsp", "rbp",
    "print", "tostring", "tonumber", "pairs", "ipairs", "type",
    "readInteger", "writeInteger", "readBytes", "writeBytes", "readFloat",
    "writeFloat", "readQword", "writeQword", "getAddress", "getAddressSafe",
    "autoAssemble", "AOBScan", "AOBScanModule", "showMessage", "sleep",
    "string.format", "table.insert",
};

// C-only declarations inside {$ccode}; shared Lua/C control words are left in
// the neutral/Lua bucket so a Lua block never loses its keywords.
static const char* const s_cOnlyKeywords[] = {
    "auto", "bool", "break", "case", "char", "const", "continue", "default",
    "double", "enum", "extern", "float", "goto", "int", "int8_t", "int16_t",
    "int32_t", "int64_t", "long", "register", "short", "signed", "sizeof",
    "size_t", "static", "struct", "switch", "typedef", "uint8_t", "uint16_t",
    "uint32_t", "uint64_t", "union", "unsigned", "void", "volatile",
    "BYTE", "DWORD", "WORD", "QWORD", "HANDLE", "LPVOID",
};

static bool _NameInList(const char* const list[], int count, const char* name)
{
    for (int i = 0; i < count; ++i) {
        if (_stricmp(name, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static CE_GROUP _EntryGroup(const CE_COMPLETION_ENTRY* pEntry)
{
    const char* const name = pEntry->name;
    if (_NameInList(s_cOnlyKeywords, (int)COUNTOF(s_cOnlyKeywords), name)) {
        return CE_GROUP_C;
    }
    if (_InKeywordList(CE_KEYWORDS_ASM, name) || _InKeywordList(CE_KEYWORDS_AA, name)) {
        return CE_GROUP_AA;
    }
    if (_InKeywordList(CE_KEYWORDS_API, name) || _InKeywordList(CE_KEYWORDS_TYPES, name) ||
        _InKeywordList(CE_KEYWORDS_LUA, name)) {
        return CE_GROUP_LUA;
    }
    if ((pEntry->kind == CE_KIND_SECTION) || (pEntry->kind == CE_KIND_ASM) ||
        (pEntry->kind == CE_KIND_AA)) {
        return CE_GROUP_AA;
    }
    if ((pEntry->kind == CE_KIND_FUNCTION) || (pEntry->kind == CE_KIND_KEYWORD)) {
        return CE_GROUP_LUA;
    }
    return CE_GROUP_NEUTRAL;
}

// Weight of one language group in the language in effect at the caret.  Shared
// by the local catalogue pass and the LSP list (which only gets a server-order
// list, not a ranking).
static int _GroupBonus(CE_GROUP group, int mode)
{
    CE_GROUP const want = (mode == (int)CE_LANG_AA) ? CE_GROUP_AA :
                          (mode == (int)CE_LANG_C) ? CE_GROUP_C : CE_GROUP_LUA;
    if (group == want) {
        return 520;
    }
    if (group == CE_GROUP_NEUTRAL) {
        return 40;
    }
    if (((group == CE_GROUP_AA) && (want == CE_GROUP_LUA)) ||
        ((group == CE_GROUP_LUA) && (want == CE_GROUP_AA))) {
        return -260;
    }
    if ((mode == (int)CE_LANG_AA) && (group == CE_GROUP_C)) {
        return 90;
    }
    return 0;
}

static int _ScoreCatalogue(const CE_COMPLETION_ENTRY* pEntry, const char* pszRoot,
                           size_t rootLen, int mode)
{
    int score = 350 + _MatchBonus(pEntry->name, pszRoot, rootLen);
    score += _GroupBonus(_EntryGroup(pEntry), mode);
    if (_NameInList(s_commonCeNames, (int)COUNTOF(s_commonCeNames), pEntry->name)) {
        score += 240;
    }
    if (pEntry->isCall) {
        score += 70;
    }
    score -= (int)strlen(pEntry->name);
    return score;
}

// Context weight for a list item whose order the caller cannot choose (the
// language server returns the generated catalogue in table order).  Same
// grouping as _ScoreCatalogue() without the prefix/length terms: the caller has
// already filtered by the typed root.
int CeCompletion_ContextRank(const char* pszName, int kind, int mode)
{
    if (!pszName || !*pszName) {
        return 0;
    }
    CE_COMPLETION_ENTRY probe = { pszName, NULL, NULL, 0, kind };
    const CE_COMPLETION_ENTRY* const pEntry = CeCompletion_Find(pszName);
    int score = _GroupBonus(_EntryGroup(pEntry ? pEntry : &probe), mode);
    if (_NameInList(s_commonCeNames, (int)COUNTOF(s_commonCeNames), pszName)) {
        score += 240;
    }
    if (pEntry && pEntry->isCall) {
        score += 70;
    }
    return score;
}

// np3watch directive comment vocabulary, mirroring the parser in CeBridge.c
// and the lexer sets in ce_edit/ce_edit/lexer.py.
static const char* const s_watchDirectives[] = {
    "np3watch", "np3unwatch", "np3nowatch"
};
static const char* const s_watchWords[] = {
    "byte", "word", "dword", "qword", "pointer", "float", "double",
    "string", "wstring", "array", "reg", "u8", "u16", "u32", "u64",
    "i8", "i16", "i32", "i64", "ptr", "f32", "f64", "str", "wstr",
    "arr", "auto", "bin", "dec", "hex", "binary", "decimal",
    "hexadecimal", "on", "off", "if", "begin", "end",
};

// CPU registers accepted as watch names / condition operands (CeBridge.c).
static const char* const s_watchRegisters[] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "eflags", "flags",
    "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip",
    "ax", "bx", "cx", "dx", "si", "di", "bp", "sp",
    "al", "bl", "cl", "dl", "ah", "bh", "ch", "dh",
    "sil", "dil", "bpl", "spl",
};

static bool _NameIsWatchRegister(const char* pszName)
{
    if (_NameInList(s_watchRegisters, (int)COUNTOF(s_watchRegisters), pszName)) {
        return true;
    }
    // r8d-r15d / r8b-r15b
    size_t const len = strlen(pszName);
    if ((len >= 3) && (pszName[0] == 'r') && ((pszName[len - 1] == 'd') || (pszName[len - 1] == 'b'))) {
        int idx = 0;
        for (size_t i = 1; i + 1 < len; ++i) {
            if ((pszName[i] < '0') || (pszName[i] > '9')) {
                return false;
            }
            idx = (idx * 10) + (int)(pszName[i] - '0');
        }
        return (idx >= 8) && (idx <= 15);
    }
    return false;
}

// Extra weight for the np3watch vocabulary while the caret is on an np3watch
// comment line.  The caller decides when the context applies; 0 means the item
// is not part of the feature.
int CeCompletion_WatchRank(const char* pszName)
{
    if (!pszName || !*pszName) {
        return 0;
    }
    if (_NameInList(s_watchDirectives, (int)COUNTOF(s_watchDirectives), pszName)) {
        return 1200;
    }
    if (_NameInList(s_watchWords, (int)COUNTOF(s_watchWords), pszName)) {
        return 900;
    }
    if (_NameIsWatchRegister(pszName)) {
        return 850;
    }
    return 0;
}

static int _ScoreDocSym(const CE_DOC_SYM* pSym, const char* pszRoot, size_t rootLen, int mode)
{
    int score = 900 + _MatchBonus(pSym->name, pszRoot, rootLen); // document symbols first
    if (pSym->kind == CE_KIND_FUNCTION) {
        score += 120;
    }
    else if ((pSym->kind == CE_KIND_AA) && (mode == (int)CE_LANG_AA)) {
        score += 90;
    }
    score -= (int)strlen(pSym->name);
    return score;
}

static int _ScoreExtraWord(const CE_EXTRA_WORD* pExtra, const char* pszRoot, size_t rootLen, int mode)
{
    int score = 250 + _MatchBonus(pExtra->name, pszRoot, rootLen);
    bool const aa = (pExtra->kind == CE_KIND_AA) || (pExtra->kind == CE_KIND_ASM);
    if ((aa && (mode == (int)CE_LANG_AA)) || (!aa && (mode == (int)CE_LANG_LUA))) {
        score += 180;
    }
    score -= (int)strlen(pExtra->name);
    return score;
}

static int __cdecl _CompareCand(const void* pA, const void* pB)
{
    const CE_CAND* const a = (const CE_CAND*)pA;
    const CE_CAND* const b = (const CE_CAND*)pB;
    if (a->score != b->score) {
        return (a->score > b->score) ? -1 : 1;
    }
    return _stricmp(a->name, b->name);
}

static bool _AlreadyAdded(const CE_CAND* cands, int count, const char* name)
{
    for (int i = 0; i < count; ++i) {
        if (_stricmp(cands[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

// Duplicate suppression while a list is assembled.  The linear scan above is
// O(candidates^2) and ran for every built-in/dictionary word on every
// keystroke.  A generation-stamped open-addressing set makes it O(1) and
// avoids clearing the table (a generation bump starts a fresh set).
typedef struct _ce_seen {
    const char* name;
    unsigned    gen;
} CE_SEEN;

static CE_SEEN* s_seen = NULL;
static size_t   s_seenCap = 0;
static unsigned s_seenGen = 0;

static unsigned _HashNameCI(const char* name)
{
    unsigned h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) {
        h = (h ^ (unsigned char)tolower(*p)) * 16777619u;
    }
    return h;
}

static bool _EnsureSeen(size_t needed)
{
    size_t cap = 1024;
    while (cap < (needed * 2)) {
        cap *= 2;
    }
    if (s_seen && (s_seenCap >= cap)) {
        return true;
    }
    CE_SEEN* const grown = AllocMem(sizeof(CE_SEEN) * cap, HEAP_ZERO_MEMORY);
    if (!grown) {
        return false;
    }
    FreeMem(s_seen);
    s_seen = grown;
    s_seenCap = cap;
    s_seenGen = 0;
    return true;
}

static bool _ClaimName(const CE_CAND* cands, int count, const char* name, bool useSeen)
{
    if (!useSeen || !s_seen || (s_seenCap == 0)) {
        return !_AlreadyAdded(cands, count, name);
    }
    size_t const mask = s_seenCap - 1;
    size_t i = (size_t)_HashNameCI(name) & mask;
    while (s_seen[i].gen == s_seenGen) {
        if (_stricmp(s_seen[i].name, name) == 0) {
            return false;
        }
        i = (i + 1) & mask;
    }
    s_seen[i].gen = s_seenGen;
    s_seen[i].name = name;
    return true;
}

// Typing invalidates the index on every keystroke; rebuilding it every time
// would walk the whole file while the user types.  Rebuild at most once per
// CE_DOC_REBUILD_MS and keep the slightly stale index meanwhile.  Shared by the
// completion list and _IsAllocDocumentSymbol() so a keystroke never scans the
// document twice.
static bool _EnsureDocSymbols(void)
{
    if (s_docSymDirty) {
        ULONGLONG const now = GetTickCount64();
        if ((s_docSymLastBuild == 0) || ((now - s_docSymLastBuild) >= CE_DOC_REBUILD_MS)) {
            _RebuildDocSymbols();
            s_docSymLastBuild = now;
        }
    }
    return (s_docSymCount > 0);
}

// Scratch buffer reused across keystrokes.  It is only alive during one call
// (qsort, emit, return), so a persistent allocation removes the AllocMem /
// FreeMem pair that used to run on every character.
static CE_CAND* s_candScratch = NULL;
static size_t   s_candScratchCap = 0;

// Last emitted list, keyed by root + mode + the generations that can change
// the candidate set.  Serving a copy avoids rescanning 2146 entries while the
// user extends the same word one character at a time.
static char*    s_lastList = NULL;
static char     s_lastRoot[240];
static int      s_lastMode = -1;
static bool     s_lastDynamic = false;
static bool     s_lastValid = false;
static unsigned s_lastDocSymGen = 0;
static unsigned s_lastDictGen = 0;
static DocPos   s_lastCaretPos = -1; // Lua locals are filtered at the caret

static void _InvalidateWordListCache(void)
{
    s_lastValid = false;
    s_lastRoot[0] = '\0';
}

static bool _EnsureCandScratch(size_t needed)
{
    if (s_candScratch && (s_candScratchCap >= needed)) {
        return true;
    }
    size_t const cap = (needed > (s_candScratchCap * 2)) ? needed : (s_candScratchCap * 2);
    CE_CAND* const grown = AllocMem(sizeof(CE_CAND) * cap, HEAP_ZERO_MEMORY);
    if (!grown) {
        return false;
    }
    FreeMem(s_candScratch);
    s_candScratch = grown;
    s_candScratchCap = cap;
    return true;
}

char* CeCompletion_CreateWordList(const char* pszRoot)
{
    (void)_EnsureDocSymbols();
    if (!s_dictionaryLoaded) {
        _LoadDictionary();
    }

    size_t const rootLen = strlen(pszRoot);
    bool const dynamic = CeSettings_DynamicCompletions();
    int const mode = _CurrentModeAtCaret();
    DocPos const caretPos = SciCall_GetCurrentPos();

    if (s_lastValid && (s_lastMode == mode) && (s_lastDynamic == dynamic) &&
        (s_lastDocSymGen == s_docSymGen) && (s_lastDictGen == s_dictGen) &&
        (s_lastCaretPos == caretPos) &&
        (strcmp(s_lastRoot, pszRoot) == 0) && s_lastList) {
        size_t const bytes = strlen(s_lastList) + 1;
        char* const copy = AllocMem(bytes, HEAP_ZERO_MEMORY);
        if (copy) {
            memcpy(copy, s_lastList, bytes);
        }
        return copy;
    }

    size_t const maxCands = (size_t)g_ceCompletionCount + (size_t)s_docSymCount +
                            (size_t)s_extraCount + (size_t)s_dictionaryCount +
                            COUNTOF(s_builtinWords) + COUNTOF(s_codeBlocks) + 1;

    // Surrounding code: only offer statement skeletons where a statement can
    // actually start, and never after a member access or inside a table body.
    DocPos const rootStart = (caretPos >= (DocPos)rootLen) ?
                             (caretPos - (DocPos)rootLen) : 0;
    bool const blocksAllowed = _CodeBlocksAllowed(rootStart, rootLen, mode);
    // Scope: when Lua blocks are open, "end"/"else"/"until" are far more
    // likely to be what the user wants; with none open they are noise.
    int const openBlocks = (mode == (int)CE_LANG_LUA) ?
                           CeLuaScope_BlockDepthAt(s_luaModel, rootStart) : 0;
    if (!_EnsureCandScratch(maxCands)) {
        return NULL;
    }
    bool const useSeen = _EnsureSeen(maxCands);
    if (useSeen) {
        ++s_seenGen;
        if (s_seenGen == 0) {
            // Generation wrapped: clear the stale stamps and start over.
            ZeroMemory(s_seen, sizeof(CE_SEEN) * s_seenCap);
            s_seenGen = 1;
        }
    }
    CE_CAND* const cands = s_candScratch;
    int count = 0;
    // Module-qualified catalogue entries ("math.floor") are member completions
    // for their dot only; a plain word root must never match them, otherwise
    // typing "math" floods the list with math.* entries.  A qualified root
    // ("math.f" in the local fallback, which has no receiver analysis) keeps
    // them available.
    const bool qualifiedRoot = strchr(pszRoot, '.') != NULL;
    for (int i = 0; i < g_ceCompletionCount; ++i) {
        const CE_COMPLETION_ENTRY* const pEntry = &g_ceCompletions[i];
        if (_CandidateMatches(pEntry->name, pszRoot, rootLen) &&
            _IsCompletableName(pEntry->name)) {
            if (!qualifiedRoot && strchr(pEntry->name, '.')) {
                continue;
            }
            if (!_ClaimName(cands, count, pEntry->name, useSeen)) {
                continue;
            }
            cands[count].name = pEntry->name;
            cands[count].kind = pEntry->kind;
            int score = _ScoreCatalogue(pEntry, pszRoot, rootLen, mode);
            // Every keyword stays available; only its rank changes with the
            // surrounding Lua block structure.
            if ((mode == (int)CE_LANG_LUA) && _IsLuaBlockCloser(pEntry->name)) {
                score += (openBlocks > 0) ? 260 : -260;
            }
            cands[count].score = score;
            ++count;
        }
    }
    // Code blocks are additional entries: the plain keywords above are kept.
    if (blocksAllowed) {
        for (int i = 0; i < (int)COUNTOF(s_codeBlocks); ++i) {
            const CE_CODE_BLOCK* const block = &s_codeBlocks[i];
            if (!(block->modeMask & (1 << mode))) {
                continue; // hard filter: no Lua skeleton inside AA/C code
            }
            if (!_CandidateMatches(block->entry.name, pszRoot, rootLen) ||
                !_ClaimName(cands, count, block->entry.name, useSeen)) {
                continue;
            }
            cands[count].name = block->entry.name;
            cands[count].kind = block->entry.kind;
            cands[count].score = _ScoreCatalogue(&block->entry, pszRoot, rootLen, mode) - 40;
            ++count;
        }
    }
    if (dynamic) {
        // Substring matching means the symbol table can no longer be binary
        // searched by prefix; scan the (small) cached symbol list instead.
        for (int i = 0; i < s_docSymCount; ++i) {
            const CE_DOC_SYM* const pSym = &s_docSyms[i];
            // A Lua local/parameter is only offered while the caret is inside
            // the block that declared it, and after the declaration.
            if (pSym->scoped &&
                !((pSym->declPos < caretPos) && (caretPos < pSym->scopeEnd))) {
                continue;
            }
            if (!_CandidateMatches(pSym->name, pszRoot, rootLen) ||
                !_IsIdentifierLike(pSym->name) || CeCompletion_Find(pSym->name) ||
                !_ClaimName(cands, count, pSym->name, useSeen)) {
                continue;
            }
            cands[count].name = pSym->name;
            cands[count].kind = pSym->kind;
            cands[count].score = _ScoreDocSym(pSym, pszRoot, rootLen, mode);
            ++count;
        }
    }
    for (int i = 0; i < s_extraCount; ++i) {
        const CE_EXTRA_WORD* const pExtra = &s_extraWords[i];
        if (!_CandidateMatches(pExtra->name, pszRoot, rootLen) ||
            !_ClaimName(cands, count, pExtra->name, useSeen)) {
            continue;
        }
        cands[count].name = pExtra->name;
        cands[count].kind = pExtra->kind;
        cands[count].score = _ScoreExtraWord(pExtra, pszRoot, rootLen, mode);
        ++count;
    }
    // Bundled variable names and the optional external dictionary.
    for (int pass = 0; pass < 2; ++pass) {
        const CE_COMPLETION_ENTRY* const table = pass ? s_dictionary : s_builtinWords;
        int const n = pass ? s_dictionaryCount : (int)COUNTOF(s_builtinWords);
        for (int i = 0; i < n; ++i) {
            const CE_COMPLETION_ENTRY* const pEntry = &table[i];
            if (!pEntry->name || !_CandidateMatches(pEntry->name, pszRoot, rootLen) ||
                !_ClaimName(cands, count, pEntry->name, useSeen)) {
                continue;
            }
            cands[count].name = pEntry->name;
            cands[count].kind = pEntry->kind;
            cands[count].score = 700 + _MatchBonus(pEntry->name, pszRoot, rootLen);
            ++count;
        }
    }
    if (count == 0) {
        _InvalidateWordListCache();
        return NULL;
    }
    if (count > 1) {
        qsort(cands, (size_t)count, sizeof(CE_CAND), _CompareCand);
    }
    int const emit = (count < CE_WORDLIST_MAX) ? count : CE_WORDLIST_MAX;
    size_t needed = 2;
    for (int i = 0; i < emit; ++i) {
        needed += (strlen(cands[i].name) + 4); // separator + name + "?N"
    }
    char* const pList = AllocMem(needed, HEAP_ZERO_MEMORY);
    if (!pList) {
        _InvalidateWordListCache();
        return NULL;
    }
    for (int i = 0; i < emit; ++i) {
        _AppendWordItem(pList, needed, cands[i].name, cands[i].kind);
    }

    // Keep a private copy for the next keystroke; the caller owns pList.
    FreeMem(s_lastList);
    s_lastList = AllocMem(needed, HEAP_ZERO_MEMORY);
    if (s_lastList) {
        memcpy(s_lastList, pList, needed);
    }
    StringCchCopyA(s_lastRoot, COUNTOF(s_lastRoot), pszRoot);
    s_lastMode = mode;
    s_lastDynamic = dynamic;
    s_lastDocSymGen = s_docSymGen;
    s_lastDictGen = s_dictGen;
    s_lastCaretPos = caretPos;
    s_lastValid = (s_lastList != NULL);

    return pList;
}

// ----------------------------------------------------------------------------

int CeCompletion_CollectExtraWords(const char* pszRoot, CE_WORD_HINT* out, int max)
{
    if (!out || (max <= 0)) {
        return 0;
    }
    if (!s_dictionaryLoaded) {
        _LoadDictionary();
    }
    char const* const root = pszRoot ? pszRoot : "";
    size_t const rootLen = strlen(root);
    int count = 0;
    for (int pass = 0; (pass < 2) && (count < max); ++pass) {
        const CE_COMPLETION_ENTRY* const table = pass ? s_dictionary : s_builtinWords;
        int const n = pass ? s_dictionaryCount : (int)COUNTOF(s_builtinWords);
        for (int i = 0; (i < n) && (count < max); ++i) {
            const CE_COMPLETION_ENTRY* const pEntry = &table[i];
            if (!pEntry->name || !_IsIdentifierLike(pEntry->name) ||
                !_CandidateMatches(pEntry->name, root, rootLen)) {
                continue;
            }
            bool duplicate = false;
            for (int j = 0; j < count; ++j) {
                if (_stricmp(out[j].name, pEntry->name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            out[count].name = pEntry->name;
            out[count].detail = pEntry->signature ? pEntry->signature : "";
            out[count].kind = pEntry->kind;
            ++count;
        }
    }
    // Code-block skeletons must also be present when the language server owns
    // the AutoC list.  Keep the same language/context rules as the local list.
    int const mode = _CurrentModeAtCaret();
    DocPos const caret = SciCall_GetCurrentPos();
    DocPos const rootStart = (caret >= (DocPos)rootLen) ? (caret - (DocPos)rootLen) : 0;
    if (_CodeBlocksAllowed(rootStart, rootLen, mode)) {
        for (int i = 0; (i < (int)COUNTOF(s_codeBlocks)) && (count < max); ++i) {
            const CE_CODE_BLOCK* const block = &s_codeBlocks[i];
            if (!(block->modeMask & (1 << mode)) ||
                !_CandidateMatches(block->entry.name, root, rootLen)) {
                continue;
            }
            bool duplicate = false;
            for (int j = 0; j < count; ++j) {
                if (_stricmp(out[j].name, block->entry.name) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            out[count].name = block->entry.name;
            out[count].detail = block->entry.signature;
            out[count].kind = block->entry.kind;
            ++count;
        }
    }
    return count;
}

// ----------------------------------------------------------------------------

typedef struct FIND_DEF_CTX {
    const char* name;
    bool found;
} FIND_DEF_CTX;

static bool _FindDefCallback(const char* pszName, const char* pszKind, void* pCtx)
{
    UNREFERENCED_PARAMETER(pszKind);
    FIND_DEF_CTX* const pFind = (FIND_DEF_CTX*)pCtx;
    if (_stricmp(pszName, pFind->name) == 0) {
        pFind->found = true;
        return true;
    }
    return false;
}

typedef struct FIND_ALLOC_CTX {
    char name[240];
    bool found;
} FIND_ALLOC_CTX;

static bool _FindAllocCallback(const char* pszName, const char* pszKind, void* pCtx)
{
    if ((_stricmp(pszKind, "alloc") == 0) ||
        (_stricmp(pszKind, "globalalloc") == 0) ||
        (_stricmp(pszKind, "sharedalloc") == 0)) {
        FIND_ALLOC_CTX* const pFind = (FIND_ALLOC_CTX*)pCtx;
        StringCchCopyA(pFind->name, COUNTOF(pFind->name), pszName);
        pFind->found = true;
        return true;
    }
    return false;
}

static void _TrimPunctuation(char* pszText)
{
    StrTrimUTF8(pszText, " \t\r\n()[];,");
    // CE scripts often use a full width comma (U+FF0C = EF BC 8C).
    size_t len = strlen(pszText);
    while ((len >= 3) && ((unsigned char)pszText[len - 3] == 0xEF) &&
           ((unsigned char)pszText[len - 2] == 0xBC) &&
           ((unsigned char)pszText[len - 1] == 0x8C)) {
        pszText[len - 3] = '\0';
        len -= 3;
    }
}

// Selection and clipboard text must name a symbol defined in the document;
// otherwise arbitrary copied text would end up inside generated code.
static bool _IsDefinedVariable(char* pszText)
{
    _TrimPunctuation(pszText);
    if (!_IsIdentifier(pszText)) {
        return false;
    }
    FIND_DEF_CTX find = { pszText, false };
    _ScanDefinitions(_FindDefCallback, &find);
    return find.found;
}

static bool _GetClipboardVariable(char* pszOut, size_t cchOut)
{
    if (!pszOut || (cchOut == 0u) || !OpenClipboard(Globals.hwndMain)) {
        return false;
    }
    bool found = false;
    HANDLE const hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const WCHAR* const pWide = (const WCHAR*)GlobalLock(hData);
        if (pWide) {
            WCHAR wide[240];
            HRESULT const hr = StringCchCopyNW(wide, COUNTOF(wide), pWide, COUNTOF(wide) - 1);
            GlobalUnlock(hData);
            if (hr == S_OK) {
                char utf8[480];
                int const cb = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                                   utf8, (int)COUNTOF(utf8), NULL, NULL);
                if ((cb > 0) && ((size_t)cb <= COUNTOF(utf8)) && ((size_t)cb <= cchOut) &&
                    _IsDefinedVariable(utf8)) {
                    StringCchCopyA(pszOut, cchOut, utf8);
                    found = true;
                }
            }
        }
    }
    CloseClipboard();
    return found;
}

// A selected identifier in the editor names the variable directly.  Like
// clipboard text it must refer to a symbol defined in the document, so
// arbitrary selections cannot end up inside generated code.
bool CeCompletion_GetSelectedVariable(char* pszOut, size_t cchOut)
{
    DocPos const selStart = SciCall_GetSelectionStart();
    DocPos const selEnd = SciCall_GetSelectionEnd();
    if (selEnd > selStart) {
        DocPos const selLen = selEnd - selStart;
        if ((selLen > 0) && (selLen < (DocPos)cchOut) && (selLen < 480)) {
            char sel[480];
            StringCchCopyNA(sel, COUNTOF(sel), SciCall_GetRangePointer(selStart, selLen), (size_t)selLen);
            if (_IsDefinedVariable(sel)) {
                StringCchCopyA(pszOut, cchOut, sel);
                return true;
            }
        }
    }
    return false;
}

bool CeCompletion_ResolveVariable(char* pszOut, size_t cchOut)
{
    if (CeCompletion_GetSelectedVariable(pszOut, cchOut)) {
        return true;
    }

    if (CeSettings_AllowClipboard()) {
        char clip[480];
        if (_GetClipboardVariable(clip, COUNTOF(clip))) {
            StringCchCopyA(pszOut, cchOut, clip);
            return true;
        }
    }

    FIND_ALLOC_CTX find = { { 0 }, false };
    _ScanDefinitions(_FindAllocCallback, &find);
    if (find.found) {
        StringCchCopyA(pszOut, cchOut, find.name);
        return true;
    }
    return false;
}

// Clipboard-only variable lookup.  Template insertion tries the selection
// first (CeCompletion_GetSelectedVariable) and falls back to this; it never
// uses the first alloc definition.
bool CeCompletion_GetClipboardVariable(char* pszOut, size_t cchOut) {
    return _GetClipboardVariable(pszOut, cchOut);
}

// ----------------------------------------------------------------------------
// Typed AA variable references: after a separator on an instruction line, wrap
// a user-defined symbol as a memory operand ("mov rax,gold " -> "mov rax,[gold] ").
// Jump/call targets and definition statements are never wrapped.
// ----------------------------------------------------------------------------

static bool _IsJumpInsn(const char* word)
{
    static const char* const s_jumps[] = {
        "jmp", "call", "je", "jne", "jz", "jnz", "ja", "jae", "jb", "jbe",
        "jg", "jge", "jl", "jle", "jc", "jnc", "jo", "jno", "js", "jns",
        "jp", "jnp", "jcxz", "jecxz", "jrcxz", "loop", "loope", "loopne",
    };
    for (int i = 0; i < (int)COUNTOF(s_jumps); ++i) {
        if (_stricmp(word, s_jumps[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool _LineStartsWithAsmInsn(DocPos caret, char* insn, size_t cchInsn)
{
    DocLn const line = SciCall_LineFromPosition(caret);
    DocPos p = SciCall_PositionFromLine(line);
    while (p < caret) {
        char const ch = (char)SciCall_GetCharAt(p);
        if ((ch != ' ') && (ch != '\t')) {
            break;
        }
        ++p;
    }
    size_t n = 0;
    while ((p < caret) && (n + 1 < cchInsn)) {
        char const ch = (char)SciCall_GetCharAt(p);
        if (!(isalnum((unsigned char)ch) || (ch == '_'))) {
            break;
        }
        insn[n++] = ch;
        ++p;
    }
    insn[n] = '\0';
    return (n > 0) && _InKeywordList(CE_KEYWORDS_ASM, insn) && !_IsJumpInsn(insn);
}

// "newmem" is Cheat Engine's built-in template symbol; never decorate it.
static bool _IsCeBuiltinVariable(const char* name)
{
    return (_stricmp(name, "newmem") == 0);
}

// Re-evaluate the line mode at completion time: the cached line-mode array is
// rebuilt on a timer and may still be stale right after a section marker.
static bool _IsAaLineAtCaret(void)
{
    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc) || (doc.lines <= 0)) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    unsigned char* const modes = AllocMem((size_t)doc.lines, HEAP_ZERO_MEMORY);
    if (!modes) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    CeScript_LineModes(&doc, modes);
    DocLn const line = SciCall_LineFromPosition(SciCall_GetCurrentPos());
    bool const isAA = ((line >= 0) && (line < doc.lines) &&
                       (modes[line] == (unsigned char)CE_LANG_AA));
    FreeMem(modes);
    CeScript_FreeDoc(&doc);
    return isAA;
}

// True only for alloc-family symbols (alloc/globalalloc/sharedalloc).  Only
// these are wrapped as "[name]" memory operands; labels, defines, registered
// symbols and aobscan results are code addresses and must stay bare.
static bool _IsAllocDocumentSymbol(const char* name)
{
    if (StrIsEmptyA(name)) {
        return false;
    }
    // The symbol index is the same one completion offers, so wrapping accepts
    // exactly what the user can see in the list.  A huge document is excluded
    // from that index; keep the precise scan for it.
    if (_EnsureDocSymbols()) {
        int lo = 0;
        int hi = s_docSymCount - 1;
        while (lo <= hi) {
            int const mid = lo + ((hi - lo) / 2);
            int const cmp = _stricmp(s_docSyms[mid].name, name);
            if (cmp == 0) {
                return s_docSyms[mid].isAlloc;
            }
            if (cmp < 0) {
                lo = mid + 1;
            }
            else {
                hi = mid - 1;
            }
        }
        return false;
    }

    CE_DOC doc = { 0 };
    if (!CeScript_InitDoc(&doc)) {
        return false;
    }
    CE_DEF* const defs = AllocMem(sizeof(CE_DEF) * CE_MAX_DEFS, HEAP_ZERO_MEMORY);
    if (!defs) {
        CeScript_FreeDoc(&doc);
        return false;
    }
    int const count = CeScript_CollectDefs(&doc, defs, CE_MAX_DEFS);
    bool found = false;
    for (int i = 0; (i < count) && !found; ++i) {
        if (_stricmp(defs[i].name, name) == 0) {
            found = (_stricmp(defs[i].kind, "alloc") == 0) ||
                    (_stricmp(defs[i].kind, "globalalloc") == 0) ||
                    (_stricmp(defs[i].kind, "sharedalloc") == 0);
        }
    }
    FreeMem(defs);
    CeScript_FreeDoc(&doc);
    return found;
}

static bool _WrapTypedAsmSymbol(int ch)
{
    if (!CeSettings_WrapAsmSymbols() || !CeCompletion_IsActive()) {
        return false;
    }
    if (!((ch == ' ') || (ch == '\t') || (ch == ',') || (ch == '\r') || (ch == '\n'))) {
        return false;
    }
    DocPos const caret = SciCall_GetCurrentPos();
    char insn[32];
    if (!_LineStartsWithAsmInsn(caret, insn, COUNTOF(insn))) {
        return false;
    }

    DocPos end = caret - 1;
    while ((end > 0) && ((SciCall_GetCharAt(end - 1) == ' ') || (SciCall_GetCharAt(end - 1) == '\t'))) {
        --end;
    }
    DocPos p = end;
    while (p > 0) {
        char const c = (char)SciCall_GetCharAt(p - 1);
        if (!(isalnum((unsigned char)c) || (c == '_') || (c == '.') || (c == '$'))) {
            break;
        }
        --p;
    }
    if (p == end) {
        return false;
    }
    size_t const len = (size_t)(end - p);
    if ((len == 0) || (len >= 64)) {
        return false;
    }
    char name[64];
    for (size_t i = 0; i < len; ++i) {
        name[i] = (char)SciCall_GetCharAt(p + (DocPos)i);
    }
    name[len] = '\0';
    if (!_IsIdentifier(name) || _IsCeBuiltinVariable(name) || CeCompletion_IsBuiltinKeyword(name)) {
        return false;
    }
    DocPos before = p;
    while ((before > 0) && ((SciCall_GetCharAt(before - 1) == ' ') || (SciCall_GetCharAt(before - 1) == '\t'))) {
        --before;
    }
    if ((before > 0) && (SciCall_GetCharAt(before - 1) == '[')) {
        return false; // already a memory operand
    }
    if (!_IsAllocDocumentSymbol(name)) {
        return false; // only alloc-family symbols are wrapped
    }

    char wrapped[80];
    StringCchPrintfA(wrapped, COUNTOF(wrapped), "[%s]", name);
    SciCall_BeginUndoAction();
    SciCall_SetSel(p, end);
    SciCall_ReplaceSel(wrapped);
    SciCall_EndUndoAction();
    return true;
}

// ----------------------------------------------------------------------------

static void _HideCeTip(void);
static void _MeasureCeTip(int forcedWidth);
static void _PositionCeTip(void);
static bool _GetCeListRect(RECT* pRect);

bool CeCompletion_HandleCharAdded(HWND hwnd, int ch)
{
    _HideCeTip();
    // A new root means a new list.  AutoCCancel() does not emit
    // SCN_AUTOCCANCELLED, so the previous list's highlighted item would
    // otherwise survive and later replace the word the user really accepted.
    s_lastHighlight[0] = '\0';
    UNREFERENCED_PARAMETER(hwnd);

    if (!CeCompletion_IsActive()) {
        return false;
    }
    if (_WrapTypedAsmSymbol(ch)) {
        // This path skips EditAutoCompleteWord(), so dismiss any stale list.
        if (SciCall_AutoCActive()) {
            SciCall_AutoCCancel();
        }
        return true;
    }
    if (ch == '.') {
        return _ShowAllMembers();
    }
    if (ch == '(') {
        // Typing '(' ends the completion; leaving the list open makes every
        // following keystroke inert until the word is deleted and retyped.
        if (SciCall_AutoCActive()) {
            SciCall_AutoCCancel();
        }
        _ShowCallTipForFunctionBeforeCaret();
        return true;
    }
    return false;
}

bool CeCompletion_OnCharAddedForLsp(int ch)
{
    if (!CeCompletion_IsActive()) {
        return false;
    }
    _HideCeTip();
    s_lastHighlight[0] = '\0';
    return _WrapTypedAsmSymbol(ch);
}

// ----------------------------------------------------------------------------
// Completion images and detail pane
//
// Scintilla's CallTipShow() cancels the autocompletion list, so the per-item
// documentation cannot be a calltip.  A free-floating tracking tooltip used to
// sit under the caret and overlap the list; this modeless WS_EX_NOACTIVATE pane
// is instead docked flush under the AutoC ListBoxX window so both windows read
// as one card: same width, no gap, and the pane draws its own thin frame
// because WS_BORDER would add a second border along the seam.
// ----------------------------------------------------------------------------

#define CE_GLYPH_W     9
#define CE_GLYPH_H     9
#define CE_TIP_PAD     10
#define CE_TIP_MIN_W   360   // logical pixels at 96 DPI
#define CE_TIP_MAX_W   720
#define CE_TIP_MIN_H   62
#define CE_TIP_MAX_H   420   // enough for a multiline usage note
#define CE_AUTOC_MAX_W 80    // characters in the AutoC list
#define CE_AUTOC_MAX_H 12    // rows in the AutoC list
// The list can be resized by dragging its frame; polling keeps the docked pane
// glued to it without subclassing the vendored ListBoxX window.
#define CE_TIP_FOLLOW_TIMER 1
#define CE_TIP_FOLLOW_MS    150

// 5x7 bitmaps for the per-category prefix glyphs (bit 4 is the leftmost pixel).
static const unsigned char s_kindGlyph[CE_KIND_COUNT][7] = {
    { 0, 0, 0, 0, 0, 0, 0 },                    // none
    { 17, 18, 20, 24, 20, 18, 17 },             // K - keyword
    { 31, 16, 16, 30, 16, 16, 16 },             // F - function / CE API
    { 17, 17, 17, 17, 17, 10, 4 },              // V - variable
    { 14, 17, 17, 31, 17, 17, 17 },             // A - Auto Assembler
    { 17, 27, 21, 21, 17, 17, 17 },             // M - mnemonic / register
    { 31, 4, 4, 4, 4, 4, 4 },                   // T - type / class
    { 15, 16, 16, 16, 16, 16, 15 },             // C - constant
    { 15, 16, 16, 14, 1, 1, 30 },               // S - section marker
};

// Same palette as the reference editor's --c-* colours (light theme).
static const COLORREF s_kindColour[CE_KIND_COUNT] = {
    RGB(0x8B, 0x94, 0x9E), // none    (--c-wild)
    RGB(0xCF, 0x22, 0x2E), // keyword (--c-kw)
    RGB(0x82, 0x50, 0xDF), // function(--c-fn)
    RGB(0x1F, 0x88, 0x3D), // variable(--c-aacmd)
    RGB(0xCF, 0x22, 0x2E), // AA      (--c-aadir)
    RGB(0x09, 0x69, 0xDA), // asm     (--c-mne)
    RGB(0x82, 0x50, 0xDF), // type    (--c-size)
    RGB(0x05, 0x50, 0xAE), // constant(--c-const)
    RGB(0x82, 0x50, 0xDF), // section (--c-aamark)
};

static const WCHAR s_ceTipClass[] = L"NP3_CE_COMPLETION_DETAIL";
static HWND s_hwndCeTip = NULL;
static WCHAR s_ceTipHead[512];
static WCHAR s_ceTipSyntax[512];
static WCHAR s_ceTipBody[4096];
static COLORREF s_ceTipHeadColour = RGB(0x8B, 0x94, 0x9E);
static COLORREF s_ceTipSyntaxColour = RGB(0x09, 0x69, 0xDA);
static int s_ceTipWidth = 420;
static int s_ceTipHeight = 72;
static int s_ceTipHeadH = 18;
static int s_ceTipPad = CE_TIP_PAD;
// Last AutoC list rect the pane was docked to; the follow timer compares
// against it to notice a manual resize of the list.
static RECT s_ceTipDockRect = { 0, 0, 0, 0 };

// Syntax fragment colours: catalogue signature (blue) vs. a name that has no
// catalogue entry (red).
#define CE_TIP_SYNTAX_KNOWN  RGB(0x09, 0x69, 0xDA)
#define CE_TIP_SYNTAX_WARN   RGB(0xCF, 0x22, 0x2E)
#define CE_TIP_HEAD_GAP      8

// Complete reference pane (toggled by a command, anchored on the upper right
// of the detail pane).
#define CE_DOC_GAP   12
#define CE_DOC_PAD   CE_TIP_PAD

static const WCHAR s_ceDocClass[] = L"NP3_CE_COMPLETION_REFERENCE";
static HWND s_hwndCeDoc = NULL;
static WCHAR s_ceDocTitle[512];
static WCHAR s_ceDocSyntax[512];
static WCHAR s_ceDocBody[8192];
static int s_ceDocScroll = 0;
static int s_ceDocTextH = 0;
static int s_ceDocPad = CE_DOC_PAD;
static bool s_ceDocWarnSyntax = false;
// True when the pane was filled from the completion list: it then mirrors the
// highlighted item and disappears together with that list.  A pane opened from
// the caret (no list) stays until the command toggles it off.
static bool s_ceDocFollowList = false;

static LRESULT CALLBACK _CeDocWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void _HideCeDoc(void);

// WM_PAINT reserves pad+4 px on the left and pad px on the right of the client
// area for the body text, plus the 1 px frame drawn inside the client area.
// Measuring at any other width makes DT_WORDBREAK wrap differently at paint
// time and clips the last line.
static int _CeTipBodyWidth(int clientCx, int pad)
{
    return (clientCx > (2 * pad + 6)) ? (clientCx - 2 * pad - 6) : 1;
}

static int _CeTipClientHeight(int pad, int headH, int bodyH)
{
    return (3 * pad) + headH + bodyH;
}

// The pane has no WS_BORDER; the frame is painted inside the client area, so
// only ask the window for a non-client size to stay exact on mixed-DPI setups.
static void _CeTipFrameSize(int* pCx, int* pCy)
{
    RECT rcWin = { 0, 0, 0, 0 };
    RECT rcClient = { 0, 0, 0, 0 };
    if (s_hwndCeTip && GetWindowRect(s_hwndCeTip, &rcWin) &&
            GetClientRect(s_hwndCeTip, &rcClient)) {
        *pCx = (rcWin.right - rcWin.left) - (rcClient.right - rcClient.left);
        *pCy = (rcWin.bottom - rcWin.top) - (rcClient.bottom - rcClient.top);
    }
    else {
        RECT rcFrame = { 0, 0, 0, 0 };
        AdjustWindowRectEx(&rcFrame, WS_POPUP, FALSE,
                           WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        *pCx = rcFrame.right - rcFrame.left;
        *pCy = rcFrame.bottom - rcFrame.top;
    }
}

static void _RegisterKindImages(void)
{
    char pixels[CE_GLYPH_W * CE_GLYPH_H * 4];
    SciCall_RGBAImageSetWidth(CE_GLYPH_W);
    SciCall_RGBAImageSetHeight(CE_GLYPH_H);

    for (int kind = 1; kind < CE_KIND_COUNT; ++kind) {
        COLORREF const colour = s_kindColour[kind];
        for (int y = 0; y < CE_GLYPH_H; ++y) {
            for (int x = 0; x < CE_GLYPH_W; ++x) {
                unsigned char* const p = (unsigned char*)pixels + ((y * CE_GLYPH_W + x) * 4);
                bool const isCorner = ((x == 0) || (x == CE_GLYPH_W - 1)) &&
                                      ((y == 0) || (y == CE_GLYPH_H - 1));
                bool const isGlyph = (y >= 1) && (y <= 7) && (x >= 2) && (x <= 6) &&
                                     ((s_kindGlyph[kind][y - 1] >> (6 - x)) & 1);
                if (isCorner) {
                    p[0] = p[1] = p[2] = p[3] = 0;
                }
                else {
                    p[0] = isGlyph ? (BYTE)255 : GetRValue(colour);
                    p[1] = isGlyph ? (BYTE)255 : GetGValue(colour);
                    p[2] = isGlyph ? (BYTE)255 : GetBValue(colour);
                    p[3] = 255;
                }
            }
        }
        SciCall_RegisterRGBAImage(kind, pixels);
    }
}

static LRESULT CALLBACK _CeTipWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC const hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH const hbr = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
            FillRect(hdc, &rc, hbr);
            DeleteObject(hbr);
            // Category colour strip along the left edge (inside the frame).
            RECT rcStrip = { 1, 1, 4, rc.bottom - 1 };
            HBRUSH const hbrStrip = CreateSolidBrush(s_ceTipHeadColour);
            FillRect(hdc, &rcStrip, hbrStrip);
            DeleteObject(hbrStrip);
            // The AutoC list above owns the thick frame; a thin frame makes this
            // pane read as the lower half of the same card.
            HPEN const hPenFrame = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
            HPEN const hPenFrameOld = (HPEN)SelectObject(hdc, hPenFrame);
            HBRUSH const hbrNull = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH const hbrNullOld = (HBRUSH)SelectObject(hdc, hbrNull);
            Rectangle(hdc, 0, 0, rc.right, rc.bottom);
            SelectObject(hdc, hbrNullOld);
            SelectObject(hdc, hPenFrameOld);
            DeleteObject(hPenFrame);
            HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HFONT const hOld = (HFONT)SelectObject(hdc, hFont);
            SetBkMode(hdc, TRANSPARENT);
            int const pad = s_ceTipPad;
            RECT rcHead = { pad + 4, pad, rc.right - pad, pad + s_ceTipHeadH };
            SetTextColor(hdc, s_ceTipHeadColour);
            DrawTextW(hdc, s_ceTipHead, -1, &rcHead,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (s_ceTipSyntax[0]) {
                // The syntax fragment follows the keyword on the same line.
                RECT rcHeadCalc = { 0, 0, 0, 0 };
                DrawTextW(hdc, s_ceTipHead, -1, &rcHeadCalc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                int const gap = MulDiv(CE_TIP_HEAD_GAP, s_ceTipPad, CE_TIP_PAD);
                RECT rcSyntax = { rcHead.left + (rcHeadCalc.right - rcHeadCalc.left) + gap, pad,
                                  rc.right - pad, pad + s_ceTipHeadH };
                SetTextColor(hdc, s_ceTipSyntaxColour);
                DrawTextW(hdc, s_ceTipSyntax, -1, &rcSyntax,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if (s_ceTipBody[0]) {
                int const sepY = pad + s_ceTipHeadH + (pad / 2);
                HPEN const hPen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DLIGHT));
                HPEN const hPenOld = (HPEN)SelectObject(hdc, hPen);
                MoveToEx(hdc, pad + 4, sepY, NULL);
                LineTo(hdc, rc.right - pad, sepY);
                SelectObject(hdc, hPenOld);
                DeleteObject(hPen);
                RECT rcBody = { pad + 4, sepY + (pad / 2),
                                pad + 4 + _CeTipBodyWidth(rc.right, pad), rc.bottom - pad };
                SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
                DrawTextW(hdc, s_ceTipBody, -1, &rcBody, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
            SelectObject(hdc, hOld);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == CE_TIP_FOLLOW_TIMER) {
            if (IsRectEmpty(&s_ceTipDockRect)) {
                return 0;   // hover / calltip path: nothing to follow
            }
            RECT rcList = { 0, 0, 0, 0 };
            if (!_GetCeListRect(&rcList)) {
                _HideCeTip();
            }
            else if (!EqualRect(&rcList, &s_ceTipDockRect)) {
                _MeasureCeTip(rcList.right - rcList.left);
                _PositionCeTip();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void CeCompletion_ApplyRuntime(void)
{
    _RegisterKindImages();
    SciCall_AutoCSetTypeSeparator('?'); // "word?N" items, N = CE_KIND_* image id
    SciCall_AutoCSetIgnoreCase(true);
    SciCall_AutoCSetAutoHide(false);
    SciCall_AutoCSetDropRestOfWord(true);
    SciCall_AutoCSetMaxWidth(CE_AUTOC_MAX_W);
    SciCall_AutoCSetMaxHeight(CE_AUTOC_MAX_H);
    if (!s_dictionaryLoaded) {
        _LoadDictionary();
    }
    CeCompletion_InvalidateDocSymbols();
    _InvalidateWordListCache();
}

void CeCompletion_Init(void)
{
    _BuildCatalogueIndex();
    _BuildKeywordSets();
    CeCompletion_ApplyRuntime();
    _BuildExtraWords();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = _CeTipWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = s_ceTipClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = _CeDocWndProc;
    wc.lpszClassName = s_ceDocClass;
    RegisterClassExW(&wc);
}

void CeCompletion_Destroy(void)
{
    _HideCeTip();
    if (s_hwndCeTip && IsWindow(s_hwndCeTip)) {
        DestroyWindow(s_hwndCeTip);
    }
    s_hwndCeTip = NULL;
    UnregisterClassW(s_ceTipClass, GetModuleHandleW(NULL));
    _HideCeDoc();
    if (s_hwndCeDoc && IsWindow(s_hwndCeDoc)) {
        DestroyWindow(s_hwndCeDoc);
    }
    s_hwndCeDoc = NULL;
    UnregisterClassW(s_ceDocClass, GetModuleHandleW(NULL));

    for (int i = 0; i < s_docSymCount; ++i) {
        FreeMem(s_docSyms[i].name);
    }
    FreeMem(s_docSyms);
    s_docSyms = NULL;
    s_docSymCount = 0;
    FreeMem(s_ceLineModes);
    s_ceLineModes = NULL;
    s_ceLineModeCount = 0;
    FreeMem(s_extraWords);
    s_extraWords = NULL;
    s_extraCount = 0;
    _FreeKeywordSets();
    FreeMem(s_catalogueByName);
    s_catalogueByName = NULL;
    FreeMem(s_candScratch);
    s_candScratch = NULL;
    s_candScratchCap = 0;
    FreeMem(s_seen);
    s_seen = NULL;
    s_seenCap = 0;
    s_seenGen = 0;
    FreeMem(s_luaModel);
    s_luaModel = NULL;
    FreeMem(s_lastList);
    s_lastList = NULL;
    _InvalidateWordListCache();
    _FreeDictionary();
    s_dictionaryLoaded = false;
    s_docSymDirty = true;
    s_docSymLastBuild = 0;
}

static void _EnsureCeTip(void)
{
    if (s_hwndCeTip && IsWindow(s_hwndCeTip)) {
        return;
    }
    s_hwndCeTip = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        s_ceTipClass, NULL, WS_POPUP,
        0, 0, s_ceTipWidth, s_ceTipHeight,
        Globals.hwndMain, NULL, GetModuleHandleW(NULL), NULL);
}

static void _HideCeTip(void)
{
    if (s_hwndCeTip && IsWindow(s_hwndCeTip)) {
        KillTimer(s_hwndCeTip, CE_TIP_FOLLOW_TIMER);
        ShowWindow(s_hwndCeTip, SW_HIDE);
    }
    SetRectEmpty(&s_ceTipDockRect);
}

void CeCompletion_HideTip(void)
{
    _HideCeTip();
    // The reference pane follows the completion list: once that list is gone
    // (accepted, cancelled, ESC, focus loss) the pane closes with it.  A pane
    // opened from the caret stays until the command toggles it off.
    if (s_ceDocFollowList && !SciCall_AutoCActive()) {
        _HideCeDoc();
    }
}

// Size the pane to its actual text instead of clipping long signatures.  When
// docked under the AutoC list, forcedWidth is that list's width and the pane
// must adopt it exactly so both windows share one outline.  The body must be
// word-wrapped at the very width WM_PAINT draws it at: the 1 px frame is
// painted inside the client area and the body rect starts at pad+4.
static void _MeasureCeTip(int forcedWidth)
{
    HDC const hdc = GetDC(s_hwndCeTip);
    if (!hdc) {
        return;
    }
    int const dpi = max_i(GetDeviceCaps(hdc, LOGPIXELSX), USER_DEFAULT_SCREEN_DPI);
    int const minW = MulDiv(CE_TIP_MIN_W, dpi, 96);
    int const maxW = MulDiv(CE_TIP_MAX_W, dpi, 96);
    int const minH = MulDiv(CE_TIP_MIN_H, dpi, 96);
    int const maxH = MulDiv(CE_TIP_MAX_H, dpi, 96);
    s_ceTipPad = MulDiv(CE_TIP_PAD, dpi, 96);

    int frameCx = 0;
    int frameCy = 0;
    _CeTipFrameSize(&frameCx, &frameCy);

    HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT const hOld = (HFONT)SelectObject(hdc, hFont);

    RECT rcHead = { 0, 0, 0, 0 };
    DrawTextW(hdc, s_ceTipHead, -1, &rcHead, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    s_ceTipHeadH = rcHead.bottom - rcHead.top;
    int headW = rcHead.right - rcHead.left;
    if (s_ceTipSyntax[0]) {
        RECT rcSyntax = { 0, 0, 0, 0 };
        DrawTextW(hdc, s_ceTipSyntax, -1, &rcSyntax, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        headW += MulDiv(CE_TIP_HEAD_GAP, s_ceTipPad, CE_TIP_PAD) + (rcSyntax.right - rcSyntax.left);
    }

    int width = 0;
    if (forcedWidth > 0) {
        // A manually resized list can be narrower than CE_TIP_MIN_W; the text
        // then wraps (and the head is ellipsized) instead of widening the card.
        width = forcedWidth;
    }
    else {
        RECT rcProbe = { 0, 0, _CeTipBodyWidth(maxW - frameCx, s_ceTipPad), 0 };
        DrawTextW(hdc, s_ceTipBody, -1, &rcProbe, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        width = max_i(minW, max_i(headW, rcProbe.right - rcProbe.left) +
                                     (2 * s_ceTipPad) + 6 + frameCx);
        width = clampi(width, minW, maxW);
    }

    int bodyH = 0;
    if (s_ceTipBody[0]) {
        RECT rcBody = { 0, 0, _CeTipBodyWidth(width - frameCx, s_ceTipPad), 0 };
        DrawTextW(hdc, s_ceTipBody, -1, &rcBody, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        bodyH = rcBody.bottom - rcBody.top;
    }
    // Without a note the pane is exactly one line high (no separator strip and
    // no minimum height), so nothing that looks like an info bar can appear
    // below the keyword.
    s_ceTipWidth = width;
    s_ceTipHeight = (bodyH > 0) ?
                    clampi(_CeTipClientHeight(s_ceTipPad, s_ceTipHeadH, bodyH) + frameCy, minH, maxH) :
                    ((2 * s_ceTipPad) + s_ceTipHeadH + frameCy);

    SelectObject(hdc, hOld);
    ReleaseDC(s_hwndCeTip, hdc);
}

// Scintilla creates its AutoC ListBoxX as a WS_POPUP top-level window.  Only
// accept the one owned by this process: another Notepad3 instance may have its
// list open at the same time.
static bool _GetCeListRect(RECT* pRect)
{
    SetRectEmpty(pRect);
    HWND const hwndList = FindWindowW(L"ListBoxX", NULL);
    if (!hwndList || !IsWindowVisible(hwndList)) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwndList, &pid);
    if (pid != GetCurrentProcessId()) {
        return false;
    }
    return (GetWindowRect(hwndList, pRect) != 0) && (IsRectEmpty(pRect) == 0);
}

static void _PositionCeTip(void)
{
    RECT rcList = { 0, 0, 0, 0 };
    bool const docked = _GetCeListRect(&rcList);
    if (docked) {
        s_ceTipDockRect = rcList;
    }
    else {
        SetRectEmpty(&s_ceTipDockRect);
        // No completion list: this is the hover path, so anchor the pane on the
        // mouse.  Anchoring on the caret made the tip appear far away (or off
        // screen) whenever the caret was not under the mouse, which looked like
        // "hovering shows nothing".
        POINT pt = { 0, 0 };
        bool atMouse = false;
        if (GetCursorPos(&pt)) {
            RECT rcEdit;
            GetWindowRect(Globals.hwndEdit, &rcEdit);
            atMouse = (PtInRect(&rcEdit, pt) != 0);
        }
        if (atMouse) {
            rcList.left = pt.x + 10;
            rcList.top = pt.y + 18;
        }
        else {
            DocPos const pos = SciCall_GetCurrentPos();
            DocLn const line = SciCall_LineFromPosition(pos);
            int const lineHeight = SciCall_TextHeight(line);
            pt.x = SciCall_PointXFromPosition(pos);
            pt.y = SciCall_PointYFromPosition(pos) + lineHeight;
            ClientToScreen(Globals.hwndEdit, &pt);
            rcList.left = pt.x;
            rcList.top = pt.y;
        }
        rcList.right = rcList.left + s_ceTipWidth;
        rcList.bottom = rcList.top;   // the pane opens just below the anchor
    }

    int const width = s_ceTipWidth;
    RECT rcWork = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    int x = rcList.left;
    int y = rcList.bottom;   // flush under the list: no gap, one card
    if ((y + s_ceTipHeight) > rcWork.bottom) {
        y = rcList.top - s_ceTipHeight;
    }
    if (y < rcWork.top) {
        y = rcWork.top;
    }
    if ((x + width) > rcWork.right) {
        x = rcWork.right - width;
    }
    if (x < rcWork.left) {
        x = rcWork.left;
    }
    SetWindowPos(s_hwndCeTip, HWND_TOPMOST, x, y, width, s_ceTipHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void _ShowCeTipEx(const char* utf8Head, const char* utf8Syntax, bool warnSyntax,
                         const char* utf8Body, int kind)
{
    _EnsureCeTip();
    if (!s_hwndCeTip) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8Head, -1, s_ceTipHead, (int)COUNTOF(s_ceTipHead));
    MultiByteToWideChar(CP_UTF8, 0, utf8Body, -1, s_ceTipBody, (int)COUNTOF(s_ceTipBody));
    MultiByteToWideChar(CP_UTF8, 0, utf8Syntax, -1, s_ceTipSyntax, (int)COUNTOF(s_ceTipSyntax));
    s_ceTipHead[COUNTOF(s_ceTipHead) - 1] = L'\0';
    s_ceTipBody[COUNTOF(s_ceTipBody) - 1] = L'\0';
    s_ceTipSyntax[COUNTOF(s_ceTipSyntax) - 1] = L'\0';
    s_ceTipHeadColour = ((kind > CE_KIND_NONE) && (kind < CE_KIND_COUNT)) ?
                        s_kindColour[kind] : s_kindColour[CE_KIND_NONE];
    s_ceTipSyntaxColour = warnSyntax ? CE_TIP_SYNTAX_WARN : CE_TIP_SYNTAX_KNOWN;
    // Docked under the AutoC list: adopt the list width so both windows read as
    // one card.  Without a list (hover / calltip) size to the pane's own text.
    RECT rcList = { 0, 0, 0, 0 };
    int const dockWidth = _GetCeListRect(&rcList) ? (rcList.right - rcList.left) : 0;
    _MeasureCeTip(dockWidth);
    _PositionCeTip();
    if (IsRectEmpty(&s_ceTipDockRect) == 0) {
        SetTimer(s_hwndCeTip, CE_TIP_FOLLOW_TIMER, CE_TIP_FOLLOW_MS, NULL);
    }
    InvalidateRect(s_hwndCeTip, NULL, TRUE);
}

static void _ShowCeTip(const char* utf8Head, const char* utf8Body, int kind)
{
    _ShowCeTipEx(utf8Head, "", false, utf8Body, kind);
}

void CeCompletion_ShowTipEx(const char* utf8Head, const char* utf8Body, int kind)
{
    _ShowCeTip(utf8Head ? utf8Head : "", utf8Body ? utf8Body : "", kind);
}

void CeCompletion_ShowTipWithSyntax(const char* utf8Head, const char* utf8Syntax, bool warnSyntax,
                                    const char* utf8Body, int kind)
{
    _ShowCeTipEx(utf8Head ? utf8Head : "", utf8Syntax ? utf8Syntax : "", warnSyntax,
                 utf8Body ? utf8Body : "", kind);
}

// ----------------------------------------------------------------------------
// Complete reference pane
// ----------------------------------------------------------------------------

static void _EnsureCeDoc(void)
{
    if (s_hwndCeDoc && IsWindow(s_hwndCeDoc)) {
        return;
    }
    s_hwndCeDoc = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        s_ceDocClass, NULL, WS_POPUP | WS_BORDER,
        0, 0, 520, 320,
        Globals.hwndMain, NULL, GetModuleHandleW(NULL), NULL);
}

static void _HideCeDoc(void)
{
    if (s_hwndCeDoc && IsWindow(s_hwndCeDoc)) {
        ShowWindow(s_hwndCeDoc, SW_HIDE);
    }
    s_ceDocScroll = 0;
    s_ceDocFollowList = false;
}

void CeCompletion_HideDocWindow(void)
{
    _HideCeDoc();
}

// Title line, then the syntax fragment, then the separator and the complete
// note.  The body scrolls with the mouse wheel; everything else is static.
static void _PaintCeDoc(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC const hdc = BeginPaint(hwnd, &ps);
    if (!hdc) {
        return;
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
    HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT const hOld = (HFONT)SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    int const pad = s_ceDocPad;
    TEXTMETRICW tm = { 0 };
    GetTextMetricsW(hdc, &tm);
    int const lineH = max_i((int)tm.tmHeight + 2, 16);

    RECT rcTitle = { pad, pad, rc.right - pad, pad + lineH };
    SetTextColor(hdc, GetSysColor(COLOR_HOTLIGHT));
    DrawTextW(hdc, s_ceDocTitle, -1, &rcTitle, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    if (s_ceDocSyntax[0]) {
        RECT rcTitleCalc = { 0, 0, 0, 0 };
        DrawTextW(hdc, s_ceDocTitle, -1, &rcTitleCalc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        RECT rcSyntax = { rcTitle.left + (rcTitleCalc.right - rcTitleCalc.left) + CE_TIP_HEAD_GAP, pad,
                          rc.right - pad, pad + lineH };
        SetTextColor(hdc, s_ceDocWarnSyntax ? CE_TIP_SYNTAX_WARN : CE_TIP_SYNTAX_KNOWN);
        DrawTextW(hdc, s_ceDocSyntax, -1, &rcSyntax,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    int const sepY = pad + lineH + (pad / 2);
    HPEN const hPen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DLIGHT));
    HPEN const hPenOld = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, pad, sepY, NULL);
    LineTo(hdc, rc.right - pad, sepY);
    SelectObject(hdc, hPenOld);
    DeleteObject(hPen);

    int const bodyTop = sepY + (pad / 2);
    RECT rcBody = { pad + 2, bodyTop - s_ceDocScroll, rc.right - pad, rc.bottom - pad };
    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(hdc, s_ceDocBody, -1, &rcBody, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);

    SelectObject(hdc, hOld);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK _CeDocWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
        _PaintCeDoc(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL: {
        int const delta = GET_WHEEL_DELTA_WPARAM(wParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int const page = rc.bottom - rc.top;
        int const maxScroll = max_i(s_ceDocTextH - page + (s_ceDocPad * 2), 0);
        s_ceDocScroll = clampi(s_ceDocScroll - ((delta * 48) / WHEEL_DELTA), 0, maxScroll);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(Globals.hwndEdit);
        _HideCeDoc();
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The keyword to describe: the active completion item, the selected text, or
// the word under the caret.
static bool _CurrentKeyword(char* pszOut, size_t cchOut)
{
    pszOut[0] = '\0';
    if (SciCall_AutoCActive() && !StrIsEmptyA(s_lastHighlight)) {
        StringCchCopyA(pszOut, cchOut, s_lastHighlight);
        return true;
    }
    DocPos const selStart = SciCall_GetSelectionStart();
    DocPos const selEnd = SciCall_GetSelectionEnd();
    if ((selEnd > selStart) && ((selEnd - selStart) < (DocPos)(cchOut - 1))) {
        char tmp[256];
        size_t const len = (size_t)(selEnd - selStart);
        memcpy(tmp, SciCall_GetRangePointer(selStart, (DocPos)len), len);
        tmp[len] = '\0';
        if (strpbrk(tmp, " \t\r\n") == NULL) {
            StringCchCopyA(pszOut, cchOut, tmp);
            return true;
        }
    }
    DocPos const pos = SciCall_GetCurrentPos();
    DocPos const start = SciCall_WordStartPosition(pos, true);
    DocPos const end = SciCall_WordEndPosition(pos, true);
    if (end <= start) {
        return false;
    }
    DocPos lenDp = end - start;
    if (lenDp > (DocPos)(cchOut - 1)) {
        lenDp = (DocPos)(cchOut - 1);
    }
    size_t const len = (size_t)lenDp;
    memcpy(pszOut, SciCall_GetRangePointer(start, (DocPos)len), len);
    pszOut[len] = '\0';
    return true;
}

static void _PositionCeDoc(int width, int height)
{
    RECT rcAnchor = { 0, 0, 0, 0 };
    if (s_hwndCeTip && IsWindowVisible(s_hwndCeTip)) {
        GetWindowRect(s_hwndCeTip, &rcAnchor);
    }
    if (IsRectEmpty(&rcAnchor)) {
        DocPos const pos = SciCall_GetCurrentPos();
        POINT pt = { SciCall_PointXFromPosition(pos), SciCall_PointYFromPosition(pos) };
        ClientToScreen(Globals.hwndEdit, &pt);
        rcAnchor.left = pt.x;
        rcAnchor.top = pt.y;
        rcAnchor.right = pt.x + 1;
        rcAnchor.bottom = pt.y + 1;
    }
    RECT rcWork = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    int const gap = MulDiv(CE_DOC_GAP, s_ceDocPad, CE_TIP_PAD);
    // Upper right of the detail pane, keeping the gap; flip to the left side
    // when the work area has no room there.
    int x = rcAnchor.right + gap;
    int y = rcAnchor.top;
    if ((x + width) > rcWork.right) {
        x = rcAnchor.left - width - gap;
    }
    if (x < rcWork.left) {
        x = rcWork.left;
    }
    if ((y + height) > rcWork.bottom) {
        y = rcWork.bottom - height;
    }
    if (y < rcWork.top) {
        y = rcWork.top;
    }
    SetWindowPos(s_hwndCeDoc, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Fill the reference pane with one keyword and show it.  Names that are not in
// the CE catalogue are still shown (red syntax) so the pane never keeps the
// documentation of a previously highlighted item.
static void _ShowCeDoc(const char* pszName)
{
    const CE_COMPLETION_ENTRY* const pEntry = CeCompletion_Find(pszName);
    const char* const pszDoc = CeCompletion_FindDoc(pszName);

    char head[512];
    char sig[2048];
    if (pEntry) {
        StringCchPrintfA(head, COUNTOF(head), "%s  \xC2\xB7  %s", pEntry->category, pEntry->name);
        char localized[1024];
        StringCchCopyA(sig, COUNTOF(sig), _LocalizeSignature(pEntry, localized, COUNTOF(localized)));
    }
    else {
        StringCchPrintfA(head, COUNTOF(head), "\xE8\xAF\x8D\xE8\xA1\xA8  \xC2\xB7  %s", pszName); // 词表
        StringCchCopyA(sig, COUNTOF(sig), pszName);
    }
    s_ceDocWarnSyntax = (!pEntry && StrIsEmptyA(pszDoc));

    MultiByteToWideChar(CP_UTF8, 0, head, -1, s_ceDocTitle, (int)COUNTOF(s_ceDocTitle));
    MultiByteToWideChar(CP_UTF8, 0, sig, -1, s_ceDocSyntax, (int)COUNTOF(s_ceDocSyntax));
    MultiByteToWideChar(CP_UTF8, 0, StrIsEmptyA(pszDoc) ? "" : pszDoc, -1,
                        s_ceDocBody, (int)COUNTOF(s_ceDocBody));
    s_ceDocTitle[COUNTOF(s_ceDocTitle) - 1] = L'\0';
    s_ceDocSyntax[COUNTOF(s_ceDocSyntax) - 1] = L'\0';
    s_ceDocBody[COUNTOF(s_ceDocBody) - 1] = L'\0';
    s_ceDocScroll = 0;

    _EnsureCeDoc();
    if (!s_hwndCeDoc) {
        return;
    }
    HDC const hdc = GetDC(s_hwndCeDoc);
    int width = 520;
    int height = 320;
    if (hdc) {
        int const dpi = max_i(GetDeviceCaps(hdc, LOGPIXELSX), USER_DEFAULT_SCREEN_DPI);
        s_ceDocPad = MulDiv(CE_DOC_PAD, dpi, USER_DEFAULT_SCREEN_DPI);
        HFONT const hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT const hOld = (HFONT)SelectObject(hdc, hFont);
        int const textW = MulDiv(440, dpi, USER_DEFAULT_SCREEN_DPI);
        RECT rcCalc = { 0, 0, textW, 0 };
        DrawTextW(hdc, s_ceDocBody, -1, &rcCalc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        s_ceDocTextH = rcCalc.bottom - rcCalc.top;
        width = textW + (s_ceDocPad * 2) + 8;
        height = s_ceDocTextH + (s_ceDocPad * 4) + 40;
        SelectObject(hdc, hOld);
        ReleaseDC(s_hwndCeDoc, hdc);
    }
    RECT rcWork = { 0, 0, 0, 0 };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    width = min_i(width, (rcWork.right - rcWork.left) / 2);
    height = min_i(height, ((rcWork.bottom - rcWork.top) * 3) / 5);
    _PositionCeDoc(width, height);
    InvalidateRect(s_hwndCeDoc, NULL, TRUE);
}

void CeCompletion_ToggleDocWindow(void)
{
    if (s_hwndCeDoc && IsWindowVisible(s_hwndCeDoc)) {
        _HideCeDoc();
        return;
    }
    char name[128];
    if (!_CurrentKeyword(name, COUNTOF(name))) {
        return;
    }
    _ShowCeDoc(name);
    // Opened while a list is up: the pane follows that list from now on.
    s_ceDocFollowList = SciCall_AutoCActive();
}

void CeCompletion_FollowDocWindow(const char* pszName)
{
    if (!s_hwndCeDoc || !IsWindowVisible(s_hwndCeDoc) || StrIsEmptyA(pszName)) {
        return;
    }
    _ShowCeDoc(pszName);
    s_ceDocFollowList = true;
}

// The completion pane stays a single line: the keyword in the head and its
// signature right after it.  The note is shown in the reference pane (the
// Ctrl+Alt+Shift+H command), never as an extra block under the list.
void CeCompletion_HandleAutoCSelectionChange(const char* pszSelected)
{
    // A late SCN_AUTOCSELECTIONCHANGE can arrive while ac.Cancel() tears the
    // list down; never re-show the pane once the list is gone.
    if (!CeCompletion_IsActive() || !SciCall_AutoCActive() || StrIsEmptyA(pszSelected)) {
        _HideCeTip();
        return;
    }
    StringCchCopyA(s_lastHighlight, COUNTOF(s_lastHighlight), pszSelected);
    const CE_COMPLETION_ENTRY* const pEntry = CeCompletion_Find(pszSelected);
    char head[512];
    char localized[2048];
    const char* pszSyntax = "";
    int kind = CE_KIND_VARIABLE;
    if (pEntry) {
        // The keyword and its signature share the head line (the signature is
        // painted right after the name).
        StringCchPrintfA(head, COUNTOF(head), "%s  \xC2\xB7  %s", pEntry->category, pEntry->name);
        pszSyntax = _LocalizeSignature(pEntry, localized, COUNTOF(localized));
        kind = pEntry->kind;
    }
    else {
        StringCchPrintfA(head, COUNTOF(head), "%s  \xC2\xB7  %s",
                         g_ceKindLabels[CE_KIND_VARIABLE], pszSelected);
        pszSyntax = pszSelected;
    }
    // Names that are only in the document / word lists are painted blue here;
    // red is reserved for the hover path where a name is neither in the CE API
    // catalogue nor a known keyword (see CeLspFeatures_OnHover).
    _ShowCeTipEx(head, pszSyntax, false, "", kind);
    // Keep the reference pane on the item the user is looking at.
    CeCompletion_FollowDocWindow(pszSelected);
}

const char* CeCompletion_CurrentItem(void)
{
    return SciCall_AutoCActive() ? s_lastHighlight : "";
}

void CeCompletion_SetCurrentItem(const char* pszName)
{
    if (pszName && *pszName) {
        StringCchCopyA(s_lastHighlight, COUNTOF(s_lastHighlight), pszName);
    }
    else {
        s_lastHighlight[0] = '\0';
    }
}

void CeCompletion_HandleAutoCCancelled(void)
{
    _HideCeTip();
    s_pendingCompletion = false;
    s_lastHighlight[0] = '\0';
}

void CeCompletion_HandleAutoCSelection(const char* pszSelected, int fillUpChar)
{
    // Hide every kind of completion hint right away: Scintilla's ac.Cancel()
    // does not emit SCN_AUTOCCANCELLED, so waiting for SCN_AUTOCCOMPLETED
    // leaves the detail pane (or a stale calltip) visible.
    _HideCeTip();
    Sci_CallTipCancelEx();
    if (CeCompletion_IsActive() && !StrIsEmptyA(pszSelected)) {
        StringCchCopyA(s_pendingName, COUNTOF(s_pendingName), pszSelected);
        s_pendingFillUp = fillUpChar;
        s_pendingCompletion = true;
    }
    else {
        s_pendingCompletion = false;
    }
}

void CeCompletion_FlushPendingCompletion(void)
{
    if (!s_pendingCompletion || SciCall_AutoCActive()) {
        return;
    }
    s_pendingCompletion = false;
    char name[240];
    StringCchCopyA(name, COUNTOF(name), s_pendingName);
    CeCompletion_HandleAutoCCompleted(name, s_pendingFillUp);
}

void CeCompletion_ShowCurrentItemTip(void)
{
    if (!CeCompletion_IsActive() || !SciCall_AutoCActive()) {
        s_lastHighlight[0] = '\0';
        return;
    }
    char name[240] = { '\0' };
    if (SciCall_AutoCGetCurrentText(name) > 0) {
        CeCompletion_HandleAutoCSelectionChange(name);
    }
    else {
        // A freshly opened list without a current item must not inherit the
        // name highlighted in the previous list (see HandleAutoCCompleted).
        s_lastHighlight[0] = '\0';
    }
}

// Shared post-completion handling: insert parentheses (or an automatically
// resolved argument).  pEntry may be NULL for words that only exist in a
// generated keyword list.  Accepting a list item must leave no tooltip behind,
// so |showTip| is false for that path.
static void _InsertCallCompletion(const char* pszName, const CE_COMPLETION_ENTRY* pEntry,
                                  int fillUpChar, bool showTip)
{
    DocPos const start = SciCall_GetCurrentPos() - (DocPos)strlen(pszName);
    if ((SciCall_GetCharAt(SciCall_GetCurrentPos()) == '(') || (fillUpChar == '(')) {
        if (pEntry && showTip) {
            _ShowCallTip(pEntry, start);
        }
        return; // Scintilla inserts a '(' fill-up itself; never double it
    }

    bool const autoArg = CeSettings_AutoArguments() &&
        (CeSettings_IsAutoArgFunction(pszName) || CeTemplate_IsTrigger(pszName));
    char var[240];
    char arg[260];

    SciCall_BeginUndoAction();
    if (SciCall_GetCharAt(SciCall_GetCurrentPos()) == ')') {
        SciCall_ReplaceSel("("); // user typed the closing bracket already
    }
    else if (autoArg && CeCompletion_ResolveVariable(var, COUNTOF(var))) {
        StringCchPrintfA(arg, COUNTOF(arg), "(%s)", var);
        SciCall_ReplaceSel(arg);
    }
    else {
        SciCall_ReplaceSel("()");
        DocPos const inside = SciCall_GetCurrentPos() - 1;
        SciCall_SetSel(inside, inside);
    }
    SciCall_EndUndoAction();

    if (pEntry && showTip) {
        _ShowCallTip(pEntry, start);
    }
}

// Scintilla has already inserted the trigger word.  Replace exactly that word
// with the rendered skeleton, inside one undo action, and leave the caret on
// the first placeholder (selected, so typing overwrites it).
static bool _TryInsertCodeBlock(const char* name)
{
    const CE_CODE_BLOCK* const block = _FindCodeBlock(name);
    if (!block) {
        return false;
    }
    DocPos const caret = SciCall_GetCurrentPos();
    size_t const nameLen = strlen(name);
    DocPos const wordStart = caret - (DocPos)nameLen;
    if (wordStart < 0) {
        return false;
    }
    const char* const inserted = SciCall_GetRangePointer(wordStart, (DocPos)nameLen);
    if (!inserted || (_strnicmp(inserted, name, nameLen) != 0)) {
        return false; // the list item was not actually inserted here
    }

    DocLn const line = SciCall_LineFromPosition(wordStart);
    DocPos const lineStart = SciCall_PositionFromLine(line);
    char indent[64] = { '\0' };
    size_t indentLen = 0;
    while ((indentLen + 1 < COUNTOF(indent)) && ((lineStart + (DocPos)indentLen) < wordStart)) {
        int const ch = SciCall_GetCharAt(lineStart + (DocPos)indentLen);
        if ((ch != ' ') && (ch != '\t')) {
            break;
        }
        indent[indentLen++] = (char)ch;
    }
    indent[indentLen] = '\0';

    char rendered[1024];
    int selStart = -1;
    int selEnd = -1;
    _RenderCodeBlock(block->body, indent, CeScript_EolStr(), "    ",
                     rendered, COUNTOF(rendered), &selStart, &selEnd);
    SciCall_BeginUndoAction();
    SciCall_SetSel(wordStart, caret);
    SciCall_ReplaceSel(rendered);
    SciCall_EndUndoAction();
    if ((selStart >= 0) && (selEnd >= selStart)) {
        SciCall_SetSel(wordStart + (DocPos)selStart, wordStart + (DocPos)selEnd);
    }
    return true;
}

bool CeCompletion_InsertCodeBlock(const char* pszName)
{
    return _TryInsertCodeBlock(pszName);
}

void CeCompletion_HandleAutoCCompleted(const char* pszSelected, int fillUpChar)
{
    s_pendingCompletion = false;
    _HideCeTip(); // the list is gone; never leave a stale detail pane behind
    if (!CeCompletion_IsActive() || StrIsEmptyA(pszSelected)) {
        s_lastHighlight[0] = '\0';
        return;
    }

    // Scintilla replaced the typed root itself (lenEntered > 0), so the caret
    // is exactly at the end of the inserted word.  If the accepted item does
    // not match what the list had highlighted (arrow-key navigation), correct
    // the inserted word to the highlighted one.
    char nameBuf[240];
    StringCchCopyA(nameBuf, COUNTOF(nameBuf), pszSelected);
    if (s_lastHighlight[0] && (strcmp(s_lastHighlight, pszSelected) != 0)) {
        DocPos const caret = SciCall_GetCurrentPos();
        DocPos const wordStart = caret - (DocPos)strlen(pszSelected);
        if (wordStart >= 0) {
            char const* const p = SciCall_GetRangePointer(wordStart, (DocPos)strlen(pszSelected));
            if (p && (_strnicmp(p, pszSelected, strlen(pszSelected)) == 0)) {
                SciCall_BeginUndoAction();
                SciCall_SetSel(wordStart, caret);
                SciCall_ReplaceSel(s_lastHighlight);
                SciCall_EndUndoAction();
                StringCchCopyA(nameBuf, COUNTOF(nameBuf), s_lastHighlight);
            }
        }
    }
    s_lastHighlight[0] = '\0';
    const char* const name = nameBuf;

    if (_TryInsertCodeBlock(name)) {
        return; // a multi-line skeleton replaced the trigger word
    }

    const CE_COMPLETION_ENTRY* const pEntry = CeCompletion_Find(name);
    bool const isCall = pEntry ? (pEntry->isCall != 0) : _LooksLikeCall(name);
    if (!isCall) {
        // In AA code only alloc-family variables denote a memory reference and
        // get wrapped as "[name]".  Labels, defines, registered symbols and
        // aobscan results are code addresses and stay bare.  An instruction
        // line also qualifies, so fragments without a [ENABLE] marker are
        // handled too.
        char insn[32];
        bool const isAA = _IsAaLineAtCaret() ||
                          _LineStartsWithAsmInsn(SciCall_GetCurrentPos(), insn, COUNTOF(insn));
        if (CeSettings_WrapAsmSymbols() && isAA &&
            !_IsCeBuiltinVariable(name) &&
            !CeCompletion_IsBuiltinKeyword(name) && _IsIdentifier(name) &&
            _IsAllocDocumentSymbol(name)) {
            DocPos const caret = SciCall_GetCurrentPos();
            DocPos const wordStart = caret - (DocPos)strlen(name);
            if ((wordStart > 0) && (SciCall_GetCharAt(wordStart - 1) == '[') &&
                (SciCall_GetCharAt(caret) == ']')) {
                return; // already wrapped
            }
            char wrapped[300];
            StringCchPrintfA(wrapped, COUNTOF(wrapped), "[%s]", name);
            SciCall_BeginUndoAction();
            SciCall_SetSel(wordStart, caret);
            SciCall_ReplaceSel(wrapped);
            SciCall_EndUndoAction();
        }
        return;
    }
    _InsertCallCompletion(name, pEntry, fillUpChar, false);
}

// ----------------------------------------------------------------------------

void CeCompletion_InsertEntry(const CE_COMPLETION_ENTRY* pEntry)
{
    if (!pEntry) {
        return;
    }
    SciCall_ReplaceSel(pEntry->name);
    if (pEntry->isCall) {
        _InsertCallCompletion(pEntry->name, pEntry, 0, true);
    }
}
