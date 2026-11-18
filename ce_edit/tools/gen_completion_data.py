"""Convert the public 5ilr completions.js data into local JSON."""

from __future__ import annotations

import json
import pathlib
import re

SOURCE = pathlib.Path(
    r"C:\Users\Administrator\AppData\Local\Temp\ce_completions.js"
)
TARGET = pathlib.Path(__file__).resolve().parents[1] / "ce_edit" / "data" / "completions.json"

ENTRY = re.compile(
    r'\{\s*c\s*:\s*"((?:[^"\\]|\\.)*)"\s*,\s*n\s*:\s*"((?:[^"\\]|\\.)*)"'
    r'\s*,\s*s\s*:\s*"((?:[^"\\]|\\.)*)"\s*\}'
)

EXTRAS = [
    ("CE 全局", "syntaxcheck", "syntaxcheck -- 当前脚本语法检查对象"),
    ("CE 全局", "memrec", "memrec -- 当前内存记录对象"),
    ("CE 全局", "process", "process -- 当前进程对象"),
    ("CE 全局", "TrainerOrigin", "TrainerOrigin -- 训练器来源"),
    ("CE 全局", "MainForm", "MainForm -- CE 主窗体"),
    ("CE 全局", "getLuaEngine", "getLuaEngine() -- 当前 Lua 引擎"),
    ("CE 全局", "targetIs64Bit", "targetIs64Bit() -- 目标是否 64 位"),
    ("CE 全局", "_G", "_G -- 全局表"),
    ("CE 全局", "_VERSION", "_VERSION -- Lua 版本"),
]


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    items = []
    for match in ENTRY.finditer(text):
        values = [json.loads('"' + group + '"') for group in match.groups()]
        items.append(
            {
                "category": values[0],
                "name": values[1],
                "signature": values[2],
            }
        )
    existing = {(item["category"], item["name"]) for item in items}
    for category, name, signature in EXTRAS:
        if (category, name) not in existing:
            items.append(
                {
                    "category": category,
                    "name": name,
                    "signature": signature,
                }
            )
    TARGET.parent.mkdir(parents=True, exist_ok=True)
    TARGET.write_text(
        json.dumps(items, ensure_ascii=False, indent=1) + "\n",
        encoding="utf-8",
    )
    print(f"parsed={len(items)} unique={len({i['name'] for i in items})}")
    print(f"wrote={TARGET}")


if __name__ == "__main__":
    main()
