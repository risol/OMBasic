#!/usr/bin/env python3
"""Exercise Console_Read_Line with redirected input and shutdown."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


def run_case(compiler: Path, root: Path, source: str, data: bytes | None,
             *, keep_stdin_open: bool = False) -> tuple[int, str, str]:
    def decode(data: bytes) -> str:
        return data.decode("utf-8", "replace").replace("\r\n", "\n")

    command = [str(compiler), "run", "--root", str(root), source]
    if keep_stdin_open:
        process = subprocess.Popen(command, cwd=root, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            if process.stdin is not None:
                process.stdin.close()
            stdout = process.stdout.read() if process.stdout is not None else b""
            stderr = process.stderr.read() if process.stderr is not None else b""
            return 124, decode(stdout), decode(stderr)
        if process.stdin is not None:
            process.stdin.close()
        stdout = process.stdout.read() if process.stdout is not None else b""
        stderr = process.stderr.read() if process.stderr is not None else b""
        return process.returncode, decode(stdout), decode(stderr)
    result = subprocess.run(command, cwd=root, input=data,
                            capture_output=True, timeout=30, check=False)
    return result.returncode, decode(result.stdout), decode(result.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--case-json", required=True)
    parser.add_argument("--repository", type=Path, required=True)
    args = parser.parse_args()
    case = json.loads(args.case_json)
    root = (args.repository / "17_actors").resolve()
    compiler = args.compiler.resolve()

    expected = [
        ("system_console_read_line.bas", b"\r\nhello\r\nfinal",
         "line=[]\nline=[hello]\nline=[final]\nerror=end_of_input\n"),
        ("system_console_read_line.bas", b"\xff\n",
         "error=invalid_utf8\n"),
    ]
    for source, data, expected_stdout in expected:
        code, stdout, stderr = run_case(compiler, root, source, data)
        if code != 0 or stdout != expected_stdout:
            print(f"{source}: expected exit=0 stdout={expected_stdout!r}; "
                  f"got exit={code} stdout={stdout!r} stderr={stderr!r}",
                  file=sys.stderr)
            return 1

    code, stdout, stderr = run_case(compiler, root,
        "system_console_read_line_shutdown.bas", None,
        keep_stdin_open=True)
    if code != 0 or stdout != "":
        print("shutdown cancellation failed: "
              f"exit={code} stdout={stdout!r} stderr={stderr!r}",
              file=sys.stderr)
        return 1
    print("Console_Read_Line input, UTF-8, EOF, and shutdown passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
