#!/usr/bin/env python3
"""Run compiler-facing OMBasic language conformance cases."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


def platform_name() -> str:
    return "windows" if os.name == "nt" else "linux"


def project_for_case(root: Path, case: dict[str, Any]) -> tuple[Path, str]:
    if "project_root" in case:
        return (root / case["project_root"]).resolve(), case["entry"]
    source = (root / case["source"]).resolve()
    return source.parent, source.name


def run_process(command: list[str], cwd: Path, timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout,
                          check=False)


def expected_diagnostic_matches(actual: dict[str, Any],
                                expected: dict[str, Any]) -> bool:
    if actual.get("code") != expected.get("code"):
        return False
    span = expected.get("primary_span", {})
    actual_range = actual.get("range", {})
    actual_start = actual_range.get("start", {})
    actual_end = actual_range.get("end", {})
    return (
        actual_start.get("line") == span.get("line")
        and actual_start.get("byteColumn") == span.get("start_byte_column")
        and actual_end.get("line") == span.get("end_line", span.get("line"))
        and actual_end.get("byteColumn") == span.get("end_byte_column")
    )


def check_compilation(compiler: Path, project_root: Path, entry: str,
                      case: dict[str, Any], timeout: int) -> list[str]:
    errors: list[str] = []
    pipeline = case.get("pipeline", "check")
    command = [str(compiler), "build" if pipeline == "build" else "check",
               "--root", str(project_root)]
    if "native_metadata" in case:
        command.extend(["--native", case["native_metadata"]])
    if case.get("include_interfaces", False):
        command.extend(["--include-interfaces"])
    if pipeline == "build":
        try:
            with tempfile.TemporaryDirectory(prefix="ombasic-conformance-") as directory:
                command.extend(["--emit=mir", "-o", str(Path(directory) / "case.mir"),
                                entry, "--diagnostics=json"])
                result = run_process(command, project_root, timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            return [f"compiler build failed to run: {error}"]
    else:
        command.extend([entry, "--diagnostics=json"])
        try:
            result = run_process(command, project_root, timeout)
        except (OSError, subprocess.TimeoutExpired) as error:
            return [f"compiler check failed to run: {error}"]
    expectation = case["expect"]
    if pipeline == "build" and expectation["compiles"] \
            and result.returncode == 0:
        return []
    try:
        analysis = json.loads(result.stdout)
        diagnostics = analysis.get("diagnostics")
    except (json.JSONDecodeError, AttributeError) as error:
        project_error = expectation.get("project_error")
        if (not expectation["compiles"] and isinstance(project_error, str)
                and result.returncode != 0 and project_error in result.stderr):
            return []
        return [f"compiler did not return diagnostic JSON: {error}; "
                f"stderr={result.stderr!r}"]
    if not isinstance(diagnostics, list):
        return ["compiler diagnostic JSON has no diagnostics array"]
    expected_exports = case.get("expected_exports", [])
    if expected_exports:
        interfaces = analysis.get("interfaces")
        exported_names = {
            symbol.get("name")
            for interface in interfaces if isinstance(interface, dict)
            for symbol in interface.get("symbols", [])
            if isinstance(symbol, dict)
        } if isinstance(interfaces, list) else set()
        missing_exports = sorted(set(expected_exports) - exported_names)
        if missing_exports:
            errors.append(f"interface metadata is missing exports {missing_exports}")

    compiles = expectation["compiles"]
    if compiles:
        if result.returncode != 0 or diagnostics:
            errors.append(
                f"expected compile success, got exit {result.returncode} "
                f"with diagnostics {diagnostics!r}")
    else:
        if result.returncode == 0:
            errors.append("expected compile failure, but check succeeded")
        expected = expectation["diagnostic"]
        if not any(expected_diagnostic_matches(item, expected)
                   for item in diagnostics if isinstance(item, dict)):
            errors.append(
                f"expected diagnostic {expected.get('code')} at "
                f"{expected.get('primary_span')}, got {diagnostics!r}")
    return errors


def run_runtime(compiler: Path, project_root: Path, entry: str,
                expectation: dict[str, Any], timeout: int) -> list[str]:
    try:
        result = run_process(
            [str(compiler), "run", "--root", str(project_root), entry],
            project_root, timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return [f"runtime case failed to run: {error}"]
    expected = expectation["run"]
    errors: list[str] = []
    if result.returncode != expected["exit_code"]:
        errors.append(f"expected run exit {expected['exit_code']}, got "
                      f"{result.returncode}; stderr={result.stderr!r}")
    for output in ("stdout", "stderr"):
        if output in expected and getattr(result, output) != expected[output]:
            errors.append(f"expected {output} {expected[output]!r}, got "
                          f"{getattr(result, output)!r}")
    return errors


def run_multi_process(root: Path, compiler: Path,
                      case: dict[str, Any], timeout: int) -> list[str]:
    scenario = (root / case["scenario"]).resolve()
    command = [sys.executable, str(scenario), "--compiler", str(compiler),
               "--case-json", json.dumps(case), "--repository", str(root)]
    try:
        result = run_process(command, root, timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        return [f"multi-process scenario failed to run: {error}"]
    expected = case["expect"]
    errors: list[str] = []
    if result.returncode != expected["exit_code"]:
        errors.append(f"scenario expected exit {expected['exit_code']}, got "
                      f"{result.returncode}; stderr={result.stderr!r}")
    for output in ("stdout", "stderr"):
        if output in expected and getattr(result, output) != expected[output]:
            errors.append(f"scenario expected {output} {expected[output]!r}, got "
                          f"{getattr(result, output)!r}")
    return errors


def should_run(case: dict[str, Any], suite: str, include_slow: bool) -> bool:
    if not include_slow and "slow" in case.get("tags", []):
        return False
    platforms = case.get("platforms", [])
    if platforms and platform_name() not in platforms:
        return False
    has_runtime = case.get("kind") in {"run", "multi_process"}
    has_runtime = has_runtime or "run" in case.get("expect", {})
    if suite == "static":
        return not has_runtime and case.get("kind") != "multi_process"
    if suite == "runtime":
        return has_runtime
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--suite", choices=("all", "static", "runtime"),
                        default="all")
    parser.add_argument("--include-slow", action="store_true")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    manifest_path = args.manifest.resolve()
    root = manifest_path.parent
    if not compiler.is_file():
        print(f"compiler does not exist: {compiler}", file=sys.stderr)
        return 2
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"cannot read manifest: {error}", file=sys.stderr)
        return 2
    cases = manifest.get("cases", [])
    selected = [case for case in cases if should_run(case, args.suite,
                                                      args.include_slow)]
    failures = 0
    for case in selected:
        case_id = case["id"]
        try:
            if case["kind"] == "multi_process":
                errors = run_multi_process(root, compiler, case,
                                           case.get("timeout_seconds", 30))
            else:
                project_root, entry = project_for_case(root, case)
                errors = check_compilation(
                    compiler, project_root, entry, case,
                    case.get("timeout_seconds", 30))
                if not errors and "run" in case.get("expect", {}):
                    errors.extend(run_runtime(
                        compiler, project_root, entry, case["expect"],
                        case.get("timeout_seconds", 30)))
        except (KeyError, TypeError, ValueError, OSError) as error:
            errors = [f"invalid case execution: {error}"]
        if errors:
            failures += 1
            for error in errors:
                print(f"FAIL {case_id}: {error}", file=sys.stderr)
        else:
            print(f"PASS {case_id}")
    print(f"{len(selected)} conformance cases, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
