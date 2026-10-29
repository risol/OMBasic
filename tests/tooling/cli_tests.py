from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read_lsp_messages(data: bytes) -> list[dict]:
    messages = []
    cursor = 0
    while cursor < len(data):
        header_end = data.find(b"\r\n\r\n", cursor)
        require(header_end >= 0, "LSP response is missing its header terminator")
        headers = data[cursor:header_end].decode("ascii").split("\r\n")
        content_length = None
        for header in headers:
            name, separator, value = header.partition(":")
            if separator and name.lower() == "content-length":
                content_length = int(value.strip())
        require(content_length is not None, "LSP response has no Content-Length")
        body_start = header_end + 4
        body_end = body_start + content_length
        require(body_end <= len(data), "LSP response body is truncated")
        messages.append(json.loads(data[body_start:body_end].decode("utf-8")))
        cursor = body_end
    return messages


def lsp_message(message: dict) -> bytes:
    body = json.dumps(message, separators=(",", ":")).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def main() -> int:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(sys.argv[2]).resolve()
    debug_info_tool = pathlib.Path(sys.argv[3]).resolve()
    native_library_dir = pathlib.Path(sys.argv[4]).resolve()
    readobj_tool = debug_info_tool.with_name(
        "llvm-readobj.exe" if sys.platform == "win32" else "llvm-readobj")
    require(readobj_tool.is_file(),
            f"LLVM object inspection tool is missing: {readobj_tool}")

    def emit_llvm_ir(command_root: pathlib.Path, source_name: str,
                     output_name: str,
                     extra_arguments: tuple[str, ...] = ()) -> str:
        artifact = command_root / output_name
        emitted = subprocess.run([
            str(compiler), "build", "--root", str(command_root), source_name,
            *extra_arguments, "--emit=llvm-ir", "-o", str(artifact),
        ], capture_output=True, text=True, check=False)
        require(emitted.returncode == 0 and artifact.is_file(),
                f"LLVM IR emission failed for {source_name}: "
                f"return={emitted.returncode}, stderr={emitted.stderr!r}")
        return artifact.read_text(encoding="utf-8")

    version = subprocess.run([str(compiler), "version"], capture_output=True,
                             text=True, check=False)
    require(version.returncode == 0 and "OMBasic" in version.stdout,
            "version command failed")

    with tempfile.TemporaryDirectory(prefix="ombasic-commands-") as temporary:
        command_root = pathlib.Path(temporary)
        (command_root / "main.bas").write_text(
            "ACTION main()\n    print(\"Hello, OMBasic\")\nEND ACTION\n"
            "ACTION alternate()\n    print(\"Alternate\")\nEND ACTION\n"
            "ACTION strings()\n    amount = 42\n"
            "    print($\"value={amount}! {amount}\")\nEND ACTION\n"
            "FUNCTION plus_one(value)\n    RETURN value + 1\nEND FUNCTION\n"
            "FUNCTION apply_unary(callback, value)\n"
            "    RETURN callback(value)\nEND FUNCTION\n"
            "ACTION calls()\n"
            "    print($\"call={apply_unary(plus_one, 41)}\")\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        rootless_check = subprocess.run([
            str(compiler), "check", "main.bas",
        ], cwd=command_root, capture_output=True, text=True, check=False)
        require(rootless_check.returncode == 0,
                f"rootless check command failed: {rootless_check.stderr!r}")
        rootless_output = command_root / (
            "rootless-hello.exe" if sys.platform == "win32"
            else "rootless-hello")
        rootless_build = subprocess.run([
            str(compiler), "build", "main.bas", "-o", str(rootless_output),
        ], cwd=command_root, capture_output=True, text=True, check=False)
        require(rootless_build.returncode == 0 and rootless_output.is_file(),
                f"rootless build command failed: "
                f"return={rootless_build.returncode}, "
                f"stderr={rootless_build.stderr!r}")
        rootless_run = subprocess.run(
            [str(rootless_output)], cwd=command_root,
            capture_output=True, text=True, check=False)
        require(rootless_run.returncode == 0
                and rootless_run.stdout == "Hello, OMBasic\n",
                f"rootless executable failed: {rootless_run.stdout!r} "
                f"{rootless_run.stderr!r}")

        host_target = ("x86_64-pc-windows-msvc" if sys.platform == "win32"
                       else "x86_64-unknown-linux-gnu")
        native_target = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--target", host_target,
            "--resource-dir", str(native_library_dir),
        ], capture_output=True, text=True, check=False)
        require(native_target.returncode == 0
                and native_target.stdout == "Hello, OMBasic\n",
                f"host --target run failed: stdout={native_target.stdout!r}, "
                f"stderr={native_target.stderr!r}")

        unsupported_target = subprocess.run([
            str(compiler), "build", "--root", str(command_root), "main.bas",
            "--target", "x86_64-unknown-freebsd",
        ], capture_output=True, text=True, check=False)
        require(unsupported_target.returncode != 0
                and "E0701: unsupported target triple" in unsupported_target.stderr,
                "unsupported target did not produce its stable diagnostic: "
                f"{unsupported_target.stderr!r}")
        foreign_target = ("x86_64-unknown-linux-gnu" if sys.platform == "win32"
                          else "x86_64-pc-windows-msvc")
        foreign_target_run = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--target", foreign_target,
        ], capture_output=True, text=True, check=False)
        require(foreign_target_run.returncode != 0
                and "E0702: target runtime/sysroot unavailable" in foreign_target_run.stderr,
                "non-host run target was not rejected explicitly: "
                f"{foreign_target_run.stderr!r}")

        ran = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--resource-dir", str(native_library_dir),
        ], capture_output=True, text=True, check=False)
        require(ran.returncode == 0 and ran.stdout == "Hello, OMBasic\n",
                f"native run failed: stdout={ran.stdout!r}, stderr={ran.stderr!r}")
        emit_specs = (
            ("hir", ".hir", b"module "),
            ("mir", ".mir", b"function "),
            ("llvm-ir", ".ll", b"; ModuleID"),
            ("asm", ".s", b""),
            ("obj", ".obj", b""),
        )
        for emit_kind, extension, marker in emit_specs:
            artifact = command_root / ("hello" + extension)
            emitted = subprocess.run([
                str(compiler), "build", "--root", str(command_root),
                "main.bas", f"--emit={emit_kind}", "-o", str(artifact),
            ], capture_output=True, text=True, check=False)
            artifact_bytes = artifact.read_bytes() if artifact.is_file() else b""
            require(emitted.returncode == 0 and artifact_bytes
                    and (not marker or marker in artifact_bytes),
                    f"build --emit={emit_kind} failed: "
                    f"return={emitted.returncode}, "
                    f"stderr={emitted.stderr!r}")
            if emit_kind == "obj":
                inspected_object = subprocess.run([
                    str(readobj_tool), "--file-header", str(artifact),
                ], capture_output=True, text=True, check=False)
                expected_format = ("COFF-x86-64" if sys.platform == "win32"
                                   else "elf64-x86-64")
                require(inspected_object.returncode == 0
                        and expected_format in inspected_object.stdout,
                        "LLVM did not recognize the emitted native object: "
                        f"return={inspected_object.returncode}, "
                        f"output={inspected_object.stdout!r}, "
                        f"stderr={inspected_object.stderr!r}")

        (command_root / "checked_arithmetic.omi").write_text(
            'NATIVE "native_math"\n'
            "FUNCTION wide_value(): CInt64\n"
            '    SYMBOL "om_ffi_wide_value"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "END NATIVE\n",
            encoding="utf-8", newline="\n")
        (command_root / "checked_arithmetic.bas").write_text(
            "ACTION main()\n"
            "    number = wide_value()\n"
            "    sum_value = number + 1\n"
            '    print($"value={sum_value}")\n'
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        arithmetic_ir = emit_llvm_ir(command_root, "checked_arithmetic.bas",
            "checked-arithmetic.ll", ("--native", "checked_arithmetic.omi"))
        require("llvm.sadd.with.overflow.i64" in arithmetic_ir,
                "LLVM IR omitted the checked signed-add overflow intrinsic")

        (command_root / "short_circuit.bas").write_text(
            "FUNCTION fail_bool()\n"
            "    invalid = 1 / 0\n"
            "    RETURN TRUE\n"
            "END FUNCTION\n"
            "ACTION main()\n"
            "    and_value = FALSE AND fail_bool()\n"
            "    or_value = TRUE OR fail_bool()\n"
            '    print("short-circuit ok")\n'
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        short_circuit = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "short_circuit.bas",
        ], capture_output=True, text=True, check=False)
        require(short_circuit.returncode == 0
                and short_circuit.stdout == "short-circuit ok\n",
                "Boolean AND/OR evaluated a skipped operand: "
                f"return={short_circuit.returncode}, "
                f"stdout={short_circuit.stdout!r}, "
                f"stderr={short_circuit.stderr!r}")

        fingerprint_roots = [
            command_root / "fingerprint_same_a",
            command_root / "fingerprint_same_b",
            command_root / "fingerprint_changed",
            command_root / "fingerprint_protocol_changed",
        ]
        fingerprint_sources = (
            "ACTION main()\n"
            "    receiver = SPAWN Worker\n"
            "    SEND receiver, Ping(1)\n"
            "END ACTION\n"
            "MESSAGE Ping(value)\n"
            "ACTOR Worker\n"
            "    RECEIVE Ping(value)\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n")
        for fingerprint_root in fingerprint_roots:
            fingerprint_root.mkdir()
        (fingerprint_roots[0] / "main.bas").write_text(
            fingerprint_sources, encoding="utf-8", newline="\n")
        (fingerprint_roots[1] / "main.bas").write_text(
            fingerprint_sources, encoding="utf-8", newline="\n")
        (fingerprint_roots[2] / "main.bas").write_text(
            fingerprint_sources.replace("Ping(1)", 'Ping("one")'),
            encoding="utf-8", newline="\n")
        protocol_changed_source = (
            fingerprint_sources
            .replace("SEND receiver, Ping(1)",
                     "SEND receiver, Ping(1)\n"
                     "    SEND receiver, Pong(2)")
            .replace("MESSAGE Ping(value)\n",
                     "MESSAGE Ping(value)\nMESSAGE Pong(value)\n")
            .replace("    END RECEIVE\nEND ACTOR\n",
                     "    END RECEIVE\n"
                     "    RECEIVE Pong(value)\n"
                     "        STOP\n"
                     "    END RECEIVE\n"
                     "END ACTOR\n"))
        (fingerprint_roots[3] / "main.bas").write_text(
            protocol_changed_source, encoding="utf-8", newline="\n")

        def read_program_fingerprints(root: pathlib.Path,
                                      output_name: str) -> tuple[int, int]:
            ir = emit_llvm_ir(root, "main.bas", output_name)
            descriptor = re.search(
                r"@\.om_program_descriptor\s*=\s*private constant "
                r"%[^\{]+\{\s*i32\s+(\d+),\s*i64\s+(-?\d+),"
                r"\s*i64\s+(-?\d+)", ir)
            require(descriptor is not None,
                    "LLVM IR omitted Program/interface fingerprints")
            require(int(descriptor.group(1)) == 1,
                    "LLVM Program descriptor omitted Runtime ABI version")
            return int(descriptor.group(2)), int(descriptor.group(3))

        same_fingerprints_a = read_program_fingerprints(
            fingerprint_roots[0], "same-a.ll")
        same_fingerprints_b = read_program_fingerprints(
            fingerprint_roots[1], "same-b.ll")
        changed_fingerprints = read_program_fingerprints(
            fingerprint_roots[2], "changed.ll")
        protocol_changed_fingerprints = read_program_fingerprints(
            fingerprint_roots[3], "protocol-changed.ll")
        require(same_fingerprints_a == same_fingerprints_b,
                "canonical program fingerprints changed between identical builds")
        require(same_fingerprints_a[0] != same_fingerprints_a[1],
                "program and interface fingerprints were not separated")
        require(same_fingerprints_a[1] != changed_fingerprints[1],
                "Message field type changes did not change the interface fingerprint")
        require(same_fingerprints_a[1] != protocol_changed_fingerprints[1],
                "Actor protocol changes did not change the interface fingerprint")
        default_emit = subprocess.run([
            str(compiler), "build", "--root", str(command_root),
            "main.bas", "--emit=hir",
        ], cwd=command_root, capture_output=True, text=True, check=False)
        require(default_emit.returncode == 0
                and (command_root / "ombasic.hir").is_file(),
                "build --emit=hir did not write its default output path: "
                f"return={default_emit.returncode}, "
                f"stderr={default_emit.stderr!r}")
        alternate = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--entry", "alternate",
        ], capture_output=True, text=True, check=False)
        require(alternate.returncode == 0 and alternate.stdout == "Alternate\n",
                f"run --entry failed: stdout={alternate.stdout!r}, "
                f"stderr={alternate.stderr!r}")
        strings = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--entry", "strings",
        ], capture_output=True, text=True, check=False)
        require(strings.returncode == 0
                and strings.stdout == "value=42! 42\n",
                f"managed string runtime failed: stdout={strings.stdout!r}, "
                f"stderr={strings.stderr!r}")
        calls = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--entry", "calls",
        ], capture_output=True, text=True, check=False)
        require(calls.returncode == 0 and calls.stdout == "call=42\n",
                f"hidden context call ABI failed: stdout={calls.stdout!r}, "
                f"stderr={calls.stderr!r}")

        (command_root / "native_math.omi").write_text(
            'NATIVE "native_math"\n'
            "FUNCTION add(left: CInt, right: CInt): CInt\n"
            '    SYMBOL "om_ffi_add"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION cstring_length(value: CString BORROWED): CInt\n"
            '    SYMBOL "om_ffi_cstring_length"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION add_i8(left: CInt8, right: CInt8): CInt8\n"
            '    SYMBOL "om_ffi_add_i8"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION add_u8(left: CUInt8, right: CUInt8): CUInt8\n"
            '    SYMBOL "om_ffi_add_u8"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION echo_cstring(value: CString BORROWED): CString\n"
            '    SYMBOL "om_ffi_echo_cstring"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "    RETURNS BORROWED FROM value\n"
            "END FUNCTION\n"
            "FUNCTION duplicate_cstring(value: CString BORROWED): CString\n"
            '    SYMBOL "om_ffi_duplicate_cstring"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "    RETURNS OWNED WITH release_cstring\n"
            "END FUNCTION\n"
            "FUNCTION release_cstring(value: CPointer OWNED): CVoid\n"
            '    SYMBOL "om_ffi_free_cstring"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION make_resource(value: CInt32): CPointer\n"
            '    SYMBOL "om_ffi_make_resource"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "    RETURNS OWNED WITH release_resource\n"
            "END FUNCTION\n"
            "FUNCTION static_resource(): CPointer\n"
            '    SYMBOL "om_ffi_static_resource"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "    RETURNS STATIC\n"
            "END FUNCTION\n"
            "FUNCTION borrow_resource(value: CPointer BORROWED): CPointer\n"
            '    SYMBOL "om_ffi_borrow_resource"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "    RETURNS BORROWED FROM value\n"
            "END FUNCTION\n"
            "FUNCTION read_resource(value: CPointer BORROWED): CInt32\n"
            '    SYMBOL "om_ffi_read_resource"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION consume_resource(value: CPointer OWNED): CInt32\n"
            '    SYMBOL "om_ffi_consume_resource"\n'
            "    EFFECT NATIVE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION release_resource(value: CPointer OWNED): CVoid\n"
            '    SYMBOL "om_ffi_release_resource"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "NATIVE RECORD C_Point\n"
            "    x: CDouble\n"
            "    y: CDouble\n"
            "END NATIVE RECORD\n"
            "NATIVE RECORD C_Mixed\n"
            "    integer: CInt32\n"
            "    real: CFloat\n"
            "END NATIVE RECORD\n"
            "NATIVE RECORD C_Floats\n"
            "    x: CFloat\n"
            "    y: CFloat\n"
            "END NATIVE RECORD\n"
            "NATIVE RECORD C_Large\n"
            "    first: CSize\n"
            "    second: CSize\n"
            "    third: CSize\n"
            "END NATIVE RECORD\n"
            "NATIVE RECORD C_Handle\n"
            "    pointer: CPointer STATIC\n"
            "    length: CUInt32\n"
            "END NATIVE RECORD\n"
            "NATIVE RECORD Cpp_Point\n"
            "    x: CDouble\n"
            "    y: CDouble\n"
            "END NATIVE RECORD\n"
            "FUNCTION make_cpp_point(): Cpp_Point\n"
            '    SYMBOL "om_ffi_make_cpp_point"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION take_cpp_point(value: Cpp_Point): CInt\n"
            '    SYMBOL "om_ffi_take_cpp_point"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION make_point(): C_Point\n"
            '    SYMBOL "om_ffi_make_point"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION take_point(value: C_Point): CInt\n"
            '    SYMBOL "om_ffi_take_point"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION make_mixed(): C_Mixed\n"
            '    SYMBOL "om_ffi_make_mixed"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION take_mixed(value: C_Mixed): CInt\n"
            '    SYMBOL "om_ffi_take_mixed"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION make_floats(): C_Floats\n"
            '    SYMBOL "om_ffi_make_floats"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION take_floats(value: C_Floats): CInt\n"
            '    SYMBOL "om_ffi_take_floats"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION make_large(): C_Large\n"
            '    SYMBOL "om_ffi_make_large"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION take_large(value: C_Large): CInt\n"
            '    SYMBOL "om_ffi_take_large"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION make_handle(): C_Handle\n"
            '    SYMBOL "om_ffi_make_handle"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION take_handle(value: C_Handle): CInt\n"
            '    SYMBOL "om_ffi_take_handle"\n'
            "    EFFECT PURE\n"
            "    NONBLOCKING\n"
            "END FUNCTION\n"
            "FUNCTION blocking_add(left: CInt, right: CInt): CInt\n"
            '    SYMBOL "om_ffi_blocking_add"\n'
            "    EFFECT NATIVE\n"
            "    BLOCKING\n"
            "END FUNCTION\n"
            "END NATIVE\n",
            encoding="utf-8", newline="\n")
        (command_root / "blocking_native.bas").write_text(
            "MESSAGE Ping()\n"
            "ACTOR Worker\n"
            "    RECEIVE Ping()\n"
            "        value = blocking_add(1, 2)\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    actor_ref = SPAWN Worker\n"
            "    SEND actor_ref, Ping()\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        blocked_native = subprocess.run([
            str(compiler), "build", "--root", str(command_root),
            "blocking_native.bas", "--native", "native_math.omi",
            "--emit=mir",
        ], capture_output=True, text=True, check=False)
        require(blocked_native.returncode != 0
                and "E1104" in blocked_native.stderr,
                "blocking Native call in RECEIVE was not rejected: "
                f"return={blocked_native.returncode}, "
                f"stderr={blocked_native.stderr!r}")

        callback_metadata = command_root / "callback_fixture.omi"
        callback_source = command_root / "callback_fixture.bas"

        def check_callback_fixture(callback_types: str, message_name: str,
                                   source: str) -> tuple[subprocess.CompletedProcess,
                                                        list[dict]]:
            callback_metadata.write_text(
                'NATIVE "callback_fixture"\n'
                "FUNCTION subscribe(callback: CALLBACK(" + callback_types
                + ") -> CVoid): CVoid\n"
                '    SYMBOL "callback_subscribe"\n'
                "    EFFECT NATIVE\n"
                "    NONBLOCKING\n"
                "    CALLBACK callback FOREIGN_THREAD MESSAGE " + message_name
                + "\nEND FUNCTION\nEND NATIVE\n",
                encoding="utf-8", newline="\n")
            callback_source.write_text(source, encoding="utf-8", newline="\n")
            checked = subprocess.run([
                str(compiler), "check", "--root", str(command_root),
                "callback_fixture.bas", "--native", "callback_fixture.omi",
                "--diagnostics=json",
            ], capture_output=True, text=True, check=False)
            try:
                diagnostics = json.loads(checked.stdout)["diagnostics"]
            except (json.JSONDecodeError, KeyError) as error:
                raise AssertionError(
                    "callback semantic check did not return JSON diagnostics: "
                    f"return={checked.returncode}, stdout={checked.stdout!r}, "
                    f"stderr={checked.stderr!r}") from error
            return checked, diagnostics

        callback_receive_source = (
            "MESSAGE Start()\n"
            "MESSAGE Tick(value)\n"
            "ACTOR Listener\n"
            "    RECEIVE Start()\n"
            "        subscribe()\n"
            "    END RECEIVE\n"
            "    RECEIVE Tick(value)\n"
            "        retained = value\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    receiver_ref = SPAWN Listener\n"
            "    SEND receiver_ref, Start()\n"
            "END ACTION\n")
        valid_callback, callback_diagnostics = check_callback_fixture(
            "CInt", "Tick", callback_receive_source)
        require(valid_callback.returncode == 0 and not callback_diagnostics,
                "matching callback Message binding was rejected: "
                f"return={valid_callback.returncode}, "
                f"diagnostics={callback_diagnostics!r}, "
                f"stderr={valid_callback.stderr!r}")

        callback_message_not_received, callback_diagnostics = \
            check_callback_fixture(
                "CInt", "Tick",
                "MESSAGE Start()\n"
                "MESSAGE Tick(value)\n"
                "ACTOR Listener\n"
                "    RECEIVE Start()\n"
                "        subscribe()\n"
                "    END RECEIVE\n"
                "END ACTOR\n"
                "ACTION main()\n"
                "    receiver_ref = SPAWN Listener\n"
                "    SEND receiver_ref, Start()\n"
                "END ACTION\n")
        require(callback_message_not_received.returncode != 0
                and any(item["code"] == "E0403"
                        for item in callback_diagnostics),
                "callback binding to a Message without a current Actor "
                "RECEIVE was not rejected: "
                f"return={callback_message_not_received.returncode}, "
                f"diagnostics={callback_diagnostics!r}")

        missing_callback_message, callback_diagnostics = check_callback_fixture(
            "CInt", "Missing", callback_receive_source)
        require(missing_callback_message.returncode != 0
                and any(item["code"] == "E0403"
                        for item in callback_diagnostics),
                "missing callback Message was not diagnosed with E0403: "
                f"return={missing_callback_message.returncode}, "
                f"diagnostics={callback_diagnostics!r}")

        wrong_callback_arity, callback_diagnostics = check_callback_fixture(
            "CInt, CDouble", "Tick", callback_receive_source)
        require(wrong_callback_arity.returncode != 0
                and any(item["code"] == "E0403"
                        for item in callback_diagnostics),
                "callback Message field count mismatch was not diagnosed: "
                f"return={wrong_callback_arity.returncode}, "
                f"diagnostics={callback_diagnostics!r}")

        unsupported_callback_pointer, callback_diagnostics = \
            check_callback_fixture("CPointer", "Tick",
                                   callback_receive_source)
        require(unsupported_callback_pointer.returncode != 0
                and any(item["code"] == "E0403"
                        for item in callback_diagnostics),
                "unsupported Native pointer callback payload was not "
                f"diagnosed: {callback_diagnostics!r}")

        callback_outside_receive, callback_diagnostics = check_callback_fixture(
            "CInt", "Tick",
            "MESSAGE Tick(value)\n"
            "ACTION main()\n"
            "    subscribe()\n"
            "END ACTION\n")
        require(callback_outside_receive.returncode != 0
                and any(item["code"] == "E0403"
                        for item in callback_diagnostics),
                "callback registration outside Actor RECEIVE was not rejected: "
                f"return={callback_outside_receive.returncode}, "
                f"diagnostics={callback_diagnostics!r}")

        callback_field_mismatch, callback_diagnostics = check_callback_fixture(
            "CInt", "Tick",
            callback_receive_source.replace(
                "    SEND receiver_ref, Start()\n",
                '    SEND receiver_ref, Start()\n'
                '    SEND receiver_ref, Tick("text")\n'))
        require(callback_field_mismatch.returncode != 0
                and any(item["code"] == "E0401"
                        for item in callback_diagnostics),
                "callback ABI type did not constrain its Message field: "
                f"return={callback_field_mismatch.returncode}, "
                f"diagnostics={callback_diagnostics!r}")

        callback_metadata.write_text(
            'NATIVE "native_math"\n'
            "FUNCTION subscribe(callback: CALLBACK(CInt8, CUInt8, CFloat, "
            "CBool) -> CVoid): CInt\n"
            '    SYMBOL "om_ffi_invoke_callback_on_thread"\n'
            "    EFFECT NATIVE\n"
            "    NONBLOCKING\n"
            "    CALLBACK callback FOREIGN_THREAD MESSAGE Tick\n"
            "END FUNCTION\nEND NATIVE\n",
            encoding="utf-8", newline="\n")
        callback_source.write_text(
            "MESSAGE Start()\n"
            "MESSAGE Tick(signed_value, unsigned_value, float_value, "
            "boolean_value)\n"
            "ACTOR Listener\n"
            "    RECEIVE Start()\n"
            "        status = subscribe()\n"
            '        print($"subscribe={status}")\n'
            "    END RECEIVE\n"
            "    RECEIVE Tick(signed_value, unsigned_value, float_value, "
            "boolean_value)\n"
            '        print($"callback={signed_value},{unsigned_value},'
            '{float_value},{boolean_value}")\n'
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    first_listener = SPAWN Listener\n"
            "    second_listener = SPAWN Listener\n"
            "    SEND first_listener, Start()\n"
            "    SEND second_listener, Start()\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        native_callback = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "callback_fixture.bas", "--native", "callback_fixture.omi", "-L",
            str(native_library_dir),
        ], capture_output=True, text=True, check=False)
        require(native_callback.returncode == 0
                and native_callback.stdout.count("subscribe=0") == 2
                and native_callback.stdout.count(
                    "callback=-7,250,1.25,true") == 2,
                "foreign-thread Native callback did not reach its Actor Message: "
                f"return={native_callback.returncode}, "
                f"stdout={native_callback.stdout!r}, "
                f"stderr={native_callback.stderr!r}")

        (command_root / "native_ffi.bas").write_text(
            "ACTION main()\n"
            '    print($"answer={add(20, 22)}, negative={add(0 - 20, 0 - 22)}, '
            'i8={add_i8(0 - 10, 0 - 20)}, u8={add_u8(200, 55)}")\n'
            '    print(echo_cstring("borrowed text"))\n'
            '    print(duplicate_cstring("owned text"))\n'
            "END ACTION\n"
            "ACTION cstring_loop()\n"
            "    VAR total = 0\n"
            "    FOR index = 1 TO 100000\n"
            '        total = total + cstring_length("stack restore")\n'
            "    END FOR\n"
            '    print($"loop={total}")\n'
            "END ACTION\n"
            "ACTION embedded_nul()\n"
            '    print(echo_cstring("a\\0b"))\n'
            "END ACTION\n"
            "ACTION native_records()\n"
            '    print($"point={take_point(make_point())}, '
            'mixed={take_mixed(make_mixed())}, '
            'floats={take_floats(make_floats())}, '
            'large={take_large(make_large())}, '
            'handle={take_handle(make_handle())}, '
            'cpp-point={take_cpp_point(make_cpp_point())}")\n'
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        native_ffi = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "native_ffi.bas", "--native", "native_math.omi", "-L",
            str(native_library_dir),
        ], capture_output=True, text=True, check=False)
        require(native_ffi.returncode == 0
                and native_ffi.stdout
                == "answer=42, negative=-42, i8=-30, u8=255\n"
                "borrowed text\nowned text\n",
                "Native CInt FFI call failed: "
                f"stdout={native_ffi.stdout!r}, stderr={native_ffi.stderr!r}")
        native_ffi_ir = emit_llvm_ir(command_root, "native_ffi.bas",
            "native-ffi.ll", ("--native", "native_math.omi"))
        require("@om_ffi_add" in native_ffi_ir,
                "Native external declaration is missing from LLVM IR")
        native_records = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "native_ffi.bas", "--native", "native_math.omi", "-L",
            str(native_library_dir), "--entry", "native_records",
        ], capture_output=True, text=True, check=False)
        require(native_records.returncode == 0
                and native_records.stdout
                == "point=1, mixed=1, floats=1, large=1, handle=1, "
                   "cpp-point=1\n",
                "Native Record ABI roundtrip failed: "
                f"return={native_records.returncode}, "
                f"stdout={native_records.stdout!r}, "
                f"stderr={native_records.stderr!r}")
        (command_root / "native_ffi.bas").write_text(
            (command_root / "native_ffi.bas").read_text(encoding="utf-8")
            + "ACTION native_pointers()\n"
            "    resource = make_resource(42)\n"
            "    borrowed = borrow_resource(resource)\n"
            '    print($"borrowed={read_resource(borrowed)}")\n'
            '    print($"static={read_resource(static_resource())}")\n'
            '    print($"consumed={consume_resource(resource)}")\n'
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        native_pointers = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "native_ffi.bas", "--native", "native_math.omi", "-L",
            str(native_library_dir), "--entry", "native_pointers",
        ], capture_output=True, text=True, check=False)
        require(native_pointers.returncode == 0
                and native_pointers.stdout
                == "borrowed=42\nstatic=77\nconsumed=42\n",
                "CPointer borrowing/ownership transfer failed: "
                f"return={native_pointers.returncode}, "
                f"stdout={native_pointers.stdout!r}, "
                f"stderr={native_pointers.stderr!r}")
        native_ffi_loop = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "native_ffi.bas", "--native", "native_math.omi", "-L",
            str(native_library_dir), "--entry", "cstring_loop",
        ], capture_output=True, text=True, check=False)
        require(native_ffi_loop.returncode == 0
                and native_ffi_loop.stdout == "loop=1300000\n",
                "repeated CString calls did not restore their stack space: "
                f"return={native_ffi_loop.returncode}, "
                f"stdout={native_ffi_loop.stdout!r}, "
                f"stderr={native_ffi_loop.stderr!r}")
        native_ffi_nul = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "native_ffi.bas", "--native", "native_math.omi", "-L",
            str(native_library_dir), "--entry", "embedded_nul",
        ], capture_output=True, text=True, check=False)
        require(native_ffi_nul.returncode == 70
                and native_ffi_nul.stdout == ""
                and "runtime operation failed" in native_ffi_nul.stderr,
                "CString conversion did not reject an embedded NUL: "
                f"return={native_ffi_nul.returncode}, "
                f"stdout={native_ffi_nul.stdout!r}, "
                f"stderr={native_ffi_nul.stderr!r}")

        debug_executable = command_root / (
            "debug-info.exe" if sys.platform == "win32" else "debug-info")
        debug_build = subprocess.run([
            str(compiler), "build", "--root", str(command_root), "main.bas",
            "-o", str(debug_executable),
        ], capture_output=True, text=True, check=False)
        require(debug_build.returncode == 0 and debug_executable.is_file(),
                f"debug-info native build failed: {debug_build.stderr!r}")
        source_path = (command_root / "main.bas").resolve()
        if sys.platform == "win32":
            pdb_path = pathlib.Path(str(debug_executable) + ".pdb")
            require(pdb_path.is_file(),
                    f"native debug build did not create {pdb_path}")
            debug_dump = subprocess.run([
                str(debug_info_tool), "dump", "-files", "-l", "-modi=0",
                str(pdb_path),
            ], capture_output=True, text=True, check=False)
            source_in_pdb = str(source_path).lower() in debug_dump.stdout.lower()
            line_in_pdb = re.search(r"(?m)^\s+2\s+000000", debug_dump.stdout)
            require(debug_dump.returncode == 0 and source_in_pdb
                    and line_in_pdb is not None,
                    "Windows PDB is missing the OMBasic source path or line 2: "
                    f"return={debug_dump.returncode}, "
                    f"stderr={debug_dump.stderr!r}, "
                    f"output={debug_dump.stdout[:2000]!r}")
        else:
            debug_dump = subprocess.run([
                str(debug_info_tool), "--debug-line", str(debug_executable),
            ], capture_output=True, text=True, check=False)
            require(debug_dump.returncode == 0
                    and source_path.name in debug_dump.stdout
                    and re.search(r"(?m)0x[0-9a-fA-F]+\s+2\s+", debug_dump.stdout),
                    "Linux DWARF line table is missing the source file or line 2: "
                    f"return={debug_dump.returncode}, "
                    f"stderr={debug_dump.stderr!r}, "
                    f"output={debug_dump.stdout[:2000]!r}")

        (command_root / "actors.bas").write_text(
            "MESSAGE Ping(value)\n"
            "ACTOR Counter\n"
            "    VAR count = 5\n"
            "    VAR initial_count = count + 2\n"
            "    RECEIVE Ping(value)\n"
            "        count = count + value + initial_count\n"
            "        print($\"count={count}\")\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION spawn_remote(node)\n"
            "    remote_ref = SPAWN Counter AT node\n"
            "END ACTION\n"
            "ACTION main()\n"
            "    counter_ref = SPAWN Counter\n"
            "    SEND counter_ref, Ping(3)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        actors = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "actors.bas",
        ], capture_output=True, text=True, check=False)
        require(actors.returncode == 0 and actors.stdout == "count=15\n",
                f"native actor runtime failed: return={actors.returncode}, "
                f"stdout={actors.stdout!r}, stderr={actors.stderr!r}")
        actors_ir = emit_llvm_ir(command_root, "actors.bas", "actors.ll")
        require("@om_runtime_actor_send_id" in actors_ir
                and re.search(r"@\.om_actor_\d+\s*=\s*", actors_ir),
                "Actor send or its generated descriptor is missing from LLVM IR")

        (command_root / "actor_self.bas").write_text(
            "MESSAGE Start(worker)\n"
            "MESSAGE Run(reply_to)\n"
            "MESSAGE Done()\n"
            "ACTOR Supervisor\n"
            "    RECEIVE Start(peer)\n"
            "        SEND peer, Run(SELF)\n"
            "    END RECEIVE\n"
            "    RECEIVE Done()\n"
            "        print(\"done\")\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTOR Worker\n"
            "    RECEIVE Run(reply_to)\n"
            "        SEND reply_to, Done()\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    supervisor_ref = SPAWN Supervisor\n"
            "    worker_ref = SPAWN Worker\n"
            "    SEND supervisor_ref, Start(worker_ref)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        actor_self = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "actor_self.bas",
        ], capture_output=True, text=True, check=False)
        require(actor_self.returncode == 0 and actor_self.stdout == "done\n",
                f"native SELF/send roundtrip failed: "
                f"return={actor_self.returncode}, stdout={actor_self.stdout!r}, "
                f"stderr={actor_self.stderr!r}")

        (command_root / "actor_self_send.bas").write_text(
            "MESSAGE Tick(remaining)\n"
            "ACTOR Counter\n"
            "    VAR count = 0\n"
            "    RECEIVE Tick(remaining)\n"
            "        count = count + 1\n"
            "        IF remaining > 0 THEN\n"
            "            SEND SELF, Tick(remaining - 1)\n"
            "        ELSE\n"
            "            print($\"ticks={count}\")\n"
            "            STOP\n"
            "        END IF\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    counter_ref = SPAWN Counter\n"
            "    SEND counter_ref, Tick(3)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        actor_self_send = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "actor_self_send.bas",
        ], capture_output=True, text=True, check=False)
        require(actor_self_send.returncode == 0
                and actor_self_send.stdout == "ticks=4\n",
                f"native actor self-send failed: "
                f"return={actor_self_send.returncode}, "
                f"stdout={actor_self_send.stdout!r}, "
                f"stderr={actor_self_send.stderr!r}")

        (command_root / "actor_state_isolation.bas").write_text(
            "MESSAGE Increment()\n"
            "ACTOR Counter\n"
            "    VAR count = 0\n"
            "    RECEIVE Increment()\n"
            "        count = count + 1\n"
            "        print($\"count={count}\")\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    first = SPAWN Counter\n"
            "    second = SPAWN Counter\n"
            "    SEND first, Increment()\n"
            "    SEND second, Increment()\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        actor_state_isolation = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "actor_state_isolation.bas",
        ], capture_output=True, text=True, check=False)
        require(actor_state_isolation.returncode == 0
                and actor_state_isolation.stdout.count("count=1") == 2
                and "count=2" not in actor_state_isolation.stdout,
                f"native actor states were not isolated: "
                f"return={actor_state_isolation.returncode}, "
                f"stdout={actor_state_isolation.stdout!r}, "
                f"stderr={actor_state_isolation.stderr!r}")

        system_file_path = (command_root / "system-actor.txt").as_posix()
        (command_root / "system_actors.bas").write_text(
            "MESSAGE Start()\n"
            "MESSAGE Tick(value)\n"
            "ACTOR File_Client\n"
            "    RECEIVE Start()\n"
            f'        SEND File_System, File_Write("{system_file_path}", '
            '"native file ✓", SELF)\n'
            "    END RECEIVE\n"
            "    RECEIVE File_Write_Completed(outcome)\n"
            "        written = TRY outcome\n"
            f'        SEND File_System, File_Read("{system_file_path}", SELF)\n'
            "    END RECEIVE\n"
            "    RECEIVE File_Read_Completed(outcome)\n"
            "        contents = TRY outcome\n"
            "        print(contents)\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTOR Timer_Client\n"
            "    RECEIVE Tick(value)\n"
            "        print(value)\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    file_actor = SPAWN File_Client\n"
            "    timer_actor = SPAWN Timer_Client\n"
            "    SEND file_actor, Start()\n"
            '    SEND Timer, Timer_After(5, Tick("timer fired"), '
            "timer_actor)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        system_actors = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "system_actors.bas",
        ], capture_output=True, text=True, encoding="utf-8", check=False)
        require(system_actors.returncode == 0
                and "native file ✓" in system_actors.stdout
                and "timer fired\n" in system_actors.stdout
                and (command_root / "system-actor.txt").read_text(
                    encoding="utf-8") == "native file ✓",
                f"native File_System/Timer actors failed: "
                f"return={system_actors.returncode}, "
                f"stdout={system_actors.stdout!r}, "
                f"stderr={system_actors.stderr!r}")

        missing_file = (command_root / "missing-system-actor.txt").as_posix()
        (command_root / "system_actor_try_failure.bas").write_text(
            "MESSAGE Start()\n"
            "MESSAGE Observe(target)\n"
            "ACTOR Monitor\n"
            "    RECEIVE Observe(target)\n"
            "        WATCH target\n"
            "        SEND target, Start()\n"
            "    END RECEIVE\n"
            "    RECEIVE Actor_Down(ended_id, reason)\n"
            "        IF reason = Actor_Down_Reason.failed THEN\n"
            '            print("try marked failed")\n'
            "        ELSE\n"
            '            print("wrong failure reason")\n'
            "        END IF\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTOR Reader\n"
            "    RECEIVE Start()\n"
            f'        SEND File_System, File_Read("{missing_file}", SELF)\n'
            "    END RECEIVE\n"
            "    RECEIVE File_Read_Completed(outcome)\n"
            "        contents = TRY outcome\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    monitor_ref = SPAWN Monitor\n"
            "    reader_ref = SPAWN Reader\n"
            "    SEND monitor_ref, Observe(reader_ref)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        system_actor_try_failure = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "system_actor_try_failure.bas",
        ], capture_output=True, text=True, encoding="utf-8", check=False)
        require(system_actor_try_failure.returncode == 0
                and system_actor_try_failure.stdout == "try marked failed\n",
                f"TRY in a Receive did not fail the Actor: "
                f"return={system_actor_try_failure.returncode}, "
                f"stdout={system_actor_try_failure.stdout!r}, "
                f"stderr={system_actor_try_failure.stderr!r}")

        (command_root / "actor_watch.bas").write_text(
            "MESSAGE Observe(target)\n"
            "MESSAGE Ping()\n"
            "ACTOR Watcher\n"
            "    RECEIVE Observe(target)\n"
            "        WATCH target\n"
            "    END RECEIVE\n"
            "    RECEIVE Actor_Down(ended_id, cause)\n"
            "        IF cause = Actor_Down_Reason.failed THEN\n"
            "            print(\"failed\")\n"
            "        ELSE\n"
            "            print(\"wrong reason\")\n"
            "        END IF\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTOR Worker\n"
            "    RECEIVE Ping()\n"
            "        value = 1 / 0\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    watcher_ref = SPAWN Watcher\n"
            "    worker_ref = SPAWN Worker\n"
            "    SEND watcher_ref, Observe(worker_ref)\n"
            "    SEND worker_ref, Ping()\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        actor_watch = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "actor_watch.bas",
        ], capture_output=True, text=True, check=False)
        require(actor_watch.returncode == 0
                and actor_watch.stdout == "failed\n",
                f"Actor watch/Actor_Down failed: return={actor_watch.returncode}, "
                f"stdout={actor_watch.stdout!r}, stderr={actor_watch.stderr!r}")

        (command_root / "actor_initializer_fault.bas").write_text(
            "MESSAGE Ping()\n"
            "ACTOR Broken\n"
            "    VAR count = 9223372036854775807 + 1\n"
            "    RECEIVE Ping()\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            "    broken_ref = SPAWN Broken\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        actor_initializer_fault = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "actor_initializer_fault.bas",
        ], capture_output=True, text=True, check=False)
        require(actor_initializer_fault.returncode == 70
                and "runtime fault:" in actor_initializer_fault.stderr,
                "Actor state initializer fault escaped its runtime boundary: "
                f"return={actor_initializer_fault.returncode}, "
                f"stderr={actor_initializer_fault.stderr!r}")

        result_first = "R" * 40000
        result_second = "S" * 40000
        (command_root / "result_try.bas").write_text(
            "FUNCTION relay(flag)\n"
            "    VAR pending = Ok(\"seed\")\n"
            "    IF flag THEN\n"
            f"        pending = Ok(\"{result_first}\")\n"
            "    ELSE\n"
            "        pending = Error(\"propagated\")\n"
            "    END IF\n"
            f"    discarded = \"{result_second}\"\n"
            "    value = TRY pending\n"
            "    RETURN Ok(value)\n"
            "END FUNCTION\n"
            "ACTION main()\n"
            "    success = relay(TRUE)\n"
            "    SELECT success\n"
            "    CASE Ok(success_value)\n"
            "        print(success_value)\n"
            "    CASE Error(success_error)\n"
            "        print(success_error)\n"
            "    CASE ELSE\n"
            "        print(\"unexpected\")\n"
            "    END SELECT\n"
            "    failure = relay(FALSE)\n"
            "    SELECT failure\n"
            "    CASE Ok(failure_value)\n"
            "        print(\"unexpected\")\n"
            "    CASE Error(failure_error)\n"
            "        print(failure_error)\n"
            "    CASE ELSE\n"
            "        print(\"unexpected\")\n"
            "    END SELECT\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        result_try = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "result_try.bas",
        ], capture_output=True, text=True, check=False)
        require(result_try.returncode == 0
                and result_try.stdout == f"{result_first}\npropagated\n",
                "Result/TRY failed to propagate an Error or root a managed "
                f"payload across GC: return={result_try.returncode}, "
                f"stdout_length={len(result_try.stdout)}, "
                f"stderr={result_try.stderr!r}")

        runtime_fault_cases = [
            ("division_by_zero.bas",
             "ACTION main()\n    value = 10 / 0\nEND ACTION\n",
             2, "division by zero"),
            ("integer_overflow.bas",
             "ACTION main()\n"
             "    value = 9223372036854775807 + 1\n"
             "END ACTION\n",
             2, "integer overflow"),
            ("list_bounds.bas",
             "ACTION main()\n"
             "    values = [10]\n"
             "    item = values[1]\n"
             "END ACTION\n",
             3, "collection index out of bounds"),
            ("zero_range_step.bas",
             "ACTION main()\n"
             "    FOR index = 1 TO 3 STEP 0\n"
             "    END FOR\n"
             "END ACTION\n",
             2, "numeric range step cannot be zero"),
        ]
        for filename, source, expected_line, fault_message in runtime_fault_cases:
            source_path = command_root / filename
            source_path.write_text(source, encoding="utf-8", newline="\n")
            fault = subprocess.run([
                str(compiler), "run", "--root", str(command_root), filename,
            ], capture_output=True, text=True, check=False)
            require(fault.returncode == 70
                    and f"{source_path.name}:{expected_line}:" in fault.stderr
                    and f"runtime fault: {fault_message}" in fault.stderr,
                    f"{filename} did not report its checked runtime fault at "
                    f"the source location: return={fault.returncode}, "
                    f"stderr={fault.stderr!r}")

        (command_root / "objects.bas").write_text(
            "RECORD Pair(left, right)\n"
            "ACTION main()\n"
            "    pair_value = Pair(\"first\", 42)\n"
            "    print($\"record={pair_value.left}:{pair_value.right}\")\n"
            "    numbers = [10, 20, 30]\n"
            "    print($\"list={numbers[1]}\")\n"
            "    VAR numbers_sum = 0\n"
            "    FOR number IN numbers\n"
            "        numbers_sum = numbers_sum + number\n"
            "    END FOR\n"
            "    print($\"sum={numbers_sum}\")\n"
            "    names = {\"first\": \"Ada\", \"last\": \"Lovelace\"}\n"
            "    name = names[\"first\"]\n"
            "    print($\"map={name}\")\n"
            "    FOR key IN names\n"
            "        print(key)\n"
            "    END FOR\n"
            "    changed = pair_value WITH left = \"updated\"\n"
            "    print(changed.left)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        objects = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "objects.bas",
        ], capture_output=True, text=True, check=False)
        require(objects.returncode == 0
                and objects.stdout == (
                    "record=first:42\nlist=20\nsum=60\nmap=Ada\n"
                    "first\nlast\nupdated\n"),
                f"native object model failed: stdout={objects.stdout!r}, "
                f"stderr={objects.stderr!r}")

        native_basic_root = repository / "tests/tooling/fixtures"
        native_basic = subprocess.run([
            str(compiler), "run", "--root", str(native_basic_root),
            "native_basic.bas",
        ], capture_output=True, text=True, check=False)
        require(native_basic.returncode == 0
                and native_basic.stdout == (
                    "run=24\npropagated=7\nerror=9\nmessage=12\n"
                    "render=value 3\nindirect=21\nfloat=7.5\n"),
                "ported non-Actor integration fixture failed: "
                f"return={native_basic.returncode}, "
                f"stdout={native_basic.stdout!r}, "
                f"stderr={native_basic.stderr!r}")

        gc_first = "A" * 40000
        gc_second = "B" * 40000
        (command_root / "gc_roots.bas").write_text(
            "ACTION main()\n"
            f"    first = \"{gc_first}\"\n"
            f"    second = \"{gc_second}\"\n"
            "    FOR index = 1 TO 10\n"
            f"        scratch = \"{gc_second}\"\n"
            "    END FOR\n"
            "    print(first)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        rooted = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "gc_roots.bas",
        ], capture_output=True, text=True, check=False)
        require(rooted.returncode == 0 and rooted.stdout == f"{gc_first}\n",
                "generated shadow roots did not retain a String across GC: "
                f"return={rooted.returncode}, stdout_length={len(rooted.stdout)}, "
                f"stderr={rooted.stderr!r}")
        gc_roots_ir = emit_llvm_ir(command_root, "gc_roots.bas", "gc-roots.ll")
        require("@om_runtime_roots_push" in gc_roots_ir
                and "@om_runtime_roots_pop" in gc_roots_ir,
                "generated LLVM IR omitted shadow-root push/pop calls")

        parameter_payload = "P" * 40000
        (command_root / "gc_parameter_root.bas").write_text(
            "FUNCTION hold(value)\n"
            f"    discarded = \"{parameter_payload}\"\n"
            "    RETURN value\n"
            "END FUNCTION\n"
            "ACTION main()\n"
            '    value = "function parameter survives GC"\n'
            "    print(hold(value))\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        parameter_rooted = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "gc_parameter_root.bas",
        ], capture_output=True, text=True, check=False)
        require(parameter_rooted.returncode == 0
                and parameter_rooted.stdout == "function parameter survives GC\n",
                "managed function parameter was not rooted across allocation: "
                f"return={parameter_rooted.returncode}, "
                f"stdout={parameter_rooted.stdout!r}, "
                f"stderr={parameter_rooted.stderr!r}")

        graph_payload = "G" * 40000
        (command_root / "gc_nested_graph.bas").write_text(
            "RECORD Inner_Record(label, values)\n"
            "RECORD Outer_Record(nested, lookup)\n"
            "ACTION main()\n"
            '    inner = Inner_Record("deep value", ["list value"])\n'
            '    graph = Outer_Record(inner, {"key": inner})\n'
            f"    discarded = \"{graph_payload}\"\n"
            '    retained = graph.lookup["key"]\n'
            "    print(retained.label)\n"
            "    print(retained.values[0])\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        nested_graph = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "gc_nested_graph.bas",
        ], capture_output=True, text=True, check=False)
        require(nested_graph.returncode == 0
                and nested_graph.stdout == "deep value\nlist value\n",
                "nested Record/List/Map graph did not survive collection: "
                f"return={nested_graph.returncode}, "
                f"stdout={nested_graph.stdout!r}, "
                f"stderr={nested_graph.stderr!r}")

        actor_payload = "A" * 40000
        (command_root / "gc_actor_roots.bas").write_text(
            "RECORD Inner_Record(label, values)\n"
            "RECORD Outer_Record(nested, lookup)\n"
            "MESSAGE Ping(value)\n"
            "ACTOR Keeper\n"
            '    VAR label = "actor state survives GC"\n'
            "    RECEIVE Ping(value)\n"
            f"        discarded = \"{actor_payload}\"\n"
            '        retained = value.lookup["key"]\n'
            "        print(label)\n"
            "        print(retained.label)\n"
            "        print(retained.values[0])\n"
            "        STOP\n"
            "    END RECEIVE\n"
            "END ACTOR\n"
            "ACTION main()\n"
            '    inner = Inner_Record("message graph", ["message list"])\n'
            '    graph = Outer_Record(inner, {"key": inner})\n'
            "    keeper_ref = SPAWN Keeper\n"
            "    SEND keeper_ref, Ping(graph)\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        actor_roots = subprocess.run([
            str(compiler), "run", "--root", str(command_root),
            "gc_actor_roots.bas",
        ], capture_output=True, text=True, check=False)
        require(actor_roots.returncode == 0
                and actor_roots.stdout == (
                    "actor state survives GC\nmessage graph\nmessage list\n"),
                "Actor state or transferred nested message was not rooted "
                "across collection: "
                f"return={actor_roots.returncode}, "
                f"stdout={actor_roots.stdout!r}, "
                f"stderr={actor_roots.stderr!r}")

        artifact = command_root / ("app.exe" if sys.platform == "win32" else "app")
        built = subprocess.run([
            str(compiler), "build", "--root", str(command_root), "main.bas",
            "--output", str(artifact),
        ], capture_output=True, text=True, check=False)
        require(built.returncode == 0 and artifact.is_file(),
                f"native build failed: {built.stderr}")
        built_run = subprocess.run([str(artifact)], capture_output=True,
                                   text=True, check=False)
        require(built_run.returncode == 0
                and built_run.stdout == "Hello, OMBasic\n",
                f"built executable failed: stdout={built_run.stdout!r}, "
                f"stderr={built_run.stderr!r}")
        if sys.platform == "win32":
            dependencies = subprocess.run([
                str(readobj_tool), "--coff-imports", str(artifact),
            ], capture_output=True, text=True, check=False)
        else:
            dependencies = subprocess.run([
                str(readobj_tool), "--needed-libs", str(artifact),
            ], capture_output=True, text=True, check=False)
        if sys.platform == "win32":
            imported_libraries = re.findall(
                r"(?im)^\s*name:\s*([^\r\n]+)", dependencies.stdout)
        else:
            needed = re.search(
                r"(?ms)^NeededLibraries\s*\[(.*?)^\]", dependencies.stdout)
            imported_libraries = (re.findall(
                r"(?m)^\s*([^\s]+)\s*$", needed.group(1))
                if needed is not None else [])
        imported_library_names = "\n".join(imported_libraries).lower()
        require(dependencies.returncode == 0
                and not re.search(r"(?:llvm|ombasic).*\.(?:dll|so(?:\.\d+)*)",
                                  imported_library_names),
                "standalone executable imports the compiler or OMBasic runtime: "
                f"return={dependencies.returncode}, "
                f"libraries={imported_libraries!r}, "
                f"output={dependencies.stdout!r}")

    with tempfile.TemporaryDirectory(prefix="ombasic-tooling-") as temporary:
        temporary_path = pathlib.Path(temporary)
        source_path = temporary_path / "format.bas"
        source_path.write_text(
            "FUNCTION main( a,b )\n    RETURN a+b\nEND FUNCTION\n",
            encoding="utf-8", newline="\n")
        formatted = subprocess.run([str(compiler), "fmt", str(source_path)],
                                   capture_output=True, text=True, check=False)
        require(formatted.returncode == 0, "fmt command failed")
        formatted_path = temporary_path / "formatted.bas"
        formatted_path.write_text(formatted.stdout, encoding="utf-8", newline="\n")
        repeated = subprocess.run([str(compiler), "fmt", "--check", str(formatted_path)],
                                 capture_output=True, text=True, check=False)
        require(repeated.returncode == 0, "formatter is not idempotent")

        generated = temporary_path / "sample.omi"
        binding = subprocess.run([
            str(compiler), "bind", "--header",
            str(repository / "tests/tooling/fixtures/bind_api.h"),
            "--library", "sample", "--output", str(generated),
        ], capture_output=True, text=True, check=False)
        require(binding.returncode == 0, f"bind command failed: {binding.stderr}")
        metadata = generated.read_text(encoding="utf-8")
        require("FUNCTION om_sample_add(left: CInt32, right: CInt32): CInt32" in metadata,
                "bind omitted scalar function signature")
        require("value: CString BORROWED" in metadata,
                "bind omitted pointer parameter metadata")
        require("RETURNS STATIC" in metadata and "TODO" not in metadata,
                "bind did not emit reviewable pointer result metadata")
        omi_check = subprocess.run([str(compiler), "check-omi", str(generated)],
                                   capture_output=True, text=True, check=False)
        require(omi_check.returncode == 0,
                f"bind emitted invalid OMI metadata: {omi_check.stderr}")

    basic_root = repository / "tests/tooling/fixtures/project"
    valid = subprocess.run([
        str(compiler), "check", "--root", str(basic_root), "main.bas",
        "--diagnostics=json",
    ], capture_output=True, text=True, check=False)
    require(valid.returncode == 0, f"valid check failed: {valid.stderr}")
    require(json.loads(valid.stdout) == {"diagnostics": []},
            "valid check did not return an empty diagnostic list")

    with tempfile.TemporaryDirectory(prefix="ombasic-down-reason-") as temporary:
        reason_root = pathlib.Path(temporary)
        empty_resource_root = reason_root / "empty-sdk"
        empty_resource_root.mkdir()
        reason_source = (
            "ACTION inspect(reason)\n"
            "    normal = TRUE\n"
            "    failed = FALSE\n"
            "    node_unreachable = TRUE\n"
            "    SELECT reason\n"
            "        CASE Actor_Down_Reason.normal\n"
            "            IF normal THEN\n"
            "                print(\"normal\")\n"
            "            END IF\n"
            "        CASE Actor_Down_Reason.failed\n"
            "            IF failed THEN\n"
            "                print(\"failed\")\n"
            "            END IF\n"
            "        CASE Actor_Down_Reason.node_unreachable\n"
            "            IF node_unreachable THEN\n"
            "                print(\"unreachable\")\n"
            "            END IF\n"
            "    END SELECT\n"
            "END ACTION\n"
            "ACTION main()\n"
            "    reason = Actor_Down_Reason.failed\n"
            "END ACTION\n")
        (reason_root / "main.bas").write_text(
            reason_source, encoding="utf-8", newline="\n")
        reason_check = subprocess.run([
            str(compiler), "check", "--root", str(reason_root), "main.bas",
            "--resource-dir", str(empty_resource_root),
            "--diagnostics=json",
        ], capture_output=True, text=True, check=False)
        require(reason_check.returncode == 0
                and json.loads(reason_check.stdout) == {"diagnostics": []},
                "Actor_Down_Reason members or lowercase identifiers failed: "
                f"return={reason_check.returncode}, "
                f"stdout={reason_check.stdout!r}, "
                f"stderr={reason_check.stderr!r}")
        (reason_root / "missing_runtime.bas").write_text(
            "ACTION main()\n"
            "    print(\"resource check\")\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        missing_runtime = subprocess.run([
            str(compiler), "build", "--root", str(reason_root),
            "missing_runtime.bas",
            "--resource-dir", str(empty_resource_root),
            "-o", str(reason_root / "missing-runtime.exe"),
        ], capture_output=True, text=True, check=False)
        expected_runtime_archive = ("ombasic_runtime.lib" if sys.platform == "win32"
                                   else "libombasic_runtime.a")
        require(missing_runtime.returncode != 0
                and str(empty_resource_root) in missing_runtime.stderr
                and expected_runtime_archive in missing_runtime.stderr,
                "missing SDK archive did not report the selected resource root: "
                f"return={missing_runtime.returncode}, "
                f"stderr={missing_runtime.stderr!r}")
        reason_hir = reason_root / "reason.hir"
        reason_emit = subprocess.run([
            str(compiler), "build", "--root", str(reason_root), "main.bas",
            "--emit=hir", "-o", str(reason_hir),
        ], capture_output=True, text=True, check=False)
        require(reason_emit.returncode == 0 and reason_hir.is_file()
                and "value=builtin::Actor_Down_Reason.failed : Actor_Down_Reason"
                    in reason_hir.read_text(encoding="utf-8"),
                "HIR did not preserve Actor_Down_Reason nominal identity: "
                f"return={reason_emit.returncode}, stderr={reason_emit.stderr!r}")

        invalid_reason_source = (
            "ACTION main()\n"
            "    IF Actor_Down_Reason.failed = 1 THEN\n"
            "        print(\"bad\")\n"
            "    END IF\n"
            "END ACTION\n")
        (reason_root / "invalid.bas").write_text(
            invalid_reason_source, encoding="utf-8", newline="\n")
        invalid_reason = subprocess.run([
            str(compiler), "check", "--root", str(reason_root), "invalid.bas",
            "--diagnostics=json",
        ], capture_output=True, text=True, check=False)
        reason_diagnostics = json.loads(invalid_reason.stdout)["diagnostics"]
        require(invalid_reason.returncode != 0
                and any(item["code"] == "E0401"
                        for item in reason_diagnostics),
                "Int was accepted as Actor_Down_Reason: "
                f"return={invalid_reason.returncode}, "
                f"diagnostics={reason_diagnostics!r}, "
                f"stderr={invalid_reason.stderr!r}")

        (reason_root / "unknown_member.bas").write_text(
            "ACTION main()\n"
            "    reason = Actor_Down_Reason.unknown\n"
            "END ACTION\n",
            encoding="utf-8", newline="\n")
        unknown_member = subprocess.run([
            str(compiler), "check", "--root", str(reason_root),
            "unknown_member.bas", "--diagnostics=json",
        ], capture_output=True, text=True, check=False)
        unknown_diagnostics = json.loads(unknown_member.stdout)["diagnostics"]
        require(unknown_member.returncode != 0 and unknown_diagnostics,
                "unknown Actor_Down_Reason member was accepted")

    lexer_root = repository / "tests/conformance/lexer"
    invalid = subprocess.run([
        str(compiler), "check", "--root", str(lexer_root), "integer_overflow.bas",
        "--diagnostics=json",
    ], capture_output=True, text=True, check=False)
    require(invalid.returncode != 0, "invalid source unexpectedly passed check")
    diagnostics = json.loads(invalid.stdout)["diagnostics"]
    require(diagnostics and diagnostics[0]["code"] == "E0104",
            "structured diagnostics omitted the stable lexer code")
    require(diagnostics[0]["range"]["start"]["line"] == 2,
            "structured diagnostics reported the wrong source line")

    with tempfile.TemporaryDirectory(prefix="ombasic-interface-") as temporary:
        interface_root = pathlib.Path(temporary)
        interface_source = "EXPORT FUNCTION public_api(value)\n    RETURN value + 1\nEND FUNCTION\n"

        def interface_for(text: str) -> dict:
            completed = subprocess.run([
                str(compiler), "check", "--root", str(interface_root), "main.bas",
                "--source-stdin", "--diagnostics=json", "--include-interfaces",
            ], input=text, capture_output=True, text=True, check=False)
            require(completed.returncode == 0,
                    f"interface analysis failed: {completed.stderr}")
            return json.loads(completed.stdout)["interfaces"][0]

        first_interface = interface_for(interface_source)
        second_interface = interface_for(
            interface_source.replace("value + 1", "value + 2"))
        require(first_interface["sourceHash"] != second_interface["sourceHash"],
                "source hash did not change with private implementation text")
        require(first_interface["interfaceHash"] == second_interface["interfaceHash"],
                "interface hash changed without a public signature change")
        require(any(item["name"] == "public_api" for item in first_interface["symbols"]),
                "interface cache omitted an exported symbol")

    source = "FUNCTION main()\n    value = 1\n    RETURN value\nEND FUNCTION\n"
    document = (basic_root / "main.bas").resolve()
    uri = document.as_uri()
    requests = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
            "textDocument": {"uri": uri, "languageId": "ombasic", "version": 1,
                             "text": source}}},
        {"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{"range": {
                "start": {"line": 1, "character": 12},
                "end": {"line": 1, "character": 13}}, "text": "\"text\""}]}},
        {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover", "params": {
            "textDocument": {"uri": uri}, "position": {"line": 2, "character": 13}}},
        {"jsonrpc": "2.0", "id": 3, "method": "textDocument/inlayHint", "params": {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": 3, "character": 0}}}},
        {"jsonrpc": "2.0", "id": 4, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    payload = b"".join(lsp_message(item) for item in requests)
    server = subprocess.run([str(compiler), "lsp", "--root", str(basic_root)],
                            input=payload, capture_output=True, cwd=repository,
                            timeout=45, check=False)
    require(server.returncode == 0,
            f"LSP server failed: {server.stderr.decode('utf-8', errors='replace')}")
    responses = read_lsp_messages(server.stdout)
    hover = next((item for item in responses if item.get("id") == 2), None)
    require(hover is not None and hover["result"] is not None
            and "String" in hover["result"]["contents"]["value"],
            "LSP hover did not refresh from the edited semantic model")
    hints = next((item for item in responses if item.get("id") == 3), None)
    require(hints is not None and hints["result"]
            and any(": String" in item["label"] for item in hints["result"]),
            "LSP inlay hints did not refresh from the edited semantic model")

    with tempfile.TemporaryDirectory(prefix="ombasic-lsp-facade-") as temporary:
        facade_root = pathlib.Path(temporary)
        (facade_root / "math").mkdir()
        (facade_root / "math" / "math.bas").write_text(
            'EXPORT IMPORT "./integer.bas"\n', encoding="utf-8")
        (facade_root / "math" / "integer.bas").write_text(
            'EXPORT FUNCTION answer()\n    RETURN 42\nEND FUNCTION\n'
            'EXPORT ACTION announce()\n    print("action")\nEND ACTION\n',
            encoding="utf-8")
        facade_source = ('IMPORT "math/math.bas"\nACTION main()\n'
                         '    value = math.answer()\nEND ACTION\n')
        facade_analysis = subprocess.run([
            str(compiler), "check", "--root", str(facade_root), "main.bas",
            "--source-stdin", "--diagnostics=json", "--include-interfaces",
        ], input=facade_source, capture_output=True, text=True, cwd=repository,
            check=False)
        require(facade_analysis.returncode == 0,
                f"Facade interface analysis failed: {facade_analysis.stderr}")
        facade_interfaces = json.loads(facade_analysis.stdout)["interfaces"]
        public_facade = next(item for item in facade_interfaces
                             if item["path"] == "math/math.bas")
        public_answer = next(item for item in public_facade["symbols"]
                             if item["name"] == "answer")
        public_action = next(item for item in public_facade["symbols"]
                             if item["name"] == "announce")
        require(public_answer["definition"]["path"] == "math/integer.bas",
                "Facade metadata lost the original definition location")
        require(public_action["kind"] == "action" and public_action["effects"] != 0,
                "Facade metadata lost the original Action kind or effects")
        private_source = facade_root / "math" / "integer.bas"
        original_interface_hash = public_facade["interfaceHash"]
        private_source.write_text(
            'EXPORT FUNCTION answer()\n    RETURN 43\nEND FUNCTION\n'
            'EXPORT ACTION announce()\n    print("action")\nEND ACTION\n',
            encoding="utf-8")
        changed_analysis = subprocess.run([
            str(compiler), "check", "--root", str(facade_root), "main.bas",
            "--source-stdin", "--diagnostics=json", "--include-interfaces",
        ], input=facade_source, capture_output=True, text=True, cwd=repository,
            check=False)
        changed_facade = next(item for item in json.loads(changed_analysis.stdout)["interfaces"]
                              if item["path"] == "math/math.bas")
        require(changed_facade["interfaceHash"] == original_interface_hash,
                "private implementation change altered the Facade interface fingerprint")
        private_source.write_text(
            'EXPORT FUNCTION answer()\n    RETURN "forty-two"\nEND FUNCTION\n'
            'EXPORT ACTION announce()\n    print("action")\nEND ACTION\n',
            encoding="utf-8")
        public_change = subprocess.run([
            str(compiler), "check", "--root", str(facade_root), "main.bas",
            "--source-stdin", "--diagnostics=json", "--include-interfaces",
        ], input=facade_source, capture_output=True, text=True, cwd=repository,
            check=False)
        require(public_change.returncode == 0,
                f"Facade public contract analysis failed: {public_change.stderr}")
        changed_contract = next(item for item in json.loads(public_change.stdout)["interfaces"]
                                if item["path"] == "math/math.bas")
        require(changed_contract["interfaceHash"] != original_interface_hash,
                "public type change did not alter the Facade interface fingerprint")
        private_source.write_text(
            'EXPORT FUNCTION answer()\n    RETURN 42\nEND FUNCTION\n'
            'EXPORT ACTION announce()\n    print("action")\nEND ACTION\n',
            encoding="utf-8")
        facade_document = (facade_root / "main.bas").as_uri()
        facade_requests = [
            {"jsonrpc": "2.0", "id": 10, "method": "initialize", "params": {}},
            {"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {
                "textDocument": {"uri": facade_document, "languageId": "ombasic",
                                 "version": 1, "text": facade_source}}},
            {"jsonrpc": "2.0", "id": 11, "method": "textDocument/completion", "params": {
                "textDocument": {"uri": facade_document},
                "position": {"line": 2, "character": 18}}},
            {"jsonrpc": "2.0", "id": 12, "method": "textDocument/hover", "params": {
                "textDocument": {"uri": facade_document},
                "position": {"line": 2, "character": 23}}},
            {"jsonrpc": "2.0", "id": 14, "method": "textDocument/definition", "params": {
                "textDocument": {"uri": facade_document},
                "position": {"line": 2, "character": 23}}},
            {"jsonrpc": "2.0", "id": 13, "method": "shutdown", "params": None},
            {"jsonrpc": "2.0", "method": "exit", "params": None},
        ]
        facade_server = subprocess.run(
            [str(compiler), "lsp", "--root", str(facade_root)],
            input=b"".join(lsp_message(item) for item in facade_requests),
            capture_output=True, cwd=repository, timeout=45, check=False)
        require(facade_server.returncode == 0,
                "LSP facade server failed: " + facade_server.stderr.decode(
                    "utf-8", errors="replace"))
        facade_responses = read_lsp_messages(facade_server.stdout)
        completion = next((item for item in facade_responses if item.get("id") == 11), None)
        require(completion is not None and any(
            item.get("label") == "answer" for item in completion["result"]["items"]),
            "LSP completion omitted a Facade re-export")
        facade_hover = next((item for item in facade_responses if item.get("id") == 12), None)
        require(facade_hover is not None and facade_hover["result"] is not None
                and "Int" in facade_hover["result"]["contents"]["value"],
                "LSP hover omitted Facade member type information")
        definition = next((item for item in facade_responses if item.get("id") == 14), None)
        require(definition is not None and definition["result"] is not None
                and definition["result"]["uri"].endswith("/math/integer.bas")
                and definition["result"]["range"]["start"]["line"] == 0,
                "LSP go-to-definition did not reach the original declaration")
    print("PASS tooling CLI, native run/build, formatter, diagnostics JSON, bind, LSP hover/inlay/completion")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
