"""Tokenizer and bracket linter for Cheat Engine Lua / Auto Assembler."""

from __future__ import annotations

import re
from bisect import bisect_right
from dataclasses import dataclass

from .completions import api_names

API_NAMES = api_names()

LUA_KW_CTRL = frozenset(
    "if then else elseif end for while do repeat until break goto return in".split()
)
LUA_KW_DECL = frozenset({"local", "function"})
LUA_KW_OP = frozenset("and or not".split())
LUA_CONST = frozenset({"true", "false", "nil"})
LUA_LIBS = frozenset(
    "string table math os io bit bit32 coroutine debug utf8 package".split()
)
CE_GLOBALS = frozenset(
    "syntaxcheck memrec process TrainerOrigin MainForm getLuaEngine _G "
    "_VERSION targetIs64Bit AddressList MemoryRecord".split()
)

AA_CMDS = frozenset(
    (
        "alloc globalalloc sharedalloc dealloc label registersymbol "
        "unregistersymbol define aobscan aobscanmodule aobscanregion "
        "aobscanpattern fullaccess assert createthread createthreadandwait "
        "loadlibrary include luacall readmem loadbinary struct endstruct "
        "kalloc reassemble savenop exports unprotect "
        "createthreadandwaitfinish"
    ).split()
)
ASM_DIR = frozenset(
    "db dw dd dq dt resb resw resd resq align times equ".split()
)
ASM_SIZE = frozenset(
    "byte word dword qword tbyte tword fword oword xmmword ymmword zmmword "
    "ptr short near far float double real4 real8 real10".split()
)

# Notepad3 manual-watch comment directives (// np3watch ...).  The editor's
# lexer colours the tokens after the directive, and the CE bridge parses the
# same line, so both sides share this vocabulary.
WATCH_DIRECTIVES = frozenset({"np3watch", "np3unwatch", "np3nowatch"})
WATCH_TYPES = frozenset(
    "byte word dword qword pointer float double string wstring array reg "
    "u8 u16 u32 u64 i8 i16 i32 i64 ptr f32 f64 str wstr arr auto".split()
)
WATCH_FORMATS = frozenset(
    "bin dec hex binary decimal hexadecimal on off".split()
)
# "if" introduces a condition; "begin"/"end" delimit a watch block whose
# condition is inherited by the lines inside it.
WATCH_BLOCK = frozenset({"if", "begin", "end"})
# CPU registers accepted as watch names / condition operands (CeBridge.c).
WATCH_REGISTERS = frozenset(
    "rax rbx rcx rdx rsi rdi rbp rsp rip r8 r9 r10 r11 r12 r13 r14 r15 "
    "eflags flags eax ebx ecx edx esi edi ebp esp eip "
    "ax bx cx dx si di bp sp al bl cl dl ah bh ch dh sil dil bpl spl".split()
) | {f"r{n}{w}" for n in range(8, 16) for w in "db"}

# All x86 condition codes; combined below into jcc / setcc / cmovcc.
_CC = (
    "a ae b be c e g ge l le na nae nb nbe nc ne ng nge nl nle no np ns nz "
    "o p pe po s z"
).split()

ASM_JMP = frozenset(
    (
        "jmp call ret retn retf iret iretd iretq leave loop loopne loope "
        "loopnz loopz jecxz jrcxz syscall sysret sysenter sysexit int int3 "
        "into ud2 endbr32 endbr64"
    ).split()
    + ["j" + cc for cc in _CC]
)

