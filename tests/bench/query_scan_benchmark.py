from __future__ import annotations

import argparse
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time


def build(compiler: pathlib.Path, source: pathlib.Path,
          executable: pathlib.Path) -> None:
    result = subprocess.run([
        str(compiler), "build", "--root", str(source.parent), source.name,
        "-o", str(executable),
    ], capture_output=True, text=True, check=False)
    if result.returncode != 0 or not executable.is_file():
        raise RuntimeError(
            f"build failed for {source.name}: {result.stderr}")


def run(executable: pathlib.Path, rounds: int) -> tuple[float, str]:
    samples: list[float] = []
    output = ""

    def execute() -> tuple[float, str]:
        start = time.perf_counter()
        completed = subprocess.run(
            [str(executable)], capture_output=True, text=True, check=False)
        elapsed = time.perf_counter() - start
        if completed.returncode != 0:
            raise RuntimeError(
                f"execution failed: {completed.stderr}")
        return elapsed, completed.stdout.strip()

    _, output = execute()
    for _ in range(rounds):
        elapsed, output = execute()
        samples.append(elapsed)
    return statistics.median(samples), output


def program(values: str, rounds: int, query: bool) -> str:
    if query:
        body = (
            "        selected = QUERY item IN values\n"
            "            WHERE item % 2 = 0\n"
            "            SELECT item\n"
            "        END QUERY\n"
            "        FOR item IN selected\n"
            "            total = total + item\n"
            "        END FOR\n")
    else:
        body = (
            "        VAR selected = []\n"
            "        FOR item IN values\n"
            "            IF item % 2 = 0 THEN\n"
            "                selected = selected + [item]\n"
            "            END IF\n"
            "        END FOR\n"
            "        FOR item IN selected\n"
            "            total = total + item\n"
            "        END FOR\n")
    return (
        f"ACTION main()\n    values = {values}\n"
        "    VAR total = 0\n"
        f"    FOR round = 1 TO {rounds}\n"
        f"{body}"
        "    END FOR\n"
        "    print($\"checksum={total}\")\n"
        "END ACTION\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare a fused Query with an equivalent list-building loop.")
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--rows", type=int, default=256)
    parser.add_argument("--iterations", type=int, default=200,
                        help="complete scans performed by each executable")
    parser.add_argument("--rounds", type=int, default=10,
                        help="measured process runs; one warmup run is added")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    if arguments.rows < 1 or arguments.iterations < 1 or arguments.rounds < 1:
        parser.error("--rows, --iterations, and --rounds must be positive")

    values = "[" + ", ".join(str(value) for value in range(arguments.rows)) + "]"
    with tempfile.TemporaryDirectory(prefix="ombasic-query-scan-bench-") as temporary:
        root = pathlib.Path(temporary)
        query_source = root / "query.bas"
        loop_source = root / "loop.bas"
        query_source.write_text(
            program(values, arguments.iterations, True), encoding="utf-8")
        loop_source.write_text(
            program(values, arguments.iterations, False), encoding="utf-8")
        query_executable = root / ("query.exe" if sys.platform == "win32" else "query")
        loop_executable = root / ("loop.exe" if sys.platform == "win32" else "loop")
        build(compiler, query_source, query_executable)
        build(compiler, loop_source, loop_executable)
        query_seconds, query_output = run(query_executable, arguments.rounds)
        loop_seconds, loop_output = run(loop_executable, arguments.rounds)
        if query_output != loop_output:
            raise RuntimeError(
                f"checksum mismatch: Query={query_output!r}, loop={loop_output!r}")
        print(
            f"rows={arguments.rows} iterations={arguments.iterations} "
            f"runs={arguments.rounds} checksum={query_output.removeprefix('checksum=')} "
            f"query_median_ms={query_seconds * 1000:.3f} "
            f"hand_loop_median_ms={loop_seconds * 1000:.3f} "
            f"query_vs_loop={query_seconds / loop_seconds:.2f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
