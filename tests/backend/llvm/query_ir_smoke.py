from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="ombasic-query-ir-") as temporary:
        artifact = pathlib.Path(temporary) / "query.ll"
        emitted = subprocess.run([
            str(compiler), "build", "--root", str(source.parent),
            source.name, "--emit=llvm-ir", "-o", str(artifact),
        ], capture_output=True, text=True, check=False)
        require(emitted.returncode == 0 and artifact.is_file(),
                f"LLVM IR emission failed: return={emitted.returncode}, "
                f"stderr={emitted.stderr!r}")
        ir = artifact.read_text(encoding="utf-8")

    for helper in (
        "om_runtime_list_length",
        "om_runtime_list_get",
        "om_runtime_list_builder_create",
        "om_runtime_list_builder_reserve",
        "om_runtime_list_builder_append_value",
        "om_runtime_list_builder_seal",
    ):
        require(f"@{helper}" in ir,
                f"Query LLVM IR does not reference {helper}")
    require("@om_runtime_list_stable_order" not in ir,
            "unordered scan Query unexpectedly invokes stable ordering")
    builder_append_calls = sum(
        line.count("call i32 @om_runtime_list_builder_append_value")
        for line in ir.splitlines())
    require(builder_append_calls == 9,
            "expected eight source-literal appends plus one fused SELECT "
            "builder append loop site; "
            f"found {builder_append_calls} builder append call sites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
