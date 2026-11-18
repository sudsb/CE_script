#!/usr/bin/env python3
# encoding: utf-8
"""Generate ce_lsp_server/ce_api_types.h from Cheat Engine's celua.txt.

celua.txt (the CE Lua API reference) is scraped from the cheat-engine repo via
the GitHub contents API (raw domain is blocked on this machine):

  Invoke-RestMethod -Headers @{"Accept"="application/vnd.github.raw+json"} \
    "https://api.github.com/repos/cheat-engine/cheat-engine/contents/Cheat%20Engine/bin/celua.txt?ref=master" \
    -OutFile celua.txt

The script extracts:
  * "X Class:" sections -> type registry (properties + methods, inheritance
    flattened so a derived type already contains its bases' members),
  * global "name(params): ... Returns a <Type> object" lines -> function to
    return-type map (used to infer `local x = getAddressList()` -> Addresslist),
  * hand-curated Lua 5.3 built-in module members (math/string/table/...).

Output is a self-contained header with static const data, included by
ce_lsp_server/CeLspServer.c.  Do not edit the generated header by hand.
"""
import re
import sys
import os

# ---------------------------------------------------------------------------
# Lua 5.3 built-in module members (standard; CE ships Lua 5.3/5.4).
# (name, kind)  kind: 'f' = function, 'v' = variable/constant
# ---------------------------------------------------------------------------
LUA_MODULES = {
    "math": [
        ("abs", "f"), ("acos", "f"), ("asin", "f"), ("atan", "f"), ("atan2", "f"),
        ("ceil", "f"), ("cos", "f"), ("cosh", "f"), ("deg", "f"), ("exp", "f"),
        ("floor", "f"), ("fmod", "f"), ("frexp", "f"), ("huge", "v"), ("ldexp", "f"),
        ("log", "f"), ("log10", "f"), ("max", "f"), ("maxinteger", "v"), ("min", "f"),
        ("mininteger", "v"), ("modf", "f"), ("pi", "v"), ("rad", "f"), ("random", "f"),
        ("randomseed", "f"), ("sin", "f"), ("sinh", "f"), ("sqrt", "f"), ("tan", "f"),
        ("tanh", "f"), ("tointeger", "f"), ("type", "f"), ("ult", "f"),
    ],
    "string": [
        ("byte", "f"), ("char", "f"), ("dump", "f"), ("find", "f"), ("format", "f"),
        ("gmatch", "f"), ("gsub", "f"), ("len", "f"), ("lower", "f"), ("match", "f"),
        ("pack", "f"), ("packsize", "f"), ("rep", "f"), ("reverse", "f"), ("sub", "f"),
        ("unpack", "f"), ("upper", "f"),
    ],
    "table": [
        ("concat", "f"), ("create", "f"), ("clear", "f"), ("getn", "f"), ("insert", "f"),
        ("move", "f"), ("pack", "f"), ("remove", "f"), ("sort", "f"), ("unpack", "f"),
    ],
    "os": [
        ("clock", "f"), ("date", "f"), ("difftime", "f"), ("execute", "f"), ("exit", "f"),
        ("getenv", "f"), ("remove", "f"), ("rename", "f"), ("setlocale", "f"), ("time", "f"),
        ("tmpname", "f"),
    ],
    "io": [
        ("close", "f"), ("flush", "f"), ("input", "f"), ("lines", "f"), ("open", "f"),
        ("output", "f"), ("popen", "f"), ("read", "f"), ("stderr", "v"), ("stdin", "v"),
        ("stdout", "v"), ("tmpfile", "f"), ("type", "f"), ("write", "f"),
    ],
    "utf8": [
        ("char", "f"), ("charpattern", "v"), ("codepoint", "f"), ("codes", "f"),
        ("len", "f"), ("offset", "f"),
    ],
    "coroutine": [
        ("create", "f"), ("resume", "f"), ("running", "f"), ("status", "f"),
        ("wrap", "f"), ("yield", "f"),
    ],
    "debug": [
        ("debug", "f"), ("gethook", "f"), ("getinfo", "f"), ("getlocal", "f"),
        ("getmetatable", "f"), ("getregistry", "f"), ("getupvalue", "f"),
        ("getuservalue", "f"), ("setcstacklimit", "f"), ("sethook", "f"),
        ("setlocal", "f"), ("setmetatable", "f"), ("setupvalue", "f"),
        ("setuservalue", "f"), ("traceback", "f"), ("upvalueid", "f"), ("upvaluejoin", "f"),
    ],
    "package": [
        ("config", "v"), ("cpath", "v"), ("loaded", "v"), ("loaders", "v"),
        ("path", "v"), ("preload", "v"), ("searchers", "v"),
    ],
}