ASM_MNE = frozenset(
    (
        "mov movzx movsx movsxd movabs movbe movnti movntdq movntps movntpd "
        "lea xchg xadd cmpxchg cmpxchg8b cmpxchg16b push pop pushad popad "
        "pushfd popfd pushfq popfq pusha popa add adc adcx adox sub sbb inc "
        "dec neg imul mul idiv div and andn or xor not shl shr sal sar shld "
        "shrd shlx shrx sarx rol ror rorx rcl rcr bt bts btr btc bsf bsr "
        "bswap bextr blsi blsmsk blsr bzhi pdep pext mulx popcnt lzcnt "
        "tzcnt crc32 cmp test cdq cqo cwde cbw cwd clc stc cmc cld std cli "
        "sti hlt pause nop wait fwait rdtsc rdtscp cpuid rdrand rdseed "
        "xgetbv xsetbv enter leave in out insb insw insd outsb outsw outsd "
        "stosb stosw stosd stosq movsb movsw movsd movsq lodsb lodsw lodsd "
        "lodsq scasb scasw scasd scasq cmpsb cmpsw cmpsd cmpsq xlat xlatb "
        "rep repe repz repne repnz lock xacquire xrelease xbegin xend "
        "xabort xtest mfence lfence sfence clflush clflushopt prefetchnta "
        "prefetcht0 prefetcht1 prefetcht2 prefetchw wbinvd invd "
        "movss movsd movaps movapd movups movupd movdqa movdqu movd movq "
        "addss addsd addps addpd subss subsd subps subpd mulss mulsd mulps "
        "mulpd divss divsd divps divpd sqrtss sqrtsd sqrtps sqrtpd rsqrtss "
        "rsqrtps rcpps rcpss maxss maxsd maxps maxpd minss minsd minps "
        "minpd comiss comisd ucomiss ucomisd cvtsi2ss cvtsi2sd cvtss2si "
        "cvtsd2si cvttss2si cvttsd2si cvtss2sd cvtsd2ss cvtdq2ps cvtps2dq "
        "cvtpd2dq cvtdq2pd cvtps2pd cvtpd2ps xorps xorpd andps andpd orps "
        "orpd andnps andnpd pxor por pand pandn paddd psubd paddb paddw "
        "paddq psubb psubw psubq pmullw pmulld pmuludq pmaxsd pminsd "
        "punpcklbw punpcklwd punpckldq punpcklqdq punpckhqdq packsswb "
        "packssdw pshufd psrldq pslldq pcmpeqb pcmpeqd unpcklps shufps "
        "vmovaps vmovapd vmovups vmovupd vmovdqa vmovdqu vmovss vmovsd "
        "vaddps vaddpd vaddss vaddsd vsubps vsubpd vsubss vsubsd vmulps "
        "vmulpd vmulss vmulsd vdivps vdivpd vdivss vdivsd vsqrtps vsqrtpd "
        "vxorps vxorpd vandps vandpd vorps vorpd vpaddd vpsubd vpxor "
        "vzeroupper vzeroall vbroadcastss vbroadcastsd vinsertps vpextrd "
        "aesenc aesenclast aesdec aesdeclast sha1rnds4 "
        "fld fst fstp fadd faddp fsub fsubp fsubr fsubrp fmul fmulp fdiv "
        "fdivp fdivr fdivrp fild fistp fchs fabs fsqrt fcom fcomp fcompp "
        "fucom fucomp fucompp frndint fprem fprem1 fptan fpatan fsin fcos "
        "fsincos f2xm1 fyl2x fyl2xp1 fld1 fldl2t fldl2e fldpi fldlg2 "
        "fldln2 fldz fnop fnclex fclex finit fninit fnstcw fldcw fnstsw "
        "fnstenv fldenv fsave frstor fxch ffree fincstp fdecstp emms"
    ).split()
    + ["set" + cc for cc in _CC]
    + ["cmov" + cc for cc in _CC]
)
ASM_REG = re.compile(
    r"(?:r[abcd]x|r[sd]i|r[sb]p|r(?:8|9|1[0-5])[dwb]?|e[abcd]x|e[sd]i|"
    r"e[sb]p|eip|[abcd][xlh]|[sd]il?|[sb]pl?|[xyz]mm\d{1,2}|mm[0-7]|"
    r"st\d|cr\d|dr\d|[cdefgs]s|[re]?flags)",
    re.IGNORECASE,
)
ASM_CC = re.compile(r"(?:j[a-z]{1,3}|set[a-z]{1,4}|cmov[a-z]{1,4})")
C_KW = frozenset(
    (
        "if else for while do switch case default break continue return "
        "goto sizeof typedef struct union enum static const volatile extern "
        "register inline auto asm"
    ).split()
)
C_TYPE = frozenset(
    (
        "void char short int long float double signed unsigned bool size_t "
        "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
        "DWORD WORD BYTE QWORD LPVOID HANDLE"
    ).split()
)

