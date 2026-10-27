#!/usr/bin/env python3
"""Generate a reviewable scalar Native signature skeleton from Clang's AST."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys


SCALARS = {
    "void": "CVoid",
    "char": "CInt8",
    "signed char": "CInt8",
    "unsigned char": "CUInt8",
    "int8_t": "CInt8",
    "uint8_t": "CUInt8",
    "short": "CInt16",
    "short int": "CInt16",
    "signed short": "CInt16",
    "signed short int": "CInt16",
    "unsigned short": "CUInt16",
    "unsigned short int": "CUInt16",
    "int16_t": "CInt16",
    "uint16_t": "CUInt16",
    "int": "CInt",
    "signed": "CInt",
    "signed int": "CInt",
    "unsigned": "CUInt",
    "unsigned int": "CUInt",
    "int32_t": "CInt32",
    "uint32_t": "CUInt32",
    "long": "CLong",
    "long int": "CLong",
    "unsigned long": "CULong",
    "unsigned long int": "CULong",
    "long long": "CLongLong",
    "long long int": "CLongLong",
    "unsigned long long": "CULongLong",
    "unsigned long long int": "CULongLong",
    "int64_t": "CInt64",
    "uint64_t": "CUInt64",
    "size_t": "CSize",
    "float": "CFloat",
    "double": "CDouble",
    "_Bool": "CBool",
    "bool": "CBool",
}


def normalized_path(value: str, base: pathlib.Path) -> pathlib.Path:
    path = pathlib.Path(value)
    if not path.is_absolute():
        path = base / path
    try:
        return path.resolve()
    except OSError:
        return path.absolute()


def function_declarations(tree: dict, header: pathlib.Path, cwd: pathlib.Path):
    current_file: pathlib.Path | None = None
    for node in tree.get("inner", []):
        loc = node.get("loc", {})
        range_begin = node.get("range", {}).get("begin", {})
        file_name = loc.get("file") or range_begin.get("file")
        if file_name:
            current_file = normalized_path(file_name, cwd)
        if node.get("kind") != "FunctionDecl" or node.get("isImplicit"):
            continue
        if current_file != header:
            continue
        if node.get("storageClass") == "static":
            continue
        yield node


def ordinary_name(value: str, fallback: str) -> str:
    result = []
    for char in value.lower():
        if "a" <= char <= "z" or "0" <= char <= "9" or char == "_":
            result.append(char)
        else:
            result.append("_")
    name = "".join(result).strip("_") or fallback
    if name[0].isdigit():
        name = "native_" + name
    return name


def map_type(qual_type: str) -> tuple[str | None, bool]:
    spelling = " ".join(qual_type.replace("const ", "").replace("volatile ", "").split())
    if spelling in SCALARS:
        return SCALARS[spelling], False
    if spelling.endswith(" *"):
        pointee = spelling[:-2].strip()
        if pointee == "char" or pointee == "signed char":
            return "CString", True
        if pointee == "unsigned char":
            return "CPointer", True
        return "CPointer", True
    return None, "*" in spelling or "(" in spelling


def escape_omi_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def make_binding(function: dict, used_names: set[str]) -> tuple[str | None, str | None]:
    source_name = function.get("name", "")
    if not source_name:
        return None, "unnamed function"
    name = ordinary_name(source_name, "native_function")
    if name in {"action", "actor", "and", "at", "break", "case", "continue",
                "else", "end", "export", "failed", "false", "for", "from",
                "function", "if", "import", "in", "message", "native", "not",
                "normal", "null", "or", "record", "receive", "return", "select",
                "self", "send", "spawn", "stop", "step", "then", "to", "true",
                "try", "var", "watch", "while", "with"}:
        name = "native_" + name
    base = name
    suffix = 2
    while name in used_names:
        name = f"{base}_{suffix}"
        suffix += 1
    used_names.add(name)

    inner = function.get("inner", [])
    params = [item for item in inner if item.get("kind") == "ParmVarDecl"]
    parameters = []
    for index, parameter in enumerate(params):
        parameter_type, is_pointer = map_type(parameter.get("type", {}).get("qualType", ""))
        if parameter_type is None or parameter_type == "CVoid":
            return None, f"{source_name}: unsupported parameter type {parameter.get('type', {}).get('qualType', '?')}"
        parameter_name = ordinary_name(parameter.get("name", ""), f"argument_{index + 1}")
        ownership = " BORROWED" if is_pointer else ""
        parameters.append(f"{parameter_name}: {parameter_type}{ownership}")
    function_type = function.get("type", {}).get("qualType", "")
    result_type_spelling = function_type.split("(", 1)[0].strip()
    result_type, result_is_pointer = map_type(result_type_spelling)
    if result_type is None:
        return None, f"{source_name}: unsupported result type {result_type_spelling or '?'}"
    if "..." in function_type:
        return None, f"{source_name}: variadic functions require a manual wrapper"

    lines = [f"FUNCTION {name}({', '.join(parameters)}): {result_type}",
             f'    SYMBOL "{source_name}"',
             "    EFFECT NATIVE",
             "    BLOCKING"]
    if result_is_pointer:
        lines.append("    RETURNS STATIC")
    lines.append("END FUNCTION")
    return "\n".join(lines), None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", required=True, help="C or extern-C header to inspect")
    parser.add_argument("--library", required=True, help="Native library path for NATIVE block")
    parser.add_argument("--output", required=True, help="Output .omi file")
    parser.add_argument("--clang", default="clang", help="Clang executable (default: clang on PATH)")
    parser.add_argument("-I", dest="include_dirs", action="append", default=[],
                        help="Additional include directory; may be repeated")
    args = parser.parse_args()
    cwd = pathlib.Path.cwd()
    header = normalized_path(args.header, cwd)
    if not header.is_file():
        parser.error(f"header does not exist: {header}")
    command = [args.clang, "-Xclang", "-ast-dump=json", "-fsyntax-only"]
    for directory in args.include_dirs:
        command.extend(["-I", directory])
    command.append(str(header))
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", check=False)
    if completed.returncode != 0:
        sys.stderr.write(completed.stderr)
        return completed.returncode or 1
    try:
        tree = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        print(f"ombasic bind: could not parse Clang AST JSON: {error}", file=sys.stderr)
        return 1

    used_names: set[str] = set()
    bindings = []
    skipped = []
    for declaration in function_declarations(tree, header, cwd):
        binding, reason = make_binding(declaration, used_names)
        if binding is None:
            skipped.append(reason or declaration.get("name", "unknown function"))
        else:
            bindings.append(binding)
    if not bindings:
        print("ombasic bind: no supported non-static functions were found in the header.",
              file=sys.stderr)
        for reason in skipped:
            print(f"  skipped: {reason}", file=sys.stderr)
        return 1
    output = (f"NATIVE {escape_omi_string(args.library)}\n"
              + "\n\n".join(bindings) + "\nEND NATIVE\n")
    destination = pathlib.Path(args.output)
    try:
        destination.write_text(output, encoding="utf-8", newline="\n")
    except OSError as error:
        print(f"ombasic bind: cannot write {destination}: {error}", file=sys.stderr)
        return 1
    print(f"generated {len(bindings)} binding(s) in {destination}", file=sys.stderr)
    print("review the BLOCKING/EFFECT defaults and pointer ownership placeholders before using this metadata.",
          file=sys.stderr)
    if any("pointer" in reason.lower() for reason in skipped):
        print("pointer/function-pointer declarations were skipped; add manual OMI metadata.",
              file=sys.stderr)
    for reason in skipped:
        print(f"skipped: {reason}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