# Phrase -> CE type, for return descriptions that do not spell the class name
# the way it is capitalised in the class registry.
ALIASES = {
    "memory scan": "MemScan",
    "memoryrecord": "MemoryRecord",
    "memrec": "MemoryRecord",
    "address list": "Addresslist",
    "cheat table addresslist": "Addresslist",
}


def _scan_fn_return(line, fn_ret, known_types, types):
    """Map `name(params): <desc>` to a return type when <desc> names a known
    CE object/primitive.  Among every known type name appearing in the
    description we keep the *most derived* one (drop any type that is an
    ancestor of another match).  'Object' is the universal base and shows up
    in every 'class object' phrase, so it is only used when nothing else
    matches."""
    m = re.match(r"^(\s*)([A-Za-z_]\w*)\s*\(([^)]*)\)\s*:\s*(.*)$", line,
                 re.IGNORECASE)
    if not m:
        return
    fn = m.group(2)
    desc = m.group(4).lower()

    def is_ancestor(a, b):
        # true if a is anywhere in b's ancestor chain
        chain = types.get(b, {}).get("ancestors", [])
        seen = set()
        stack = list(chain)
        while stack:
            x = stack.pop()
            if x in seen:
                continue
            seen.add(x)
            if x == a:
                return True
            stack.extend(types.get(x, {}).get("ancestors", []))
        return False

    def depth(t, _seen=None):
        _seen = _seen or set()
        if t in _seen:
            return 0
        _seen.add(t)
        chain = types.get(t, {}).get("ancestors", [])
        if not chain:
            return 1
        return 1 + max((depth(a, set(_seen)) for a in chain), default=0)

    cands = [t for t in known_types if t.lower() in desc]
    cands = [t for t in cands if t != "Object"]
    if cands:
        # keep only most-derived (not an ancestor of another candidate)
        cands = [t for t in cands
                 if not any(is_ancestor(t, u) for u in cands if u != t)]
        if cands:
            # tie-break among unrelated candidates by inheritance depth
            fn_ret[fn] = max(cands, key=depth)
            return
    if "Object".lower() in desc:
        fn_ret[fn] = "Object"
        return
    for phrase, t in ALIASES.items():
        if phrase in desc:
            fn_ret[fn] = t
            return
    if "table" in desc:
        fn_ret[fn] = "table"