OP2 = frozenset(("==", "~=", "<=", ">=", "..", "//", "<<", ">>"))
OP1 = frozenset("+-*/%^#<>=&|~")
PUNCT = frozenset(",;:.")

LUA_NUM = re.compile(
    r"(?:0[xX][0-9a-fA-F]*(?:\.[0-9a-fA-F]*)?(?:[pP][-+]?\d+)?|"
    r"(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)"
)
ESCAPE = re.compile(r"\\(?:x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]+\}|z|\d{1,3}|[\s\S])")
LONG_OPEN = re.compile(r"\[(=*)\[")
LONG_COMMENT = re.compile(r"--\[(=*)\[")
DIRECTIVE = re.compile(r"\{\$[A-Za-z_]\w*[^}\n]*\}")
ENABLE_MARK = re.compile(r"\[(?:ENABLE|DISABLE)\]", re.IGNORECASE)
LABEL = re.compile(r"::\s*[A-Za-z_]\w*\s*::")
WILD = re.compile(r"\?+")
MODULE = re.compile(
    r"[A-Za-z_][\w\-]*\.(?:exe|dll|sys|bin|ocx|node|so)\b",
    re.IGNORECASE,
)
AA_NUM = re.compile(
    r"(?:\$[0-9A-Fa-f]+|#-?\d+(?:\.\d+)?|0[xX][0-9A-Fa-f]+|"
    r"[0-9][0-9A-Fa-f]*(?:\.\d+)?)"
)
AA_WORD = re.compile(r"[A-Za-z_][\w@]*")
LUA_WORD = re.compile(r"[A-Za-z_]\w*")
C_WORD = re.compile(r"[A-Za-z_]\w*")
HEX_WORD = re.compile(r"[0-9A-Fa-f]+")
VT_NAME = re.compile(r"(?:vt|mt|mb|mr|cb|al|hk)[A-Z]\w*")
CLOSE_TO_OPEN = {")": "(", "]": "[", "}": "{"}
OPEN_TO_CLOSE = {"(": ")", "[": "]", "{": "}"}
AA_LINE_HINT = re.compile(
    r"^[ \t]*(?:mov|jmp|call|push|pop|lea|cmp|add|sub|nop|db|jne|je|"
    r"jle|jge|xor|test|shr|shl|inc|dec|ret)\b",
    re.IGNORECASE | re.MULTILINE,
)
AA_CALL_HINT = re.compile(
    r"^[ \t]*(?:alloc|globalalloc|label|registersymbol|aobscan\w*|define|"
    r"dealloc|fullaccess|assert)\s*\(",
    re.IGNORECASE | re.MULTILINE,
)
AA_MARK_HINT = re.compile(r"\[\s*(?:ENABLE|DISABLE)\s*\]", re.IGNORECASE)


@dataclass(slots=True)
class Token:
    """A half-open source range with an optional syntax kind."""

    start: int
    end: int
    kind: str | None = None


@dataclass(slots=True)
class LexResult:
    """Everything the editor needs from one tokenizer pass."""

    tokens: list[Token]
    line_modes: list[str]
    pairs: dict[int, int]
    ok: bool
    message: str
    line_starts: list[int]


def compute_line_starts(src: str) -> list[int]:
    """Return the character offset of every line start."""
    starts = [0]
    index = src.find("\n")
    while index != -1:
        starts.append(index + 1)
        index = src.find("\n", index + 1)
    return starts


def detect_mode(src: str) -> str:
    """Guess whether a script is primarily Lua, AA, or C."""
    for line in src.splitlines()[:80]:
        text = line.strip()
        if not text or text.startswith("--"):
            continue
        if text.startswith("//"):
            return "aa"
        if re.fullmatch(r"\[(?:ENABLE|DISABLE)\]", text, re.IGNORECASE):
            return "aa"
        if re.match(r"\{\$(?:lua|asm|c|ccode|code)", text, re.IGNORECASE):
            return "aa"
        if re.match(
            r"(?:aobscan\w*|globalalloc|alloc|define|label|"
            r"registersymbol|assert|fullaccess|include)\s*\(",
            text,
            re.IGNORECASE,
        ):
            return "aa"
        return "lua"
    return "lua"


