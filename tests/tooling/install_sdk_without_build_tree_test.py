from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


def resolve_command(value: str) -> pathlib.Path:
    command = shutil.which(value)
    if command is None:
        candidate = pathlib.Path(value)
        if candidate.is_file():
            command = str(candidate)
    if command is None:
        raise AssertionError(f"required command is unavailable: {value}")
    return pathlib.Path(command).resolve()


def read_cache_value(cache_file: pathlib.Path, name: str) -> str:
    prefix = f"{name}:"
    for line in cache_file.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            _, _, value = line.partition("=")
            return value
    raise AssertionError(f"CMake cache is missing {name}: {cache_file}")


def run(arguments: list[str], cwd: pathlib.Path,
        environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(arguments, cwd=cwd, env=environment,
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {arguments!r}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}")
    return result


def main() -> int:
    cmake = resolve_command(sys.argv[1])
    build_root = pathlib.Path(sys.argv[2]).resolve(strict=True)
    repository = pathlib.Path(sys.argv[3]).resolve(strict=True)
    configuration = sys.argv[4]
    cache_file = build_root / "CMakeCache.txt"
    if not cache_file.is_file():
        raise AssertionError(f"not a CMake build tree: {build_root}")
    cached_build_root = pathlib.Path(
        read_cache_value(cache_file, "CMAKE_CACHEFILE_DIR")).resolve(strict=True)
    cached_repository = pathlib.Path(
        read_cache_value(cache_file, "CMAKE_HOME_DIRECTORY")).resolve(strict=True)
    if os.path.normcase(str(cached_build_root)) != os.path.normcase(str(build_root)):
        raise AssertionError(
            f"build tree argument does not match CMake cache: {build_root}")
    if os.path.normcase(str(cached_repository)) != os.path.normcase(str(repository)):
        raise AssertionError(
            f"repository argument does not match CMake cache: {repository}")

    executable_name = "ombasic.exe" if sys.platform == "win32" else "ombasic"
    compiler_environment = os.environ.copy()
    compiler_environment.pop("OMBASIC_RESOURCE_DIR", None)
    original_working_directory = pathlib.Path.cwd()

    with tempfile.TemporaryDirectory(prefix="ombasic-sdk-no-build-tree-") as temporary:
        temporary_root = pathlib.Path(temporary)
        staged_sdk = temporary_root / "staged-sdk"
        moved_sdk = temporary_root / "moved-sdk"
        installed = subprocess.run(
            [str(cmake), "--install", str(build_root), "--config", configuration,
             "--prefix", str(staged_sdk)],
            cwd=repository, capture_output=True, text=True, check=False)
        if installed.returncode != 0:
            raise AssertionError(
                f"SDK installation failed: {installed.stdout}\n{installed.stderr}")
        staged_sdk.rename(moved_sdk)

        compiler = moved_sdk / "bin" / executable_name
        if not compiler.is_file():
            raise AssertionError(f"installed compiler is missing: {compiler}")

        quarantine_name = f"{build_root.name}.ombasic-sdk-hidden-{os.getpid()}"
        hidden_build_root = (build_root.parent / quarantine_name).resolve()
        if hidden_build_root.parent != build_root.parent or hidden_build_root.exists():
            raise AssertionError(
                f"unsafe or occupied build-tree quarantine path: {hidden_build_root}")

        source_root = temporary_root / "hello"
        source_root.mkdir()
        (source_root / "main.bas").write_text(
            'ACTION main()\n    print("SDK works without build tree")\n'
            'END ACTION\n', encoding="utf-8", newline="\n")

        actor_root = repository / "examples" / "actor"
        hello_output = temporary_root / (
            "hello.exe" if sys.platform == "win32" else "hello")
        actor_output = temporary_root / (
            "actor.exe" if sys.platform == "win32" else "actor")

        moved_build_tree = False
        try:
            os.chdir(temporary_root)
            build_root.rename(hidden_build_root)
            moved_build_tree = True
            if build_root.exists():
                raise AssertionError(
                    f"original build-tree path is still available: {build_root}")

            version = run([str(compiler), "version"], temporary_root,
                          compiler_environment)
            if "OMBasic" not in version.stdout:
                raise AssertionError(
                    f"installed compiler version output is unexpected: {version.stdout!r}")

            run([str(compiler), "check", "--root", str(source_root), "main.bas"],
                temporary_root, compiler_environment)
            run([str(compiler), "build", "--root", str(source_root), "main.bas",
                 "-o", str(hello_output)], temporary_root, compiler_environment)
            hello = run([str(hello_output)], temporary_root, compiler_environment)
            if hello.stdout != "SDK works without build tree\n":
                raise AssertionError(f"Hello World output mismatch: {hello.stdout!r}")

            run([str(compiler), "check", "--root", str(actor_root), "main.bas"],
                temporary_root, compiler_environment)
            run([str(compiler), "build", "--root", str(actor_root), "main.bas",
                 "-o", str(actor_output)], temporary_root, compiler_environment)
            actor = run([str(actor_output)], temporary_root, compiler_environment)
            if actor.stdout != "count=1\n":
                raise AssertionError(f"Actor output mismatch: {actor.stdout!r}")
        finally:
            try:
                if moved_build_tree:
                    if build_root.exists():
                        raise AssertionError(
                            f"build path unexpectedly reappeared: {build_root}")
                    hidden_build_root.rename(build_root)
            finally:
                os.chdir(original_working_directory)

    print("PASS installed SDK version/check/Hello World/Actor with build tree removed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
