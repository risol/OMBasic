#!/usr/bin/env python3
"""Enforce OMBasic's initial source-boundary and C-safety rules."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SOURCE_SUFFIXES = {".c", ".h", ".inc"}
RAW_ALLOCATORS = re.compile(r"\b(?:malloc|calloc|realloc|free|aligned_alloc)\s*\(")
FORBIDDEN_APIS = re.compile(r"\b(?:gets|strcpy|strcat|sprintf|alloca)\s*\(")
INCLUDE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.MULTILINE)

FORBIDDEN_INCLUDES = {
    "foundation": {"source", "syntax", "module_graph", "semantic", "bytecode", "compiler", "runtime", "cli"},
    "source": {"syntax", "module_graph", "semantic", "bytecode", "compiler", "runtime", "cli"},
    "syntax": {"semantic", "bytecode", "compiler", "runtime", "cli"},
    "module_graph": {"bytecode", "compiler", "runtime", "cli"},
    "semantic": {"compiler", "runtime", "cli"},
    "bytecode": {"compiler", "runtime", "cli"},
    "ffi": {"semantic", "bytecode", "runtime", "compiler", "cli"},
    "runtime": {"source", "syntax", "module_graph", "semantic", "compiler", "cli"},
}
ALLOCATOR_OWNERS = {
    "src/foundation/memory/",
    "src/foundation/arena/",
    "src/foundation/vector/",
    "src/runtime/heap/",
}


def strip_comments_and_literals(text: str) -> str:
    """Keep line breaks while blanking comments and literals for API scans."""
    pattern = re.compile(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    return pattern.sub(lambda match: "\n" * match.group(0).count("\n"), text)


def capability(path: str) -> tuple[str | None, str | None]:
    normalized = path.replace("\\", "/")
    for prefix in ("src/", "include/ombasic/", "ombasic/"):
        if normalized.startswith(prefix):
            rest = normalized[len(prefix):]
            parts = rest.split("/")
            if len(parts) > 1:
                return parts[0], "/".join(parts[1:])
    return None, None


def check(root: Path) -> list[str]:
    errors: list[str] = []
    source_files = [
        path for base in ("src", "include")
        for path in (root / base).rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    ]

    for path in source_files:
        relative = path.relative_to(root).as_posix()
        owner, _ = capability(relative)
        source = path.read_text(encoding="utf-8")
        code = strip_comments_and_literals(source)

        if RAW_ALLOCATORS.search(code) and not any(
            relative.startswith(allowed) for allowed in ALLOCATOR_OWNERS
        ):
            errors.append(f"{relative}: raw allocator call is outside an authorized module")
        if FORBIDDEN_APIS.search(code):
            errors.append(f"{relative}: forbidden unsafe C API call")

        if owner is not None:
            for include in INCLUDE.findall(source):
                include_path = include.replace("\\", "/")
                if "/internal/" in f"/{include_path}/":
                    included_owner, _ = capability(include_path)
                    if included_owner is not None and included_owner != owner:
                        errors.append(
                            f"{relative}: cross-capability include of private header {include}"
                        )
                included_capability, _ = capability(include_path)
                if included_capability in FORBIDDEN_INCLUDES.get(owner, set()):
                    errors.append(
                        f"{relative}: {owner} may not include {included_capability} ({include})"
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
    print("Architecture checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
