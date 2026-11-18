"""Focused checks for the generated CE completion / keyword data.

The C side consumes ``ce_completions_data.c/.h`` verbatim, so the glyph ids,
category mapping and the five keyword sets must stay in sync with the Python
catalogue.  These tests read the generated files; they never rewrite them.
"""

from __future__ import annotations

import importlib.util
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "tools" / "gen_ce_keywords.py"
HEADER = ROOT / "src" / "StyleLexers" / "ce_completions_data.h"
DATA = ROOT / "src" / "StyleLexers" / "ce_completions_data.c"


@pytest.fixture(scope="module")
def gen():
    spec = importlib.util.spec_from_file_location("gen_ce_keywords", GEN)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)  # does not run main(): __name__ != "__main__"
    return module


@pytest.fixture(scope="module")
def data_text() -> str:
    return DATA.read_text(encoding="utf-8")


def _keyword_set(text: str, name: str) -> set[str]:
    """Rejoin the generated multi-line C literals into one word set."""
    block = re.search(rf"CE_KEYWORDS_{name}\[\] =\s*(.*?);", text, re.S)
    assert block, name
    joined = "".join(re.findall(r'"([^"]*)"', block.group(1)))
    return set(joined.split())


def test_generated_kind_enum_matches_generator(gen):
    header = HEADER.read_text(encoding="utf-8")
    for name in (
        "NONE", "KEYWORD", "FUNCTION", "VARIABLE", "AA", "ASM",
        "TYPE", "CONSTANT", "SECTION", "COUNT",
    ):
        value = getattr(gen, "KIND_" + name)
        assert re.search(rf"#define CE_KIND_{name}\s+{value}\b", header), name


def test_kind_labels_cover_every_id(gen, data_text):
    assert len(gen.KIND_LABELS) == gen.KIND_COUNT
    block = re.search(r"g_ceKindLabels\[\] = \{(.*?)\};", data_text, re.S)
    assert block
    assert len(re.findall(r'^\s*"', block.group(1), re.M)) == gen.KIND_COUNT


def test_all_catalogue_categories_have_a_glyph(gen):
    for category in (*gen.completions.category_order(), "CE Lua API", "汇编指令"):
        assert 1 <= gen.completion_kind(category) < gen.KIND_COUNT, category


def test_every_generated_entry_carries_a_valid_glyph(gen, data_text):
    rows = re.findall(r'^\s*\{ ".*", \d+, (\d+) \},$', data_text, re.M)
    assert rows, "no generated CE_COMPLETION_ENTRY rows"
    count = re.search(r"g_ceCompletionCount = (\d+)", data_text)
    assert count
    assert len(rows) == int(count.group(1))
    assert len(rows) > len(gen.completions.load_completions())
    assert all(1 <= int(kind) < gen.KIND_COUNT for kind in rows)


def test_keyword_sets_cover_lua_aa_asm_and_types(data_text):
    """The catalogue alone misses registers/mnemonics: the five sets fill them."""
    words = {
        name: _keyword_set(data_text, name)
        for name in ("LUA", "API", "AA", "ASM", "TYPES")
    }
    assert {"function", "local", "then", "string"} <= words["LUA"]
    assert {"alloc", "aobscanmodule", "registersymbol", "label"} <= words["AA"]
    assert {"mov", "jmp", "rax", "xmm0", "r15d", "vzeroupper", "jne", "cmovne"} <= words["ASM"]
    assert words["API"] and words["TYPES"]


