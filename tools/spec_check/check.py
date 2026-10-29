#!/usr/bin/env python3
"""Lint language examples and required Actor_Down_Reason wording."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

PUBLIC_DECLARATION = re.compile(
    r"\bPUBLIC\s+(?:FUNCTION|ACTION|RECORD|MESSAGE|ACTOR)\b"
)
OLD_REASON_KEYWORD = re.compile(
    r"\b(?:NORMAL|FAILED|NODE_UNREACHABLE)\b"
)
KEYWORD_ENUM = re.compile(
    r"\bOM_KEYWORD_(?:NORMAL|FAILED|NODE_UNREACHABLE)\b"
)
LEGACY_KEYWORD_ENTRY = re.compile(
    r"\{\s*['\"](?:NORMAL|FAILED|NODE_UNREACHABLE)['\"]\s*,"
)
FENCED_BLOCK = re.compile(r"```[^\r\n]*\r?\n(.*?)```", re.DOTALL)
KEYWORD_ENTRY = re.compile(r'^\s*\{"([A-Z]+)",\s*\d+,\s*OM_KEYWORD_',
                           re.MULTILINE)


def check(root: Path) -> list[str]:
    errors: list[str] = []
    standard_path = root / "OMBasic-Language-Standard.md"
    standard = standard_path.read_text(encoding="utf-8")
    try:
        keyword_section = standard.split("### 5.7 保留关键字", 1)[1].split("---", 1)[0]
        standard_keyword_block = FENCED_BLOCK.search(keyword_section)
        if standard_keyword_block is None:
            errors.append("OMBasic-Language-Standard.md: missing reserved-keyword table")
        else:
            standard_keywords = {
                line.strip() for line in standard_keyword_block.group(1).splitlines()
                if line.strip()
            }
            lexer_source = (root / "src/syntax/lexer/lexer.c").read_text(encoding="utf-8")
            lexer_keywords = set(KEYWORD_ENTRY.findall(lexer_source))
            if standard_keywords != lexer_keywords:
                missing = sorted(lexer_keywords - standard_keywords)
                extra = sorted(standard_keywords - lexer_keywords)
                errors.append(
                    "reserved-keyword table differs from lexer: "
                    f"missing={missing}, extra={extra}"
                )
            for legacy in ("NORMAL", "FAILED", "NODE_UNREACHABLE", "PUBLIC"):
                if legacy in standard_keywords:
                    errors.append(f"reserved-keyword table contains legacy spelling {legacy}")
    except IndexError:
        errors.append("OMBasic-Language-Standard.md: missing section 5.7")

    required_values = (
        "Actor_Down_Reason.normal",
        "Actor_Down_Reason.failed",
        "Actor_Down_Reason.node_unreachable",
    )
    for value in required_values:
        if value not in standard:
            errors.append(f"OMBasic-Language-Standard.md: missing {value}")
    if "Actor_Down" not in standard:
        errors.append("OMBasic-Language-Standard.md: missing built-in Actor_Down")
    if not any(phrase in standard for phrase in (
        "不证明该 Actor 已经终止",
        "not proof that the remote Actor has terminated",
    )):
        errors.append(
            "OMBasic-Language-Standard.md: node_unreachable must state that "
            "termination is not proven"
        )

    markdown_files = [standard_path, *sorted((root / "docs").rglob("*.md"))]
    for path in markdown_files:
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        for block in FENCED_BLOCK.finditer(text):
            for pattern, description in (
                (PUBLIC_DECLARATION, "PUBLIC declaration syntax"),
                (OLD_REASON_KEYWORD, "legacy Actor down reason keyword"),
            ):
                match = pattern.search(block.group(1))
                if match is not None:
                    line = text.count("\n", 0, block.start(1) + match.start()) + 1
                    errors.append(f"{relative}:{line}: forbidden {description}")

    source_files = [
        *sorted((root / "examples").rglob("*.bas")),
        *sorted((root / "tests").rglob("*.bas")),
    ]
    expected_negative_fixtures: set[Path] = set()
    manifest_path = root / "tests/conformance/manifest.json"
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            for case in manifest.get("cases", []):
                if (isinstance(case, dict)
                        and isinstance(case.get("source"), str)
                        and isinstance(case.get("expect"), dict)
                        and case["expect"].get("compiles") is False):
                    expected_negative_fixtures.add(
                        (root / "tests/conformance" / case["source"]).resolve())
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            errors.append(f"tests/conformance/manifest.json: {error}")
    for path in source_files:
        relative = path.relative_to(root).as_posix()
        if path.resolve() in expected_negative_fixtures:
            continue
        text = path.read_text(encoding="utf-8")
        for pattern, description in (
            (PUBLIC_DECLARATION, "PUBLIC declaration syntax"),
            (OLD_REASON_KEYWORD, "legacy Actor down reason keyword"),
        ):
            match = pattern.search(text)
            if match is not None:
                line = text.count("\n", 0, match.start()) + 1
                errors.append(f"{relative}:{line}: forbidden {description}")

    for base in (root / "src", root / "include", root / "tests"):
        for path in sorted(base.rglob("*.c")) + sorted(base.rglob("*.h")):
            relative = path.relative_to(root).as_posix()
            source = path.read_text(encoding="utf-8")
            if KEYWORD_ENUM.search(source):
                errors.append(
                    f"{relative}: legacy Actor down reason keyword enum "
                    "must not be reintroduced"
                )
            if (relative.startswith("src/syntax/")
                    and LEGACY_KEYWORD_ENTRY.search(source)):
                errors.append(
                    f"{relative}: legacy Actor down reason spelling is in "
                    "the lexer keyword table"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    errors = check(args.root.resolve())
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("Language specification checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