def looks_aa(body: str) -> bool:
    """Return whether an embedded long string looks like AA code."""
    return bool(
        AA_MARK_HINT.search(body)
        or AA_CALL_HINT.search(body)
        or AA_LINE_HINT.search(body)
    )


def is_in_string_or_comment(tokens: list[Token], offset: int) -> bool:
    """Return whether ``offset`` falls inside a string or comment token."""
    low, high = 0, len(tokens) - 1
    while low <= high:
        middle = (low + high) // 2
        token = tokens[middle]
        if offset < token.start:
            high = middle - 1
        elif offset >= token.end:
            low = middle + 1
        else:
            return token.kind in {"str", "strd", "com", "comdoc", "esc"}
    return False


def mode_at_offset(result: LexResult, offset: int) -> str:
    """Return the language mode of the line containing ``offset``."""
    line = bisect_right(result.line_starts, offset) - 1
    line = max(0, line)
    if line < len(result.line_modes):
        return result.line_modes[line]
    return result.line_modes[-1] if result.line_modes else "lua"


class _Lexer:
    def __init__(self, src: str) -> None:
        self.src = src
        self.n = len(src)
        self.i = 0
        self.line = 0
        self.mode = detect_mode(src)
        self.line_modes = [self.mode]
        self.tokens: list[Token] = []
        self.pairs: dict[int, int] = {}
        self.embeds: list[tuple[int, str]] = []
        self.hist: list[tuple[str, str]] = []
        self.br_stack: list[tuple[str, int, int]] = []
        self.col_first = True
        self.mne_seen = False
        self.lint_ok = True
        self.lint_msg = ""

    def run(self) -> LexResult:
        while self.i < self.n:
            if self.embeds and self.i >= self.embeds[-1][0]:
                self._close_embed()
                continue
            char = self.src[self.i]
            if char == "\n":
                self._newline()
                continue
            if char in " \t\r":
                self.i += 1
                continue
            before = self.i
            if self.mode == "lua":
                self._scan_lua()
            elif self.mode == "aa":
                self._scan_aa()
            else:
                self._scan_c()
            if self.i <= before:
                self.i = before + 1
        self._finish_brackets()
        return LexResult(
            tokens=self.tokens,
            line_modes=self.line_modes,
            pairs=self.pairs,
            ok=self.lint_ok,
            message=self.lint_msg,
            line_starts=compute_line_starts(self.src),
        )

    def _push(self, start: int, end: int, kind: str | None = None) -> int:
        if end <= start:
            return -1
        index = len(self.tokens)
        self.tokens.append(Token(start, end, kind))
        if kind is not None:
            self.hist.append((kind, self.src[start:end]))
            if len(self.hist) > 4:
                self.hist.pop(0)
            self.col_first = False
        return index

    def _prev(self, depth: int = 1) -> tuple[str, str] | None:
        if len(self.hist) < depth:
            return None
        return self.hist[-depth]

    def _ensure_line(self) -> None:
        while len(self.line_modes) <= self.line:
            self.line_modes.append(self.mode)

    def _set_mode(self, mode: str) -> None:
        self.mode = mode
        self._ensure_line()
        self.line_modes[self.line] = mode

    def _newline(self) -> None:
        self.i += 1
        self.line += 1
        self._ensure_line()
        self.line_modes[self.line] = self.mode
        self.col_first = True
        self.mne_seen = False
        self.hist.clear()

    def _nl_scan(self, start: int, end: int) -> None:
        position = self.src.find("\n", start, end)
        while position != -1:
            self.line += 1
            self._ensure_line()
            self.line_modes[self.line] = self.mode
            position = self.src.find("\n", position + 1, end)

    def _eol(self, position: int) -> int:
        found = self.src.find("\n", position)
        return self.n if found == -1 else found

    def _next_char(self, position: int) -> str:
        while position < self.n and self.src[position] in " \t":
            position += 1
        return self.src[position] if position < self.n else ""

    def _close_embed(self) -> None:
        _, close = self.embeds.pop()
        self._set_mode("lua")
        if self.src.startswith(close, self.i):
            self._push(self.i, self.i + len(close), "strd")
            self.i += len(close)

    def _bracket(self, char: str) -> None:
        if char in OPEN_TO_CLOSE:
            depth = len(self.br_stack) % 3
            kind = f"br{depth}" if self.mode == "lua" else "aabr"
            index = self._push(self.i, self.i + 1, kind)
            self.br_stack.append((char, index, self.i))
            self.i += 1
            return
        wanted = CLOSE_TO_OPEN[char]
        top = self.br_stack[-1] if self.br_stack else None
        if top is not None and top[0] == wanted:
            self.br_stack.pop()
            depth = len(self.br_stack) % 3
            kind = f"br{depth}" if self.mode == "lua" else "aabr"
            self._push(self.i, self.i + 1, kind)
            self.pairs[top[2]] = self.i
            self.pairs[self.i] = top[2]
        else:
            self._push(self.i, self.i + 1, "brerr")
            if self.lint_ok:
                self.lint_ok = False
                self.lint_msg = f'括号不匹配：多余的 "{char}"'
        self.i += 1

    def _finish_brackets(self) -> None:
        for _, index, _ in self.br_stack:
            if index >= 0:
                self.tokens[index].kind = "brerr"
        if self.lint_ok and self.br_stack:
            self.lint_ok = False
            expected = OPEN_TO_CLOSE[self.br_stack[-1][0]]
            self.lint_msg = f'括号未闭合：缺少 "{expected}"'

    def _scan_quoted(self, quote: str) -> None:
        start = self.i
        position = self.i + 1
        parts: list[tuple[int, int]] = []
        while position < self.n:
            char = self.src[position]
            if char == "\\":
                match = ESCAPE.match(self.src, position)
                length = match.end() - position if match else min(
                    2, self.n - position
                )
                parts.append((position, position + length))
                position += length
                continue
            if char == quote:
                position += 1
                break
            if char == "\n":
                break
            position += 1
        cursor = start
        for part_start, part_end in parts:
            self._push(cursor, part_start, "str")
            self._push(part_start, part_end, "esc")
            cursor = part_end
        self._push(cursor, position, "str")
        self._nl_scan(start, position)
        self.i = position

    def _scan_lua(self) -> None:
        char = self.src[self.i]
        two = self.src[self.i : self.i + 2]

        if self.col_first:
            match = ENABLE_MARK.match(self.src, self.i)
            if match:
                rest = self.src[match.end() : self._eol(self.i)]
                tail = rest.lstrip()
                if not tail or tail.startswith(("--", "//")):
                    self._push(self.i, match.end(), "aamark")
                    self.i = match.end()
                    self._set_mode("aa")
                    return

        match = DIRECTIVE.match(self.src, self.i)
        if match:
            directive = match.group(0).casefold()
            if directive.startswith("{$asm"):
                self._set_mode("aa")
            elif directive.startswith("{$c"):
                self._set_mode("c")
            elif directive.startswith("{$lua"):
                self._set_mode("lua")
            self._push(self.i, match.end(), "aadir")
            self.i = match.end()
            self.line_modes[self.line] = self.mode
            return

        if two == "--":
            match = LONG_COMMENT.match(self.src, self.i)
            if match:
                close = "]" + match.group(1) + "]"
                found = self.src.find(close, match.end())
                end = self.n if found == -1 else found + len(close)
                self._push(self.i, end, "com")
                self._nl_scan(self.i, end)
                self.i = end
                return
            end = self._eol(self.i)
            is_doc = self.src.startswith("---", self.i) and not self.src.startswith(
                "----", self.i
            )
            self._push(self.i, end, "comdoc" if is_doc else "com")
            self.i = end
            return

        match = LONG_OPEN.match(self.src, self.i)
        if match:
            close = "]" + match.group(1) + "]"
            body_start = match.end()
            found = self.src.find(close, body_start)
            body_end = self.n if found == -1 else found
            if body_end > body_start and looks_aa(
                self.src[body_start:body_end]
            ):
                self._push(self.i, body_start, "strd")
                self._nl_scan(self.i, body_start)
                self.i = body_start
                self.embeds.append((body_end, close))
                self.mode = "aa"
                self.mne_seen = False
                return
            end = self.n if found == -1 else found + len(close)
            self._push(self.i, end, "str")
            self._nl_scan(self.i, end)
            self.i = end
            return

        if char in "\"'":
            self._scan_quoted(char)
            return

        if two == "::":
            match = LABEL.match(self.src, self.i)
            if match:
                self._push(self.i, match.end(), "lbl")
                self.i = match.end()
                return

        if char.isdigit() or (
            char == "." and self.src[self.i + 1 : self.i + 2].isdigit()
        ):
            match = LUA_NUM.match(self.src, self.i)
            if match:
                self._push(self.i, match.end(), "num")
                self.i = match.end()
                return

        if char.isalpha() or char == "_":
            match = LUA_WORD.match(self.src, self.i)
            if match:
                self._scan_lua_word(match.group(0), match.end())
                return

        if char in "([{)]}":
            self._bracket(char)
            return
        if self.src.startswith("...", self.i):
            self._push(self.i, self.i + 3, "vararg")
            self.i += 3
            return
        if two in OP2:
            self._push(self.i, self.i + 2, "op")
            self.i += 2
            return
        if char in OP1:
            self._push(self.i, self.i + 1, "op")
            self.i += 1
            return
        if char in PUNCT:
            self._push(self.i, self.i + 1, "punct")
            self.i += 1
            return
        self.i += 1

    def _scan_lua_word(self, word: str, end: int) -> None:
        next_char = self._next_char(end)
        first = self._prev(1)
        second = self._prev(2)
        kind: str | None = None
        dotted = bool(first and first[0] == "punct" and first[1] == ".")
        coloned = bool(first and first[0] == "punct" and first[1] == ":")
        if dotted:
            owner = second[1] if second else ""
            kind = (
                "builtin"
                if f"{owner}.{word}" in API_NAMES
                else "call" if next_char == "(" else "prop"
            )
        elif coloned:
            kind = "call" if next_char == "(" else "prop"
            if next_char == "(":
                kind = "method"
        elif word in LUA_KW_CTRL:
            kind = "kw"
        elif word in LUA_KW_DECL:
            kind = "kwd"
        elif word in LUA_KW_OP:
            kind = "kwop"
        elif word in LUA_CONST:
            kind = "const"
        elif word == "self":
            kind = "self"
        elif first and first[0] == "kwd" and first[1] == "function":
            kind = "fn"
        elif word in LUA_LIBS and next_char == ".":
            kind = "lib"
        elif word in API_NAMES:
            kind = "builtin"
        elif word in CE_GLOBALS or VT_NAME.fullmatch(word):
            kind = "const"
        elif next_char in "(\"'{" or self.src.startswith("[[", end):
            kind = "call"
        self._push(self.i, end, kind)
        self.i = end

    def _scan_aa(self) -> None:
        char = self.src[self.i]
        two = self.src[self.i : self.i + 2]

        if self.col_first:
            match = ENABLE_MARK.match(self.src, self.i)
            if match:
                self._push(self.i, match.end(), "aamark")
                self.i = match.end()
                return

        match = DIRECTIVE.match(self.src, self.i)
        if match:
            directive = match.group(0).casefold()
            if directive.startswith("{$lua"):
                self._set_mode("lua")
            elif directive.startswith("{$asm"):
                self._set_mode("aa")
            elif directive.startswith("{$c"):
                self._set_mode("c")
            self._push(self.i, match.end(), "aadir")
            self.i = match.end()
            self.line_modes[self.line] = self.mode
            return

        if char == "{":
            end = self.src.find("}", self.i)
            end = self.n if end == -1 else end + 1
            self._push(self.i, end, "com")
            self._nl_scan(self.i, end)
            self.i = end
            return
        if two in ("//", "--"):
            end = self._eol(self.i)
            self._push(self.i, end, "com")
            self.i = end
            return
        if char in "\"'":
            self._scan_quoted(char)
            return
        if char == "?":
            match = WILD.match(self.src, self.i)
            if match:
                self._push(self.i, match.end(), "wild")
                self.i = match.end()
                return

        match = MODULE.match(self.src, self.i)
        if match:
            self._push(self.i, match.end(), "mod")
            self.i = match.end()
            return

        if char in "0123456789$#":
            match = AA_NUM.match(self.src, self.i)
            if match:
                self._push(self.i, match.end(), "num")
                self.i = match.end()
                return

        if char.isalpha() or char == "_":
            match = AA_WORD.match(self.src, self.i)
            if match:
                self._scan_aa_word(match.group(0), match.end())
                return

        if char in "([{)]}":
            self._bracket(char)
            return
        if char in "+-*/":
            self._push(self.i, self.i + 1, "op")
            self.i += 1
            return
        if char in ",;:":
            self._push(self.i, self.i + 1, "punct")
            self.i += 1
            return
        self.i += 1

    def _scan_aa_word(self, word: str, end: int) -> None:
        lower = word.casefold()
        next_char = self._next_char(end)
        first = self.col_first
        kind: str | None = None
        if lower in ASM_SIZE:
            kind = "size"
        elif ASM_REG.fullmatch(word):
            kind = "reg"
        elif lower in AA_CMDS and next_char == "(":
            kind = "aacmd"
        elif first and next_char == ":" and self.src[
            end + 1 : end + 2
        ] != ":":
            kind = "asmlab"
        elif first and lower in ASM_DIR:
            kind = "asmdir"
            self.mne_seen = True
        elif not self.mne_seen and (
            lower in ASM_JMP or (ASM_CC.fullmatch(lower) and lower[0] == "j")
        ):
            kind = "jmp"
            self.mne_seen = True
        elif not self.mne_seen and (
            lower in ASM_MNE or ASM_CC.fullmatch(lower)
        ):
            kind = "mne"
            self.mne_seen = True
        elif not first and HEX_WORD.fullmatch(word):
            kind = "num"
        elif next_char == "(":
            kind = "call"
        else:
            kind = "aasym"
        self._push(self.i, end, kind)
        self.i = end

    def _scan_c(self) -> None:
        char = self.src[self.i]
        two = self.src[self.i : self.i + 2]

        match = DIRECTIVE.match(self.src, self.i)
        if match:
            directive = match.group(0).casefold()
            if directive.startswith("{$lua"):
                self._set_mode("lua")
            elif directive.startswith("{$asm"):
                self._set_mode("aa")
            self._push(self.i, match.end(), "aadir")
            self.i = match.end()
            self.line_modes[self.line] = self.mode
            return
        if two == "//":
            end = self._eol(self.i)
            self._push(self.i, end, "com")
            self.i = end
            return
        if two == "/*":
            found = self.src.find("*/", self.i)
            end = self.n if found == -1 else found + 2
            self._push(self.i, end, "com")
            self._nl_scan(self.i, end)
            self.i = end
            return
        if char in "\"'":
            self._scan_quoted(char)
            return
        if char == "#":
            end = self._eol(self.i)
            self._push(self.i, end, "kwd")
            self.i = end
            return
        if char.isdigit():
            match = re.match(
                r"(?:0[xX][0-9a-fA-F]+|\d+\.?\d*(?:[eE][-+]?\d+)?)[uUlLfF]*",
                self.src[self.i :],
            )
            if match:
                self._push(self.i, self.i + match.end(), "num")
                self.i += match.end()
                return
        if char.isalpha() or char == "_":
            match = C_WORD.match(self.src, self.i)
            if match:
                word = match.group(0)
                next_char = self._next_char(match.end())
                if word in C_KW:
                    kind = "kw"
                elif word in C_TYPE:
                    kind = "size"
                elif next_char == "(":
                    kind = "call"
                else:
                    kind = None
                self._push(self.i, match.end(), kind)
                self.i = match.end()
                return
        if char in "([{)]}":
            self._bracket(char)
            return
        if char in "+-*/%^<>=&|!~":
            self._push(self.i, self.i + 1, "op")
            self.i += 1
            return
        if char in PUNCT:
            self._push(self.i, self.i + 1, "punct")
            self.i += 1
            return
        self.i += 1


def tokenize(src: str) -> LexResult:
    """Tokenize a CE Lua / AA / C mixed script."""
    return _Lexer(src).run()