def test_every_lexer_keyword_is_completable(data_text):
    """Anything the lexer colours must also be offered by autocomplete.

    The curated catalogue used to miss Lua standard libraries and most AA /
    inline-C directives even though CE_KEYWORDS_* highlighted them.
    """
    block = re.search(r"g_ceCompletions\[\] = \{(.*?)\};", data_text, re.S)
    assert block
    names = set(re.findall(r'^\s*\{ "([^"]+)",', block.group(1), re.M))
    for set_name in ("LUA", "API", "AA", "ASM", "TYPES"):
        missing = _keyword_set(data_text, set_name) - names
        assert not missing, f"{set_name}: {sorted(missing)[:10]}"
    # State-switch markers are not identifier shaped, so no keyword set can
    # carry them; they still have to be completion items.
    assert {
        "[ENABLE]", "[DISABLE]", "{$lua}", "{$asm}", "{$ccode}",
        "{$c}", "{$code}", "{$CODE}", "{$STDCALL}", "{$fastcode}",
    } <= names


def test_celua_api_and_asm_words_reach_the_catalogue(gen, data_text):
    """CE's celua.txt functions and the extra x86 words are completable."""
    lua_api = {entry.name for entry in gen.load_lua_api()}
    assert {"getCEVersion", "readBytesLocal", "createSymbolList", "split"} <= lua_api
    names = set(re.findall(r'^\s*\{ "([^"]+)",', data_text, re.M))
    assert lua_api <= names
    assert "vzeroupper" in names
    assert {"getCEVersion", "readBytesLocal"} <= _keyword_set(data_text, "API")


def test_ce_pascal_source_api_reaches_the_catalogue(gen, data_text):
    """Registrations from CE's Lua*.pas / memscan.pas and Lua 5.3 additions."""
    source_api = {entry.name for entry in gen.load_source_api()}
    assert len(source_api) >= 150, "the upstream-source catalogue was trimmed"
    assert {
        "createNativeThread", "structure_addElement", "dbvm_getCR3",
        "MemScan", "LuaPipeServer", "math.cosh", "utf8.codepoint",
    } <= source_api
    names = set(re.findall(r'^\s*\{ "([^"]+)",', data_text, re.M))
    assert source_api <= names
    assert {"createNativeThread", "getCEName"} <= _keyword_set(data_text, "API")
    assert "MemScan" in _keyword_set(data_text, "TYPES")


def _doc_rows(text: str) -> list[tuple[str, str]]:
    """Parse the sparse usage-note table; the C source holds \\n escapes."""
    block = re.search(r"g_ceCompletionDocs\[\] = \{(.*?)\};", text, re.S)
    assert block
    rows = re.findall(r'^\s*\{ "([^"]+)", "(.*)" \},$', block.group(1), re.M)
    assert rows
    return rows


def test_generated_usage_notes_are_sorted_and_counted(gen, data_text):
    rows = _doc_rows(data_text)
    count = re.search(r"g_ceCompletionDocCount = (\d+)", data_text)
    assert count
    assert len(rows) == int(count.group(1))
    names = [name for name, _ in rows]
    assert names == sorted(names), "binary search needs strcmp order"
    assert len(names) == len(set(names))
    # A documented name is either a catalogue entry or a keyword that only the
    # generated lexer sets provide (AA directives); both are completable.
    catalogue = set(re.findall(r'^\s*\{ "([^"]+)", .*, \d+, \d+ \},$', data_text, re.M))
    keywords = set()
    for set_name in ("LUA", "API", "AA", "ASM", "TYPES"):
        keywords |= _keyword_set(data_text, set_name)
    assert set(names) <= (catalogue | keywords)
    assert len(names) >= 900, "expected broad keyword coverage"


def test_mov_note_covers_function_usage_and_pitfalls(gen, data_text):
    rows = dict(_doc_rows(data_text))
    assert "mov" in rows
    note = rows["mov"]
    for label in ("功能", "语法", "示例", "约束", "相关", "易错", "AA 语境"):
        assert f"{label}：" in note, label
    assert note.count("\\n") >= 6, "one line per angle"
    assert "eax,[ebx]" in note and "EFLAGS" in note
    assert "movzx" in note and "qword ptr" in note
    # Group templates must have expanded their {name} placeholder.
    assert all("{name}" not in doc for doc in rows.values())


