from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import hashlib
import json


def main() -> int:
    cmake = pathlib.Path(sys.argv[1]).resolve()
    build_root = pathlib.Path(sys.argv[2]).resolve()
    repository = pathlib.Path(sys.argv[3]).resolve()
    configuration = sys.argv[4]
    c_compiler = pathlib.Path(sys.argv[5]).resolve()
    native_library_dir = pathlib.Path(sys.argv[6]).resolve()
    executable_name = "ombasic.exe" if sys.platform == "win32" else "ombasic"
    compiler_environment = os.environ.copy()
    compiler_environment.pop("OMBASIC_RESOURCE_DIR", None)

    with tempfile.TemporaryDirectory(prefix="ombasic-sdk-relocation-") as temporary:
        temporary_root = pathlib.Path(temporary)
        staged_sdk = temporary_root / "staged-sdk"
        moved_sdk = temporary_root / "moved-sdk"
        installed = subprocess.run(
            [str(cmake), "--install", str(build_root), "--config", configuration,
             "--prefix", str(staged_sdk)],
            capture_output=True, text=True, check=False)
        if installed.returncode != 0:
            raise AssertionError(
                f"SDK installation failed: {installed.stdout}\n{installed.stderr}")
        shutil.move(staged_sdk, moved_sdk)

        legacy_runtime_header = (moved_sdk / "include" / "ombasic" / "runtime"
                                 / "native_runtime.h")
        if legacy_runtime_header.exists():
            raise AssertionError("legacy native_runtime.h was installed")

        header_smoke = repository / "tests" / "runtime" \
            / "runtime_abi_header_smoke.c"
        if c_compiler.name.lower().startswith("clang-cl"):
            compiler_arguments = [
                str(c_compiler), "/nologo", "/clang:-std=c23",
                "/clang:-fsyntax-only", "/I", str(moved_sdk / "include"),
                str(header_smoke),
            ]
        else:
            compiler_arguments = [
                str(c_compiler), "-std=c23", "-fsyntax-only", "-I",
                str(moved_sdk / "include"), str(header_smoke),
            ]
        header_check = subprocess.run(compiler_arguments, cwd=temporary_root,
                                      capture_output=True, text=True, check=False)
        if header_check.returncode != 0:
            raise AssertionError(
                f"installed SDK C23 header smoke failed: "
                f"{header_check.stdout!r} {header_check.stderr!r}")

        compiler = moved_sdk / "bin" / executable_name
        version = subprocess.run([str(compiler), "version"], capture_output=True,
                                 text=True, check=False,
                                 env=compiler_environment)
        if version.returncode != 0 or "OMBasic" not in version.stdout:
            raise AssertionError(
                f"relocated compiler failed: {version.stdout!r} {version.stderr!r}")
        if (sys.platform == "win32"
                or configuration.lower() in {"release", "minsizerel"}):
            markers = []
            for path in (str(repository), str(build_root)):
                markers.extend((path.encode("utf-8"),
                                path.replace("/", "\\").encode("utf-8")))
            for sdk_file in moved_sdk.rglob("*"):
                if sdk_file.is_file():
                    contents = sdk_file.read_bytes()
                    if any(marker in contents for marker in markers):
                        raise AssertionError(
                            f"installed SDK file contains a development path: "
                            f"{sdk_file.relative_to(moved_sdk)}")

        source_root = temporary_root / "source"
        source_root.mkdir()
        (source_root / "main.bas").write_text(
            'ACTION main()\n    print("relocated SDK works")\nEND ACTION\n',
            encoding="utf-8", newline="\n")
        empty_resources = temporary_root / "empty-resources"
        empty_resources.mkdir()
        check_without_runtime = subprocess.run(
            [str(compiler), "check", "--root", str(source_root), "main.bas",
             "--resource-dir", str(empty_resources)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if check_without_runtime.returncode != 0:
            raise AssertionError(
                f"relocated check unexpectedly requires Runtime archives: "
                f"{check_without_runtime.stdout!r} "
                f"{check_without_runtime.stderr!r}")

        resource_environment = compiler_environment.copy()
        resource_environment["OMBASIC_RESOURCE_DIR"] = str(empty_resources)
        environment_override_output = temporary_root / (
            "environment-override.exe" if sys.platform == "win32"
            else "environment-override")
        environment_override = subprocess.run(
            [str(compiler), "build", "--root", str(source_root), "main.bas",
             "-o", str(environment_override_output)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=resource_environment)
        expected_runtime_archive = ("ombasic_runtime.lib" if sys.platform == "win32"
                                    else "libombasic_runtime.a")
        if (environment_override.returncode == 0
                or str(empty_resources) not in environment_override.stderr
                or expected_runtime_archive not in environment_override.stderr):
            raise AssertionError(
                f"resource environment override was not honored: "
                f"return={environment_override.returncode}, "
                f"stderr={environment_override.stderr!r}")

        explicit_override_output = temporary_root / (
            "explicit-override.exe" if sys.platform == "win32"
            else "explicit-override")
        explicit_override = subprocess.run(
            [str(compiler), "build", "--root", str(source_root), "main.bas",
             "--resource-dir", str(moved_sdk), "-o",
             str(explicit_override_output)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=resource_environment)
        if explicit_override.returncode != 0 or not explicit_override_output.is_file():
            raise AssertionError(
                f"explicit resource directory did not override the environment: "
                f"{explicit_override.stdout!r} {explicit_override.stderr!r}")
        explicit_override_ran = subprocess.run(
            [str(explicit_override_output)], cwd=temporary_root,
            capture_output=True, text=True, check=False)
        if (explicit_override_ran.returncode != 0
                or explicit_override_ran.stdout != "relocated SDK works\n"):
            raise AssertionError(
                f"explicit resource directory executable failed: "
                f"{explicit_override_ran.stdout!r} "
                f"{explicit_override_ran.stderr!r}")

        output = temporary_root / ("relocated-app.exe" if sys.platform == "win32"
                                   else "relocated-app")
        checked = subprocess.run(
            [str(compiler), "check", "--root", str(source_root), "main.bas"],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if checked.returncode != 0:
            raise AssertionError(
                f"relocated SDK check failed: {checked.stdout!r} {checked.stderr!r}")
        built = subprocess.run(
            [str(compiler), "build", "--root", str(source_root), "main.bas",
             "-o", str(output)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if built.returncode != 0 or not output.is_file():
            raise AssertionError(
                f"relocated SDK build failed: {built.stdout!r} {built.stderr!r}")
        ran = subprocess.run([str(output)], cwd=temporary_root,
                             capture_output=True, text=True, check=False)
        if ran.returncode != 0 or ran.stdout != "relocated SDK works\n":
            raise AssertionError(
                f"relocated output failed: {ran.stdout!r} {ran.stderr!r}")

        stdlib_source_root = temporary_root / "stdlib-source"
        stdlib_source_root.mkdir()
        (stdlib_source_root / "main.bas").write_text(
            'IMPORT "@std/math/math.bas"\n'
            'IMPORT "@std/text/text.bas"\n'
            'IMPORT "@std/json/json.bas"\n'
            "ACTION main()\n"
            "    sum = math.add(20, 22)\n"
            "    magnitude = math.abs(-7)\n"
            '    print($\"{sum},{magnitude},'
            '{math.min(sum, magnitude)},{math.max(sum, magnitude)},'
            '{math.clamp(100, 0, 10)},{math.clamp(5, 10, 0)}\")\n'
            '    scalar_length = text.length("é東京")\n'
            '    print($"{scalar_length}")\n'
            "    print(text.slice(\"aé東京z\", 1, 2))\n"
            '    SELECT json.parse("[1,2]")\n'
            '    CASE Ok(document)\n'
            '        SELECT json.array_length(document)\n'
            '        CASE Ok(count)\n'
            '            print($"json={count}")\n'
            '        CASE Error(problem)\n'
            '            print("json failed")\n'
            '        END SELECT\n'
            '    CASE Error(problem)\n'
            '        print("json parse failed")\n'
            '    END SELECT\n'
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        stdlib_check = subprocess.run(
            [str(compiler), "check", "--root", str(stdlib_source_root), "main.bas"],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if stdlib_check.returncode != 0:
            raise AssertionError(
                f"relocated SDK @std check failed: {stdlib_check.stdout!r} "
                f"{stdlib_check.stderr!r}")
        stdlib_output = temporary_root / (
            "relocated-stdlib.exe" if sys.platform == "win32" else "relocated-stdlib")
        stdlib_build = subprocess.run(
            [str(compiler), "build", "--root", str(stdlib_source_root), "main.bas",
             "-o", str(stdlib_output)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if stdlib_build.returncode != 0 or not stdlib_output.is_file():
            raise AssertionError(
                f"relocated SDK @std build failed: {stdlib_build.stdout!r} "
                f"{stdlib_build.stderr!r}")
        stdlib_run = subprocess.run(
            [str(stdlib_output)], cwd=temporary_root,
            capture_output=True, text=True, encoding="utf-8", check=False)
        if stdlib_run.returncode != 0 or stdlib_run.stdout != "42,7,7,42,10,5\n3\né東\njson=2\n":
            raise AssertionError(
                f"relocated SDK @std output mismatch: {stdlib_run.stdout!r} "
                f"{stdlib_run.stderr!r}")
        missing_stdlib = subprocess.run(
            [str(compiler), "check", "--root", str(stdlib_source_root), "main.bas",
             "--resource-dir", str(empty_resources)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if (missing_stdlib.returncode == 0
                or "SDK standard-library module" not in missing_stdlib.stderr
                or "@std/math/math.bas" not in missing_stdlib.stderr):
            raise AssertionError(
                "missing @std resource did not produce a precise SDK diagnostic: "
                f"{missing_stdlib.stdout!r} {missing_stdlib.stderr!r}")

        actor_output = temporary_root / (
            "relocated-actor.exe" if sys.platform == "win32" else "relocated-actor")
        actor_root = repository / "examples" / "actor"
        actor_check = subprocess.run(
            [str(compiler), "check", "--root", str(actor_root), "main.bas"],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if actor_check.returncode != 0:
            raise AssertionError(
                f"relocated SDK Actor check failed: "
                f"{actor_check.stdout!r} {actor_check.stderr!r}")
        actor_built = subprocess.run(
            [str(compiler), "build", "--root", str(actor_root), "main.bas",
             "-o", str(actor_output)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if actor_built.returncode != 0 or not actor_output.is_file():
            raise AssertionError(
                f"relocated SDK Actor build failed: "
                f"{actor_built.stdout!r} {actor_built.stderr!r}")
        actor_ran = subprocess.run(
            [str(actor_output)], cwd=temporary_root,
            capture_output=True, text=True, check=False)
        if actor_ran.returncode != 0 or actor_ran.stdout != "count=1\n":
            raise AssertionError(
                f"relocated Actor output failed: "
                f"{actor_ran.stdout!r} {actor_ran.stderr!r}")

        native_metadata = source_root / "native_math.omi"
        native_metadata.write_text(
            'NATIVE "native_math"\n'
            "FUNCTION add(left: CInt, right: CInt): CInt\n"
            '    SYMBOL "om_ffi_add"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "END NATIVE\n",
            encoding="utf-8", newline="\n")
        ffi_source = source_root / "ffi.bas"
        ffi_source.write_text(
            "ACTION main()\n"
            '    print($"ffi={add(40, 2)}")\n'
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        native_library = ("native_math.lib" if sys.platform == "win32"
                          else "libnative_math.a")
        native_library_source = native_library_dir / native_library
        if not native_library_source.is_file():
            raise AssertionError(
                f"Native FFI fixture library is missing: "
                f"{native_library_source}")
        isolated_native_library_dir = temporary_root / "native-libraries"
        isolated_native_library_dir.mkdir()
        shutil.copy2(native_library_source,
                     isolated_native_library_dir / native_library)
        ffi_output = temporary_root / (
            "relocated-ffi.exe" if sys.platform == "win32" else "relocated-ffi")
        ffi_built = subprocess.run(
            [str(compiler), "build", "--root", str(source_root), "ffi.bas",
             "--native", native_metadata.name, "-L",
             str(isolated_native_library_dir),
             "-o", str(ffi_output)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if ffi_built.returncode != 0 or not ffi_output.is_file():
            raise AssertionError(
                f"relocated SDK Native FFI build failed: "
                f"{ffi_built.stdout!r} {ffi_built.stderr!r}")
        ffi_ran = subprocess.run(
            [str(ffi_output)], cwd=temporary_root,
            capture_output=True, text=True, check=False)
        if ffi_ran.returncode != 0 or ffi_ran.stdout != "ffi=42\n":
            raise AssertionError(
                f"relocated Native FFI output failed: "
                f"{ffi_ran.stdout!r} {ffi_ran.stderr!r}")

        missing_runtime_output = temporary_root / (
            "missing-runtime.exe" if sys.platform == "win32"
            else "missing-runtime")
        missing_runtime = subprocess.run(
            [str(compiler), "build", "--root", str(source_root), "main.bas",
             "--resource-dir", str(empty_resources), "-o",
             str(missing_runtime_output)],
            cwd=temporary_root, capture_output=True, text=True, check=False,
            env=compiler_environment)
        if (missing_runtime.returncode == 0
                or "runtime resources unavailable for target" not in missing_runtime.stderr
                or expected_runtime_archive not in missing_runtime.stderr):
            raise AssertionError(
                f"missing Runtime archive diagnostic was not precise: "
                f"return={missing_runtime.returncode}, "
                f"stderr={missing_runtime.stderr!r}")

        manifest = moved_sdk / "share" / "ombasic" / "target-manifest.json"
        manifest_text = manifest.read_text(encoding="utf-8")
        if str(repository) in manifest_text or str(build_root) in manifest_text:
            raise AssertionError("SDK manifest contains a development-machine path")
        target_manifest = json.loads(manifest_text)
        stdlib_manifest_path = moved_sdk / target_manifest["stdlib"]["manifest"]
        stdlib_manifest = json.loads(stdlib_manifest_path.read_text(encoding="utf-8"))
        aggregate = hashlib.sha256()
        for entry in stdlib_manifest["files"]:
            relative = pathlib.PurePosixPath(entry["path"])
            if relative.is_absolute() or ".." in relative.parts:
                raise AssertionError("SDK standard-library manifest escapes its root")
            source_file = moved_sdk / "share" / "ombasic" / "stdlib" / pathlib.Path(*relative.parts)
            file_hash = hashlib.sha256(source_file.read_bytes()).hexdigest()
            if file_hash != entry["sha256"]:
                raise AssertionError(f"SDK standard-library hash mismatch: {entry['path']}")
            aggregate.update(entry["path"].encode("utf-8"))
            aggregate.update(b"\0")
            aggregate.update(file_hash.encode("ascii"))
            aggregate.update(b"\n")
        if (aggregate.hexdigest() != stdlib_manifest["content_sha256"]
                or aggregate.hexdigest() != target_manifest["stdlib"]["content_sha256"]
                or stdlib_manifest["ombasic_version"] != target_manifest["stdlib"]["version"]):
            raise AssertionError("SDK standard-library manifest identity is inconsistent")

        stdlib = moved_sdk / "share" / "ombasic" / "stdlib" / "math"
        facade = stdlib / "math.bas"
        implementation = stdlib / "integer.bas"
        if (not facade.is_file()
                or 'EXPORT IMPORT "./integer.bas"' not in facade.read_text(encoding="utf-8")
                or not implementation.is_file()):
            raise AssertionError("SDK standard-library source is missing or incomplete")

    print("PASS relocated SDK install, Hello World, Actor, and Native FFI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
