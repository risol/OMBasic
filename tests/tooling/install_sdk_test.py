from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    cmake = pathlib.Path(sys.argv[1]).resolve()
    build_root = pathlib.Path(sys.argv[2]).resolve()
    repository = pathlib.Path(sys.argv[3]).resolve()
    configuration = sys.argv[4]
    executable_name = "ombasic.exe" if sys.platform == "win32" else "ombasic"

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

        compiler = moved_sdk / "bin" / executable_name
        version = subprocess.run([str(compiler), "version"], capture_output=True,
                                 text=True, check=False)
        if version.returncode != 0 or "OMBasic" not in version.stdout:
            raise AssertionError(
                f"relocated compiler failed: {version.stdout!r} {version.stderr!r}")
        if sys.platform == "win32":
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
        output = temporary_root / ("relocated-app.exe" if sys.platform == "win32"
                                   else "relocated-app")
        built = subprocess.run(
            [str(compiler), "build", "--root", str(source_root), "main.bas",
             "-o", str(output)],
            cwd=temporary_root, capture_output=True, text=True, check=False)
        if built.returncode != 0 or not output.is_file():
            raise AssertionError(
                f"relocated SDK build failed: {built.stdout!r} {built.stderr!r}")
        ran = subprocess.run([str(output)], cwd=temporary_root,
                             capture_output=True, text=True, check=False)
        if ran.returncode != 0 or ran.stdout != "relocated SDK works\n":
            raise AssertionError(
                f"relocated output failed: {ran.stdout!r} {ran.stderr!r}")

        manifest = moved_sdk / "share" / "ombasic" / "target-manifest.json"
        manifest_text = manifest.read_text(encoding="utf-8")
        if str(repository) in manifest_text or str(build_root) in manifest_text:
            raise AssertionError("SDK manifest contains a development-machine path")

    print("PASS relocated SDK install, build, and execution")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
