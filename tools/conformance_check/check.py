#!/usr/bin/env python3
"""Enforce one-to-one traceability from normative Rule IDs to conformance."""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

RULE_LINE = re.compile(r"^\*\*(OM-[0-9]{2}\.[0-9]{2}(?:\.[a-z])?)\s+—")


def check(root: Path) -> tuple[list[str], str]:
    root = root.resolve()
    errors: list[str] = []
    standard_path = root / "OMBasic-Language-Standard.md"
    manifest_path = root / "tests/conformance/manifest.json"
    validator_path = root / "tests/conformance/validate_manifest.py"
    if not standard_path.is_file():
        return ["missing OMBasic-Language-Standard.md"], ""
    if not manifest_path.is_file():
        return ["missing tests/conformance/manifest.json"], ""

    standard_rules: list[str] = []
    for line in standard_path.read_text(encoding="utf-8").splitlines():
        match = RULE_LINE.match(line)
        if match:
            standard_rules.append(match.group(1))
    rule_counts = Counter(standard_rules)
    duplicate_rules = sorted(rule for rule, count in rule_counts.items() if count > 1)
    if duplicate_rules:
        errors.append(f"duplicate Rule IDs in Standard: {', '.join(duplicate_rules)}")
    if not standard_rules:
        errors.append("Standard defines no normative Rule IDs")

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return [f"cannot read conformance manifest: {error}"], ""
    cases = manifest.get("cases") if isinstance(manifest, dict) else None
    if not isinstance(cases, list):
        return errors + ["conformance manifest cases must be an array"], ""

    if validator_path.is_file():
        sys.path.insert(0, str(validator_path.parent))
        try:
            from validate_manifest import validate
            errors.extend(validate(manifest_path))
        except ImportError as error:
            errors.append(f"cannot load manifest validator: {error}")

    known_rules = set(standard_rules)
    covered: set[str] = set()
    kinds: Counter[str] = Counter()
    for index, case in enumerate(cases):
        if not isinstance(case, dict):
            continue
        kind = case.get("kind")
        if isinstance(kind, str):
            kinds[kind] += 1
        rules = case.get("rules")
        if not isinstance(rules, list):
            continue
        for rule in rules:
            if not isinstance(rule, str):
                continue
            if rule not in known_rules:
                errors.append(f"cases[{index}] references unknown Rule ID {rule}")
            else:
                covered.add(rule)
    uncovered = sorted(known_rules - covered)
    if uncovered:
        errors.append("uncovered normative Rules: " + ", ".join(uncovered))

    report_lines = [
        "OMBasic 0.1 Conformance Coverage",
        "",
        f"Normative Rules: {len(known_rules)}",
        f"Covered Rules:   {len(known_rules & covered)}",
        f"Uncovered Rules: {len(uncovered)}",
        f"Cases:           {len(cases)}",
        "",
        "Cases by kind:",
    ]
    for kind in sorted(kinds):
        report_lines.append(f"  {kind}: {kinds[kind]}")
    if uncovered:
        report_lines.extend(["", "Uncovered Rule IDs:", *[f"  {rule}" for rule in uncovered]])
    report = "\n".join(report_lines) + "\n"
    return errors, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    errors, report = check(args.root)
    if args.report is not None:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(report, encoding="utf-8", newline="\n")
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        if report:
            print(report, file=sys.stderr, end="")
        return 1
    print(report, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