def parse_celua(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")

    class_re = re.compile(
        r"^([A-Za-z_]\w*)\s+class\s*:?\s*"
        r"(\((?:Inheritance\s*:)?\s*(.*?)\))?\s*$",
        re.IGNORECASE,
    )
    prop_sec_re = re.compile(r"^\s*properties\s*$", re.IGNORECASE)
    meth_sec_re = re.compile(r"^\s*methods\s*$", re.IGNORECASE)
    # property getter like "  MouseHighlightedRecord() : Returns ..."
    prop_getter_re = re.compile(r"^\s{2,}([A-Za-z_]\w*)\s*\(\s*\)\s*:\s")
    # method like "  getCount()" or "  getMemoryRecord(index)"
    method_re = re.compile(r"^\s{2,}([A-Za-z_]\w*)\s*\(([^)]*)\)\s*$")
    method_re2 = re.compile(r"^\s{2,}([A-Za-z_]\w*)\s*\(([^)]*)\)\s*:")
    # property like "  Name: Type - desc" or "  Name : Type"
    prop_re = re.compile(r"^\s{2,}([A-Za-z_]\w*)\s*:\s*(.*)$")

    types = {}          # name -> {"ancestors": [...], "members": [(name,kind,type)]}
    order = []
    fn_ret = {}         # function name -> return type (object type name or primitive)
    cur = None
    section = None

    # Pass 1: collect class definitions and their members.
    for raw in lines:
        m = class_re.match(raw)
        if m:
            name = m.group(1)
            inh = (m.group(3) or "").strip()
            anc = [a.strip() for a in inh.split("->") if a.strip()] if inh else []
            if name not in types:
                types[name] = {"ancestors": anc, "members": []}
                order.append(name)
            else:
                # keep first-seen ancestors if re-declared
                if not types[name]["ancestors"] and anc:
                    types[name]["ancestors"] = anc
            cur = name
            section = None
            continue

        if prop_sec_re.match(raw):
            section = "prop"
            continue
        if meth_sec_re.match(raw):
            section = "meth"
            continue

        if cur is None:
            # global function/description lines before the first class: skip
            # here; return types are resolved in a second pass below.
            continue

        if section == "prop":
            mg = prop_getter_re.match(raw)
            if mg:
                types[cur]["members"].append((mg.group(1), "v", None))
                continue
            mp = prop_re.match(raw)
            if mp:
                nm = mp.group(1)
                rest = mp.group(2)
                # type hint = text before " - " or end
                th = rest.split(" - ", 1)[0].strip() if rest else ""
                types[cur]["members"].append((nm, "v", th if th else None))
                continue
        elif section == "meth":
            mm = method_re.match(raw) or method_re2.match(raw)
            if mm:
                types[cur]["members"].append((mm.group(1), "f", None))
                continue

        # Even outside explicit blocks, getters/methods are already handled
        # above; no separate fn-return scan here.

    # Pass 2: resolve function -> return type from descriptions.
    known_types = set(types.keys())
    for raw in lines:
        _scan_fn_return(raw, fn_ret, known_types, types)

    return types, order, fn_ret


def flatten(types, order):
    """Return ordered list of (name, ancestors, merged_members).
    merged_members = own members then ancestor members not already present
    (ancestors expanded recursively, base-first)."""
    by_name = {n: types[n] for n in order}

    def expand(name, seen, out):
        if name in seen or name not in by_name:
            return
        seen.add(name)
        for a in by_name[name]["ancestors"]:
            expand(a, seen, out)
        for mem in by_name[name]["members"]:
            out.append(mem)

    result = []
    for n in order:
        seen = set()
        merged = []
        expand(n, seen, merged)
        # dedup by name keeping first (base) occurrence
        dedup = {}
        final = []
        for mem in merged:
            if mem[0] not in dedup:
                dedup[mem[0]] = True
                final.append(mem)
        result.append((n, by_name[n]["ancestors"], final))
    return result


def c_escape(s):
    return (s or "").replace("\\", "\\\\").replace('"', '\\"')


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else None
    if not src or not os.path.exists(src):
        # default: sibling temp location
        cand = os.path.join(os.environ.get("TEMP", "."), "celua.txt")
        if os.path.exists(cand):
            src = cand
        else:
            sys.stderr.write("usage: gen_ce_api_types.py <celua.txt>\n")
            sys.exit(2)

    types, order, fn_ret = parse_celua(src)
    flattened = flatten(types, order)

    # merge Lua built-in modules as pseudo-types
    for mod, members in LUA_MODULES.items():
        flattened.append((mod, [], [(nm, kd, None) for nm, kd in members]))

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "ce_lsp_server", "ce_api_types.h")
    out_path = os.path.normpath(out_path)

    L = []
    L.append("// Generated by tools/gen_ce_api_types.py from celua.txt - do not edit by hand.")
    L.append("#pragma once")
    L.append("")
    L.append('#include "../src/StyleLexers/ce_completions_data.h"  // CE_KIND_*')
    L.append("")
    L.append("typedef struct CE_API_MEMBER {")
    L.append("    const char* name;")
    L.append("    int         kind;     // CE_KIND_FUNCTION / CE_KIND_VARIABLE")
    L.append("    const char* type;     // type hint string or NULL")
    L.append("} CE_API_MEMBER;")
    L.append("")
    L.append("typedef struct CE_API_TYPE {")
    L.append("    const char*        name;")
    L.append("    const CE_API_MEMBER* members;")
    L.append("    int               memberCount;")
    L.append("} CE_API_TYPE;")
    L.append("")
    L.append("// case-insensitive lookup of a registered type")
    L.append("const CE_API_TYPE* CeApi_FindType(const char* name);")
    L.append("// function name -> return type (object type name or primitive), or NULL")
    L.append("const char* CeApi_FunctionReturnType(const char* fnName);")
    L.append("// known built-in module name (math/string/...) -> its members, or NULL")
    L.append("const CE_API_TYPE* CeApi_FindModule(const char* name);")
    L.append("")

    # Emit member arrays first.
    mem_var_names = []
    for (tname, anc, members) in flattened:
        if not members:
            continue
        arr = "g_ceApiMembers_" + _ident(tname)
        mem_var_names.append((tname, arr, len(members)))
        L.append("static const CE_API_MEMBER %s[] = {" % arr)
        for (nm, kd, tp) in sorted(members, key=lambda x: x[0].lower()):
            kind = "CE_KIND_FUNCTION" if kd == "f" else "CE_KIND_VARIABLE"
            tp_lit = '\"%s\"' % c_escape(tp) if tp else "NULL"
            L.append('    { "%s", %s, %s },' % (c_escape(nm), kind, tp_lit))
        L.append("};")
        L.append("")

    L.append("static const CE_API_TYPE g_ceApiTypes[] = {")
    for (tname, arr, cnt) in mem_var_names:
        L.append('    { "%s", %s, %d },' % (c_escape(tname), arr, cnt))
    L.append("    { NULL, NULL, 0 }")
    L.append("};")
    L.append("")

    # function -> return type map
    L.append("static const struct { const char* fn; const char* type; } g_ceApiFnRet[] = {")
    for fn in sorted(fn_ret.keys()):
        L.append('    { "%s", "%s" },' % (c_escape(fn), c_escape(fn_ret[fn])))
    L.append("    { NULL, NULL }")
    L.append("};")
    L.append("")

    # lookup helpers
    L.append("static int _CeApiStriCmp(const char* a, const char* b) {")
    L.append("    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);")
    L.append("    for (; *a && *b; ++a, ++b) {")
    L.append("        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;")
    L.append("        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;")
    L.append("        if (ca != cb) return (ca > cb) ? 1 : -1;")
    L.append("    }")
    L.append("    return (*a == 0 && *b == 0) ? 0 : (*a ? 1 : -1);")
    L.append("}")
    L.append("")
    L.append("const CE_API_TYPE* CeApi_FindType(const char* name) {")
    L.append("    for (int i = 0; g_ceApiTypes[i].name; ++i) {")
    L.append("        if (_CeApiStriCmp(g_ceApiTypes[i].name, name) == 0)")
    L.append("            return &g_ceApiTypes[i];")
    L.append("    }")
    L.append("    return NULL;")
    L.append("}")
    L.append("")
    L.append("const CE_API_TYPE* CeApi_FindModule(const char* name) {")
    L.append("    return CeApi_FindType(name);")
    L.append("}")
    L.append("")
    L.append("const char* CeApi_FunctionReturnType(const char* fnName) {")
    L.append("    for (int i = 0; g_ceApiFnRet[i].fn; ++i) {")
    L.append("        if (_CeApiStriCmp(g_ceApiFnRet[i].fn, fnName) == 0)")
    L.append("            return g_ceApiFnRet[i].type;")
    L.append("    }")
    L.append("    return NULL;")
    L.append("}")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")

    sys.stderr.write("wrote %s : %d types, %d fn-return entries\n"
                     % (out_path, len(mem_var_names), len(fn_ret)))


def _ident(name):
    out = []
    for ch in name:
        out.append(ch if (ch.isalnum() or ch == "_") else "_")
    return "".join(out)


if __name__ == "__main__":
    main()
