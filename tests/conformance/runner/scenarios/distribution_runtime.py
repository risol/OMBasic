#!/usr/bin/env python3
"""Run the native two-process distribution contract alongside language cases."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--case-json", required=True)
    parser.add_argument("--repository", type=Path, required=True)
    args = parser.parse_args()

    suffix = ".exe" if sys.platform == "win32" else ""
    executable = args.compiler.resolve().with_name(
        f"ombasic_runtime_distribution_tests{suffix}")
    if not executable.is_file():
        print(f"distribution test executable is missing: {executable}",
              file=sys.stderr)
        return 2
    integration = args.repository.resolve() / "tests" / "distribution" \
        / "runtime_integration.py"
    result = subprocess.run(
        [sys.executable, str(integration), str(executable)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=115,
        check=False,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
