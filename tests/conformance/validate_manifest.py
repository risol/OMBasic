#!/usr/bin/env python3
"""Validate the conformance fixture manifest and its source spans."""

from __future__ import annotations

import json
import sys
from pathlib import Path


def validate(manifest_path: Path) -> list[str]:
    errors: list[str] = []
    root = manifest_path.parent.resolve()
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return [f"cannot read manifest: {error}"]

    if manifest.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    cases = manifest.get("cases")
    if not isinstance(cases, list):
        return errors + ["cases must be an array"]

    seen_ids: set[str] = set()
    for index, case in enumerate(cases):
        label = f"cases[{index}]"
        if not isinstance(case, dict):
            errors.append(f"{label} must be an object")
            continue
        case_id = case.get("id")
        if not isinstance(case_id, str) or not case_id:
            errors.append(f"{label}.id must be a non-empty string")
        elif case_id in seen_ids:
            errors.append(f"duplicate case id: {case_id}")
        else:
            seen_ids.add(case_id)

        source_name = case.get("source")
        if not isinstance(source_name, str) or not source_name:
            errors.append(f"{label}.source must be a non-empty path")
            continue
        source_path = (root / source_name).resolve()
        if root not in source_path.parents or source_path.suffix != ".bas":
            errors.append(f"{label}.source must be a .bas file below the manifest directory")
            continue
        try:
            source = source_path.read_bytes()
        except OSError as error:
            errors.append(f"{label}.source cannot be read: {error}")
            continue
        if source.startswith(b"\xef\xbb\xbf"):
            errors.append(f"{label}.source must not contain a UTF-8 BOM")
        try:
            source.decode("utf-8")
        except UnicodeDecodeError as error:
            errors.append(f"{label}.source is not valid UTF-8: {error}")

        expectation = case.get("expect")
        if not isinstance(expectation, dict) or not isinstance(
            expectation.get("compiles"), bool
        ):
            errors.append(f"{label}.expect.compiles must be a boolean")
            continue
        diagnostic = expectation.get("diagnostic")
        if expectation["compiles"]:
            if diagnostic is not None:
                errors.append(f"{label}: accepted case must not specify a diagnostic")
            continue
        if not isinstance(diagnostic, dict):
            errors.append(f"{label}: rejected case must specify a diagnostic")
            continue
        if not isinstance(diagnostic.get("code"), str):
            errors.append(f"{label}.expect.diagnostic.code must be a string")
        span = diagnostic.get("primary_span")
        if not isinstance(span, dict):
            errors.append(f"{label}.expect.diagnostic.primary_span must be an object")
            continue
        line_number = span.get("line")
        start = span.get("start_byte_column")
        end = span.get("end_byte_column")
        if not all(isinstance(value, int) and value > 0 for value in (line_number, start, end)):
            errors.append(f"{label}: span line and columns must be positive integers")
            continue
        if end <= start:
            errors.append(f"{label}: span end must be greater than its start")
            continue
        lines = source.splitlines()
        if line_number > len(lines):
            errors.append(f"{label}: span line is outside the source")
            continue
        expected_text = diagnostic.get("text")
        if not isinstance(expected_text, str):
            errors.append(f"{label}.expect.diagnostic.text must identify the primary token")
            continue
        actual = lines[line_number - 1][start - 1 : end - 1].decode("utf-8", errors="replace")
        if actual != expected_text:
            errors.append(
                f"{label}: span selects {actual!r}, expected primary text {expected_text!r}"
            )

    return errors


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate_manifest.py MANIFEST", file=sys.stderr)
        return 2
    errors = validate(Path(sys.argv[1]))
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("Conformance manifest is valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
