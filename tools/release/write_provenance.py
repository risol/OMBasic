#!/usr/bin/env python3
"""Write a machine-readable provenance record for an installed OMBasic SDK."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
from pathlib import Path


def command_version(executable: str) -> str:
    result = subprocess.run(
        [executable, "--version"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=15,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"{executable} --version failed: {detail}")
    output = result.stdout.strip() or result.stderr.strip()
    if not output:
        raise RuntimeError(f"{executable} --version returned no version")
    return output.splitlines()[0].strip()


def git_output(root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=15,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git command failed")
    return result.stdout.strip()


def project_versions(root: Path) -> tuple[str, int]:
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    version_match = re.search(
        r"project\(\s*OMBasic\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        cmake,
        re.IGNORECASE,
    )
    if version_match is None:
        raise RuntimeError("cannot find the OMBasic project version in CMakeLists.txt")
    abi_header = (root / "include/ombasic/runtime/abi/base.h").read_text(
        encoding="utf-8")
    abi_match = re.search(r"^#define\s+OM_RUNTIME_ABI_VERSION\s+(\d+)\s*$",
                          abi_header, re.MULTILINE)
    if abi_match is None:
        raise RuntimeError("cannot find OM_RUNTIME_ABI_VERSION")
    return version_match.group(1), int(abi_match.group(1))


def write_provenance(root: Path, output: Path, target: str,
                     llvm_version: str, clang: str, lld: str,
                     cmake: str) -> None:
    root = root.resolve()
    output = output.resolve()
    version, runtime_abi = project_versions(root)
    source_commit = git_output(root, "rev-parse", "HEAD")
    working_tree = git_output(root, "status", "--porcelain", "--untracked-files=all")
    diff = git_output(root, "diff", "--binary", "HEAD")
    changes_hash = hashlib.sha256(diff.encode("utf-8")).hexdigest() if diff else None
    record = {
        "schema_version": 1,
        "ombasic": {
            "version": version,
            "commit": source_commit,
            "working_tree_clean": not working_tree,
            "working_tree_diff_sha256": changes_hash,
        },
        "toolchain": {
            "llvm": llvm_version,
            "clang": command_version(clang),
            "lld": command_version(lld),
            "cmake": command_version(cmake),
            "python": platform.python_version(),
        },
        "build": {
            "host_os": platform.platform(),
            "target_triple": target,
            "runtime_abi_version": runtime_abi,
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--llvm-version", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--lld", required=True)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    try:
        write_provenance(args.root, args.output, args.target, args.llvm_version,
                         args.clang, args.lld, args.cmake)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"cannot write SDK provenance: {error}", file=sys.stderr)
        return 1
    print(f"Wrote SDK provenance to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
