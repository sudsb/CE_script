"""Tests for the completion catalogue and prefix matching."""

from __future__ import annotations

from ce_edit.completions import (
    LUA_KEYWORDS,
    Completion,
    _keyword_matches,
    api_names,
    category_order,
    filter_completions,
    find_completion,
    grouped_reference,
    load_completions,
    word_prefix,
)


def test_catalogue_loads_and_is_unique() -> None:
    items = load_completions()
    assert len(items) > 500
    names = [item.name for item in items]
    assert len(names) == len(set(names))
    assert all(isinstance(item, Completion) for item in items)


def test_catalogue_contains_common_entries() -> None:
    names = {item.name for item in load_completions()}
    for name in ("getAddress", "readInteger", "writeInteger", "print", "openProcess"):
        assert name in names
    assert find_completion("getAddress") is not None
    assert find_completion("definitely_missing_name") is None


def test_receiver_aliases_are_added() -> None:
    names = {item.name for item in load_completions()}
    dotted = [name for name in names if "." in name]
    assert dotted
    assert all(" " not in name for name in dotted)


def test_is_call() -> None:
    address = find_completion("getAddress")
    assert address is not None and address.is_call is True
    for keyword in ("if", "local", "return"):
        assert keyword in LUA_KEYWORDS
        item = find_completion(keyword)
        if item is not None:
            assert item.is_call is False
    marker = find_completion("[ENABLE]")
    assert marker is not None and marker.is_call is False


def test_filter_prefix_first() -> None:
    results = filter_completions("getA", mode="lua", limit=20)
    assert results
    assert results[0].name.casefold().startswith("geta")
    assert all("geta" in item.name.casefold() for item in results)


def test_filter_respects_limit_and_case() -> None:
    results = filter_completions("READ", limit=5)
    assert len(results) <= 5
    assert all("read" in item.name.casefold() for item in results)


def test_filter_empty_prefix_returns_candidates() -> None:
    results = filter_completions("", limit=10)
    assert len(results) == 10
    assert len({item.name for item in results}) == 10
    catalogue = {item.name for item in load_completions()}
    assert all(item.name in catalogue for item in results)


def test_filter_mode_prefers_aa_in_aa_mode() -> None:
    lua_first = filter_completions("add", mode="lua", limit=3)
    aa_first = filter_completions("add", mode="aa", limit=3)
    assert lua_first and aa_first
    assert aa_first[0].category != lua_first[0].category
    assert aa_first[0].name == "addCIncludePath"


def test_word_prefix() -> None:
    assert word_prefix("local x = getAd", 16) == "getAd"
    assert word_prefix("readInteger", 11) == "readInteger"
    assert word_prefix("x = ", 4) == ""


def test_keyword_matching_never_splits_a_word() -> None:
    assert _keyword_matches("axcc", "ax")
    assert _keyword_matches("axcc_value", "value")
    assert not _keyword_matches("axcc", "cc")
    assert not _keyword_matches("axcc_value", "cc")
    assert _keyword_matches("anything", "")


def test_extra_keywords_are_not_fragment_matched() -> None:
    results = filter_completions("ax", mode="aa", limit=10, extra=["axcc", "game", "exe"])
    assert results[0].name == "axcc"
    dynamic = {item.name for item in results if item.category == "文档符号"}
    assert dynamic == {"axcc"}
    inner = filter_completions("cc", mode="aa", limit=30, extra=["axcc"])
    assert not any(item.category == "文档符号" for item in inner)


def test_api_names_excludes_non_api_categories() -> None:
    names = api_names()
    assert "getAddress" in names
    assert "readInteger" in names
    assert "if" not in names
    assert "eax" not in names


def test_category_order_matches_catalogue() -> None:
    order = category_order()
    seen = []
    for item in load_completions():
        if item.category not in seen:
            seen.append(item.category)
    assert list(order) == seen


def test_grouped_reference_covers_catalogue() -> None:
    groups = grouped_reference()
    assert groups
    total = sum(len(items) for _, items in groups)
    assert total == len(load_completions())
