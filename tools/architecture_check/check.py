#!/usr/bin/env python3
"""Enforce OMBasic's capability boundaries and C-safety rules."""

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
    "foundation": {"source", "syntax", "module_graph", "semantic", "native",
                   "ir", "target", "backend", "compiler", "runtime", "tooling"},
    "source": {"syntax", "module_graph", "semantic", "native", "ir", "backend",
               "compiler", "runtime", "tooling"},
    "syntax": {"module_graph", "semantic", "native", "ir", "backend", "compiler",
               "runtime", "tooling"},
    "module_graph": {"semantic", "ir", "backend", "compiler", "runtime", "tooling"},
    "native": {"module_graph", "semantic", "ir", "backend", "compiler", "runtime",
               "tooling"},
    "semantic": {"ir", "target", "backend", "compiler", "runtime", "tooling"},
    "ir": {"backend", "compiler", "runtime", "tooling"},
    "target": {"backend", "compiler", "runtime", "tooling"},
    "backend": {"compiler", "runtime", "tooling"},
    "distribution": {"semantic", "ir", "backend", "compiler", "runtime", "tooling"},
    "runtime": {"source", "syntax", "module_graph", "native", "semantic", "ir",
                "target", "backend", "compiler", "tooling"},
}
ALLOCATOR_OWNERS = {
    "src/foundation/memory/",
    "src/foundation/arena/",
    "src/foundation/vector/",
    "src/runtime/gc/",
}
LEGACY_SYMBOLS = re.compile(
    r"\b(?:Om_VM\b|om_vm_[A-Za-z0-9_]*\b|Om_Bytecode\b|"
    r"om_bytecode_[A-Za-z0-9_]*\b|OM_BYTECODE_[A-Za-z0-9_]*\b|"
    r"OMBC\b|LOAD_SYSTEM_ACTOR\b)"
)

CORE_TARGET_EDGES = {
    "ombasic_source": {"ombasic::foundation"},
    "ombasic_syntax": {"ombasic::source"},
    "ombasic_module_graph": {"ombasic::source"},
    "ombasic_native_metadata": {"ombasic::source", "ombasic::syntax"},
    "ombasic_semantic": {"ombasic::syntax", "ombasic::module_graph",
                          "ombasic::native_metadata"},
    "ombasic_hir": {"ombasic::semantic"},
    "ombasic_specialization": {"ombasic::hir"},
    "ombasic_mir": {"ombasic::specialization"},
    "ombasic_target": {"ombasic::foundation"},
    "ombasic_backend_llvm": {"ombasic::mir", "ombasic::target"},
    "ombasic_compiler": {"ombasic::mir", "ombasic::backend_llvm"},
    "ombasic_linker": {"ombasic::foundation"},
    "ombasic_distribution": {"ombasic::foundation"},
    "ombasic_runtime": {"ombasic::foundation"},
    "ombasic_runtime_services": {"ombasic::foundation"},
    "ombasic_runtime_distribution": {"ombasic::runtime",
                                     "ombasic::distribution",
                                     "ombasic::foundation"},
    "ombasic_tooling": {"ombasic::syntax", "ombasic::semantic"},
    "ombasic": {"ombasic::tooling", "ombasic::native_metadata",
                "ombasic::compiler", "ombasic::linker"},
}
ALIAS_TARGETS = {
    "ombasic::foundation": "ombasic_foundation",
    "ombasic::source": "ombasic_source",
    "ombasic::syntax": "ombasic_syntax",
    "ombasic::module_graph": "ombasic_module_graph",
    "ombasic::native_metadata": "ombasic_native_metadata",
    "ombasic::semantic": "ombasic_semantic",
    "ombasic::hir": "ombasic_hir",
    "ombasic::specialization": "ombasic_specialization",
    "ombasic::mir": "ombasic_mir",
    "ombasic::target": "ombasic_target",
    "ombasic::backend_llvm": "ombasic_backend_llvm",
    "ombasic::compiler": "ombasic_compiler",
    "ombasic::linker": "ombasic_linker",
    "ombasic::distribution": "ombasic_distribution",
    "ombasic::runtime": "ombasic_runtime",
    "ombasic::runtime_services": "ombasic_runtime_services",
    "ombasic::runtime_distribution": "ombasic_runtime_distribution",
    "ombasic::tooling": "ombasic_tooling",
}


def strip_comments_and_literals(text: str) -> str:
    """Keep line breaks while blanking comments and literals for code scans."""
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


def check_cmake_graph(root: Path) -> list[str]:
    errors: list[str] = []
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    actual: dict[str, set[str]] = {name: set() for name in CORE_TARGET_EDGES}
    calls = re.finditer(
        r"target_link_libraries\s*\(\s*([A-Za-z0-9_:.+-]+)(.*?)\)",
        cmake,
        re.DOTALL,
    )
    for call in calls:
        target = call.group(1)
        if target in actual:
            actual[target].update(re.findall(r"\bombasic::[A-Za-z0-9_]+\b", call.group(2)))

    graph: dict[str, set[str]] = {name: set() for name in CORE_TARGET_EDGES}
    for target, allowed in CORE_TARGET_EDGES.items():
        dependencies = actual[target]
        unexpected = dependencies - allowed
        missing = allowed - dependencies
        if unexpected:
            errors.append(f"CMake target {target}: forbidden project dependencies {sorted(unexpected)}")
        if missing:
            errors.append(f"CMake target {target}: missing declared dependencies {sorted(missing)}")
        graph[target].update(
            ALIAS_TARGETS[dependency]
            for dependency in dependencies & allowed
            if ALIAS_TARGETS[dependency] in graph
        )

    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(target: str) -> None:
        if target in visiting:
            errors.append(f"CMake production target graph contains a cycle at {target}")
            return
        if target in visited:
            return
        visiting.add(target)
        for dependency in graph[target]:
            visit(dependency)
        visiting.remove(target)
        visited.add(target)

    for target in graph:
        visit(target)
    return errors


def check(root: Path) -> list[str]:
    errors = check_cmake_graph(root)
    source_files = [
        path for base in ("src", "include", "tests")
        for path in (root / base).rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES
    ]

    for path in source_files:
        relative = path.relative_to(root).as_posix()
        source = path.read_text(encoding="utf-8")
        code = strip_comments_and_literals(source)

        if LEGACY_SYMBOLS.search(code):
            errors.append(f"{relative}: legacy VM/bytecode symbol is forbidden")
        if not relative.startswith("tests/") and RAW_ALLOCATORS.search(code) and not any(
            relative.startswith(allowed) for allowed in ALLOCATOR_OWNERS
        ):
            errors.append(f"{relative}: raw allocator call is outside an authorized module")
        if FORBIDDEN_APIS.search(code):
            errors.append(f"{relative}: forbidden unsafe C API call")

        if "llvm-c/" in source and not relative.startswith("src/backend/llvm/"):
            errors.append(f"{relative}: LLVM C headers are private to src/backend/llvm")

        owner, _ = capability(relative)
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
