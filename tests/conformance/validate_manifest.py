#!/usr/bin/env python3
"""Validate conformance manifest v2 and fixture-relative diagnostics."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

CASE_ID = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
RULE_ID = re.compile(r"^OM-[0-9]{2}\.[0-9]{2}(?:\.[a-z])?$")
DIAGNOSTIC_CODE = re.compile(r"^E[0-9]{4}$")
CASE_KINDS = {"compile", "run", "multi_file", "multi_process"}
TAGS = {
    "lexer", "parser", "semantic", "type", "effect", "actor", "runtime",
    "native", "distribution", "diagnostic", "windows", "linux", "slow",
}
PLATFORMS = {"windows", "linux"}
CASE_FIELDS = {
    "id", "rules", "kind", "source", "project_root", "entry", "expect",
    "platforms", "targets", "tags", "scenario", "timeout_seconds",
    "native_metadata", "pipeline",
    "include_interfaces", "expected_exports",
}


def path_below(root: Path, value: Any, label: str, errors: list[str],
               suffix: str | None = None) -> Path | None:
    if not isinstance(value, str) or not value:
        errors.append(f"{label} must be a non-empty relative path")
        return None
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        errors.append(f"{label} must stay below the conformance directory")
        return None
    path = (root / relative).resolve()
    if root not in path.parents:
        errors.append(f"{label} must stay below the conformance directory")
        return None
    if suffix is not None and path.suffix != suffix:
        errors.append(f"{label} must use the {suffix} extension")
    return path


def source_for_case(root: Path, case: dict[str, Any], errors: list[str],
                    label: str) -> Path | None:
    if "source" in case:
        source = path_below(root, case["source"], f"{label}.source", errors,
                            ".bas")
        if source is not None and not source.is_file():
            errors.append(f"{label}.source does not exist")
            return None
        return source
    project_root = case.get("project_root")
    entry = case.get("entry")
    if project_root is None or entry is None:
        return None
    project = path_below(root, project_root, f"{label}.project_root", errors)
    if project is None:
        return None
    if not project.is_dir():
        errors.append(f"{label}.project_root is not a directory")
        return None
    entry_path = path_below(project, entry, f"{label}.entry", errors, ".bas")
    if entry_path is not None and not entry_path.is_file():
        errors.append(f"{label}.entry does not exist")
        return None
    return entry_path


def validate_diagnostic(root: Path, case: dict[str, Any], expectation: dict[str, Any],
                        source: Path | None, label: str,
                        errors: list[str]) -> None:
    diagnostic = expectation.get("diagnostic")
    if expectation.get("compiles"):
        if diagnostic is not None:
            errors.append(f"{label}: accepted case must not specify a diagnostic")
        return
    if not isinstance(diagnostic, dict):
        if isinstance(expectation.get("project_error"), str) \
                and expectation["project_error"]:
            return
        errors.append(f"{label}: rejected case must specify a diagnostic")
        return
    code = diagnostic.get("code")
    if not isinstance(code, str) or not DIAGNOSTIC_CODE.fullmatch(code):
        errors.append(f"{label}.expect.diagnostic.code must be a stable E#### code")
    span = diagnostic.get("primary_span")
    if not isinstance(span, dict):
        errors.append(f"{label}.expect.diagnostic.primary_span must be an object")
        return
    line_number = span.get("line")
    start = span.get("start_byte_column")
    end_line = span.get("end_line", line_number)
    end = span.get("end_byte_column")
    if not all(isinstance(value, int) and value > 0
               for value in (line_number, start, end_line, end)):
        errors.append(f"{label}: span lines and byte columns must be positive integers")
        return
    if end_line < line_number or (end_line == line_number and end <= start):
        errors.append(f"{label}: span end must be greater than its start")
        return

    span_source = source
    if "source" in diagnostic:
        span_source = path_below(root, diagnostic["source"],
                                 f"{label}.expect.diagnostic.source", errors,
                                 ".bas")
    if span_source is None or not span_source.is_file():
        errors.append(f"{label}: diagnostic needs an existing source fixture")
        return
    try:
        data = span_source.read_bytes()
    except OSError as error:
        errors.append(f"{label}: cannot read diagnostic source: {error}")
        return
    if data.startswith(b"\xef\xbb\xbf"):
        errors.append(f"{label}: source fixture must not contain a UTF-8 BOM")
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as error:
        errors.append(f"{label}: source fixture is not valid UTF-8: {error}")
        return
    lines = data.splitlines()
    if line_number > len(lines) or end_line > len(lines):
        errors.append(f"{label}: diagnostic line is outside its source")
        return
    expected_text = diagnostic.get("text")
    if expected_text is not None:
        if not isinstance(expected_text, str):
            errors.append(f"{label}.expect.diagnostic.text must be a string")
            return
        if end_line != line_number:
            errors.append(f"{label}: diagnostic text assertion must select one line")
            return
        actual = lines[line_number - 1][start - 1:end - 1].decode(
            "utf-8", errors="replace")
        if actual != expected_text:
            errors.append(
                f"{label}: span selects {actual!r}, expected {expected_text!r}")


def validate_case(root: Path, case: Any, index: int,
                  seen_ids: set[str]) -> list[str]:
    errors: list[str] = []
    label = f"cases[{index}]"
    if not isinstance(case, dict):
        return [f"{label} must be an object"]
    unknown = set(case) - CASE_FIELDS
    if unknown:
        errors.append(f"{label} has unknown fields: {sorted(unknown)}")

    case_id = case.get("id")
    if not isinstance(case_id, str) or not CASE_ID.fullmatch(case_id):
        errors.append(f"{label}.id must be a stable lowercase kebab-case id")
    elif case_id in seen_ids:
        errors.append(f"duplicate case id: {case_id}")
    else:
        seen_ids.add(case_id)

    rules = case.get("rules")
    if not isinstance(rules, list) or not rules:
        errors.append(f"{label}.rules must be a non-empty array")
    elif any(not isinstance(rule, str) or not RULE_ID.fullmatch(rule)
             for rule in rules):
        errors.append(f"{label}.rules contains an invalid Rule ID")
    elif len(set(rules)) != len(rules):
        errors.append(f"{label}.rules must not repeat a Rule ID")

    kind = case.get("kind")
    if kind not in CASE_KINDS:
        errors.append(f"{label}.kind must be one of {sorted(CASE_KINDS)}")
    if kind in {"compile", "run"} and "source" not in case:
        errors.append(f"{label}.source is required for {kind} cases")
    if kind == "multi_file" and not {"project_root", "entry"}.issubset(case):
        errors.append(f"{label} multi_file cases require project_root and entry")
    if kind == "multi_process":
        if "scenario" not in case:
            errors.append(f"{label}.scenario is required for multi_process cases")
        else:
            scenario = path_below(root, case["scenario"],
                                  f"{label}.scenario", errors, ".py")
            if scenario is not None and not scenario.is_file():
                errors.append(f"{label}.scenario does not exist")
    pipeline = case.get("pipeline", "check")
    if pipeline not in {"check", "build"}:
        errors.append(f"{label}.pipeline must be check or build")
    if pipeline == "build" and kind not in {"compile", "multi_file"}:
        errors.append(f"{label}.pipeline=build requires a compile case")
    if "include_interfaces" in case and not isinstance(case["include_interfaces"], bool):
        errors.append(f"{label}.include_interfaces must be a boolean")
    expected_exports = case.get("expected_exports", [])
    if not isinstance(expected_exports, list) or any(
            not isinstance(name, str) or not name for name in expected_exports):
        errors.append(f"{label}.expected_exports must contain non-empty names")
    if expected_exports and not case.get("include_interfaces", False):
        errors.append(f"{label}.expected_exports requires include_interfaces=true")

    native_metadata = case.get("native_metadata")
    if native_metadata is not None:
        base = ((root / case["project_root"]).resolve()
                if "project_root" in case
                else (root / case["source"]).resolve().parent
                if "source" in case else root)
        metadata = path_below(base, native_metadata,
                              f"{label}.native_metadata", errors, ".omi")
        if metadata is not None and not metadata.is_file():
            errors.append(f"{label}.native_metadata does not exist")

    source = source_for_case(root, case, errors, label)
    expectation = case.get("expect")
    if not isinstance(expectation, dict):
        errors.append(f"{label}.expect must be an object")
        expectation = {}
    if kind == "multi_process":
        if not isinstance(expectation.get("exit_code"), int):
            errors.append(f"{label}.expect.exit_code must be an integer")
    else:
        compiles = expectation.get("compiles")
        if not isinstance(compiles, bool):
            errors.append(f"{label}.expect.compiles must be a boolean")
        else:
            validate_diagnostic(root, case, expectation, source, label, errors)
        run = expectation.get("run")
        if kind == "run" and not isinstance(run, dict):
            errors.append(f"{label}: run cases must specify expect.run")
        if run is not None:
            if not isinstance(run, dict) or not isinstance(run.get("exit_code"), int):
                errors.append(f"{label}.expect.run.exit_code must be an integer")
            elif not expectation.get("compiles"):
                errors.append(f"{label}: a compile-fail case cannot also run")
            if isinstance(run, dict):
                for output in ("stdout", "stderr"):
                    if output in run and not isinstance(run[output], str):
                        errors.append(f"{label}.expect.run.{output} must be a string")
    if kind == "compile" and "run" in expectation:
        errors.append(f"{label}: compile cases cannot specify expect.run")
    if "diagnostic" in expectation:
        diagnostic = expectation["diagnostic"]
        if isinstance(diagnostic, dict) and not isinstance(
                diagnostic.get("primary_span"), dict):
            errors.append(f"{label}.expect.diagnostic.primary_span must be an object")

    tags = case.get("tags", [])
    if not isinstance(tags, list) or any(tag not in TAGS for tag in tags):
        errors.append(f"{label}.tags contains an unknown tag")
    platforms = case.get("platforms", [])
    if not isinstance(platforms, list) or any(p not in PLATFORMS for p in platforms):
        errors.append(f"{label}.platforms contains an unsupported platform")
    targets = case.get("targets", [])
    if not isinstance(targets, list) or any(not isinstance(t, str) or not t
                                             for t in targets):
        errors.append(f"{label}.targets must contain non-empty target names")
    timeout = case.get("timeout_seconds", 30)
    if not isinstance(timeout, int) or not 1 <= timeout <= 120:
        errors.append(f"{label}.timeout_seconds must be from 1 through 120")
    return errors


def validate(manifest_path: Path) -> list[str]:
    errors: list[str] = []
    root = manifest_path.parent.resolve()
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        return [f"cannot read manifest: {error}"]
    if not isinstance(manifest, dict):
        return ["manifest must be an object"]
    if manifest.get("schema_version") != 2:
        errors.append("schema_version must be 2")
    cases = manifest.get("cases")
    if not isinstance(cases, list):
        return errors + ["cases must be an array"]
    seen_ids: set[str] = set()
    for index, case in enumerate(cases):
        errors.extend(validate_case(root, case, index, seen_ids))
    if not cases:
        errors.append("cases must not be empty")
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
    print("Conformance manifest v2 is valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