def test_note_source_is_well_formed(gen):
    docs = gen.load_keyword_docs()
    assert "mov" in docs and "alloc" in docs and "jmp" in docs
    assert "[ENABLE]" in docs and "autoAssemble" in docs
    assert all("\n" in doc for doc in docs.values())
    assert all("{name}" not in doc for doc in docs.values())
    for key in gen.DOC_FIELDS:
        assert f"{key}：" in docs["mov"]


def test_ce_palette_matches_reference_theme():
    """INI keys are a name-based contract with the EDITLEXER style rows."""
    lexer = (ROOT / "src" / "StyleLexers" / "styleLexCE.c").read_text(encoding="utf-8")
    rows = set(re.findall(r'IDS_LEX_STR_\w+, L"([^"]+)"', lexer))
    assert "bold; fore:#CF222E" in lexer  # --c-kw, light theme
    assert "bold; fore:#0969DA" in lexer  # --c-mne

    for theme in ("Dark", "Obsidian", "Sombra"):
        text = (ROOT / "Build" / "Themes" / f"{theme}.ini").read_text(encoding="utf-8-sig")
        block = re.search(r"\[Cheat Engine Script\]\s*(.*?)(?:\r?\n\[|\Z)", text, re.S)
        assert block, theme
        section = block.group(1)
        keys = set(re.findall(r"^([^=\r\n]+)=", section, re.M)) - {"FileNameExtensions"}
        assert keys <= rows, (theme, keys - rows)
        assert {"Comment", "Keyword", "Auto Assembler", "Function"} <= keys, theme
        assert re.search(r"^Keyword=.*#FF7B72", section, re.M), theme  # --c-kw, dark


MONO_ROW = r'^\s*\{ "([^"]+)", "([^"]*)", "Mono/\.NET", \d+, \d+ \},$'


def _mono_entries(text: str) -> dict[str, str]:
    return dict(re.findall(MONO_ROW, text, re.M))


def test_mono_functions_are_completable_with_source_parameters(data_text):
    """Signatures must match CE 7.7 autorun/monoscript.lua, not the Wiki prose."""
    entries = _mono_entries(data_text)
    assert len(entries) >= 70, "the source-derived Mono/.NET family was trimmed"
    for name in ("mono_initialize", "LaunchMonoDataCollector", "mono_findClass"):
        assert name in entries, name
    assert entries["mono_findClass"] == "mono_findClass(namespace, classname)"
    assert entries["mono_class_getStaticFieldAddress"] == "mono_class_getStaticFieldAddress(domain, class)"
    assert entries["mono_invoke_method"] == "mono_invoke_method(domain, method, object, args)"
    assert entries["mono_findMethod"] == "mono_findMethod(namespace, classname, methodname, parameters)"
    # The Wiki's misspelling mono_iamge_rva_map must not be the offered name.
    assert "mono_image_rva_map" in entries
    assert "mono_iamge_rva_map" not in entries
    assert {"mono_initialize", "LaunchMonoDataCollector"} <= _keyword_set(data_text, "API")


def test_every_mono_entry_has_a_chinese_usage_note(data_text):
    """The detail pane needs a note for every Mono entry, not just a signature."""
    rows = dict(_doc_rows(data_text))
    entries = _mono_entries(data_text)
    assert len(entries) >= 70
    for name in entries:
        assert name in rows, name
        for label in ("功能", "语法", "示例", "约束", "相关"):
            assert f"{label}：" in rows[name], (name, label)
        # Notes must explain where the parameter list came from.
        assert "CE 7.7 autorun/monoscript.lua" in rows[name], name
    assert "monoscript.lua" in rows["mono_invoke_method"]
    assert "4 个参数" in rows["mono_invoke_method"]
    # Examples must use the same parameter lists as the source signatures.
    assert "mono_invoke_method(domain, methodId, instance," in rows["mono_invoke_method"]
    assert "'TakeDamage', nil)" in rows["mono_findMethod"]
    for name in entries:
        example = next(line for line in rows[name].split("\\n") if line.startswith("示例："))
        assert "..." not in example, name
