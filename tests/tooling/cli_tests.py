from __future__ import annotations

import json
import pathlib
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

    version = subprocess.run([str(compiler), "version"], capture_output=True,
                             text=True, check=False)
    require(version.returncode == 0 and "OMBasic" in version.stdout,
            "version command failed")

    with tempfile.TemporaryDirectory(prefix="ombasic-commands-") as temporary:
        command_root = pathlib.Path(temporary)
        (command_root / "main.bas").write_text(
            "FUNCTION main()\n    RETURN 40 + 2\nEND FUNCTION\n"
            "FUNCTION alternate()\n    RETURN 17\nEND FUNCTION\n"
            "FUNCTION broken()\n    RETURN 1 / 0\nEND FUNCTION\n",
            encoding="utf-8", newline="\n")
        ran = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
        ], capture_output=True, text=True, check=False)
        require(ran.returncode == 0 and ran.stdout == "42\n",
                f"run command returned unexpected output: {ran.stderr}")
        alternate = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--entry", "alternate",
        ], capture_output=True, text=True, check=False)
        require(alternate.returncode == 0 and alternate.stdout == "17\n",
                f"run --entry failed: {alternate.stderr}")
        runtime_error = subprocess.run([
            str(compiler), "run", "--root", str(command_root), "main.bas",
            "--entry", "broken",
        ], capture_output=True, text=True, check=False)
        require(runtime_error.returncode != 0
                and "runtime error" in runtime_error.stderr
                and "main.bas:8:" in runtime_error.stderr,
                f"run did not report the runtime failure location: {runtime_error.stderr}")

        artifact = command_root / "app.ombc"
        built = subprocess.run([
            str(compiler), "build", "--root", str(command_root), "main.bas",
            "--output", str(artifact),
        ], capture_output=True, text=True, check=False)
        require(built.returncode == 0 and artifact.is_file(),
                f"build command failed: {built.stderr}")
        require(artifact.read_bytes().startswith(b"OMBC"),
                "build output is not a serialized OMBC image")

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

    basic_root = repository / "tests/runtime/fixtures/basic"
    valid = subprocess.run([
        str(compiler), "check", "--root", str(basic_root), "main.bas",
        "--diagnostics=json",
    ], capture_output=True, text=True, check=False)
    require(valid.returncode == 0, f"valid check failed: {valid.stderr}")
    require(json.loads(valid.stdout) == {"diagnostics": []},
            "valid check did not return an empty diagnostic list")

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
    print("PASS tooling CLI, run/build, formatter, diagnostics JSON, bind, LSP hover/inlay")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
