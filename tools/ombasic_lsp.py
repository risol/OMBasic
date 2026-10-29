#!/usr/bin/env python3
"""Small LSP bridge over the compiler's shared semantic pipeline."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys
from urllib.parse import unquote, urlparse
from urllib.request import url2pathname


def uri_path(uri: str) -> pathlib.Path:
    parsed = urlparse(uri)
    if parsed.scheme != "file":
        raise ValueError("only file:// documents are supported")
    return pathlib.Path(url2pathname(unquote(parsed.path)))


def byte_offset(text: str, position: dict) -> int:
    lines = text.splitlines(keepends=True)
    line = int(position.get("line", 0))
    character = int(position.get("character", 0))
    if line < 0 or line > len(lines) or character < 0:
        raise ValueError("position is out of range")
    if line == len(lines):
        return len(text)
    start = sum(len(part) for part in lines[:line])
    contents = lines[line].rstrip("\r\n")
    units = 0
    index = 0
    while index < len(contents) and units < character:
        units += len(contents[index].encode("utf-16-le")) // 2
        index += 1
    if units != character:
        raise ValueError("position splits a UTF-16 surrogate pair")
    return start + index


def byte_column_to_utf16(text: str, line_number: int, byte_column: int) -> int:
    lines = text.splitlines()
    if line_number <= 0 or line_number > len(lines):
        return 0
    line = lines[line_number - 1]
    prefix = line.encode("utf-8")[:max(0, byte_column - 1)]
    try:
        return len(prefix.decode("utf-8").encode("utf-16-le")) // 2
    except UnicodeDecodeError:
        return 0


def compiler_analysis(compiler: pathlib.Path, root: pathlib.Path,
                      path: pathlib.Path, text: str) -> dict:
    relative = path.resolve().relative_to(root.resolve()).as_posix()
    command = [str(compiler), "check", "--root", str(root.resolve()), relative,
               "--source-stdin", "--diagnostics=json", "--include-inferred",
               "--include-interfaces"]
    completed = subprocess.run(command, input=text.encode("utf-8"),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               cwd=root, check=False)
    try:
        result = json.loads(completed.stdout.decode("utf-8"))
        if isinstance(result, dict) and "diagnostics" in result:
            result.setdefault("inferred", [])
            return result
    except (UnicodeDecodeError, json.JSONDecodeError):
        pass
    message = completed.stderr.decode("utf-8", errors="replace").strip()
    if not message:
        message = f"compiler analysis failed with exit code {completed.returncode}"
    print(f"ombasic-lsp: {message}", file=sys.stderr)
    return {"diagnostics": [], "inferred": []}


def lsp_diagnostics(items: list, text: str) -> list:
    converted = []
    for item in items:
        source_range = item.get("range", {})
        start = source_range.get("start", {})
        end = source_range.get("end", {})
        converted.append({
            "range": {
                "start": {"line": max(0, int(start.get("line", 1)) - 1),
                          "character": byte_column_to_utf16(
                              text, int(start.get("line", 1)),
                              int(start.get("byteColumn", 1)))},
                "end": {"line": max(0, int(end.get("line", 1)) - 1),
                        "character": byte_column_to_utf16(
                            text, int(end.get("line", 1)),
                            int(end.get("byteColumn", 1)))},
            },
            "severity": 1,
            "code": item.get("code", "E0000"),
            "source": "OMBasic",
            "message": item.get("message", "Compiler diagnostic"),
        })
    return converted


def inferred_offsets(items: list, text: str) -> list:
    lines = text.splitlines(keepends=True)
    starts = []
    offset = 0
    for line in lines:
        starts.append(offset)
        offset += len(line.encode("utf-8"))
    result = []
    for item in items:
        span = item.get("range", {})
        start = span.get("start", {})
        end = span.get("end", {})
        start_line = int(start.get("line", 1)) - 1
        end_line = int(end.get("line", 1)) - 1
        if start_line < 0 or end_line < 0 or start_line >= len(starts) or end_line >= len(starts):
            continue
        start_offset = starts[start_line] + int(start.get("byteColumn", 1)) - 1
        end_offset = starts[end_line] + int(end.get("byteColumn", 1)) - 1
        result.append((start_offset, end_offset, item))
    return result


class Server:
    def __init__(self, compiler: pathlib.Path, root: pathlib.Path):
        self.compiler = compiler.resolve()
        self.root = root.resolve()
        self.documents: dict[str, dict] = {}
        self.shutdown_requested = False

    def send(self, message: dict) -> None:
        body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        sys.stdout.buffer.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii"))
        sys.stdout.buffer.write(body)
        sys.stdout.buffer.flush()

    def response(self, request_id, result) -> None:
        self.send({"jsonrpc": "2.0", "id": request_id, "result": result})

    def read_message(self):
        content_length = None
        while True:
            header = sys.stdin.buffer.readline()
            if not header:
                return None
            if header in (b"\r\n", b"\n"):
                break
            name, separator, value = header.partition(b":")
            if separator and name.strip().lower() == b"content-length":
                content_length = int(value.strip())
        if content_length is None or content_length < 0 or content_length > 64 * 1024 * 1024:
            raise ValueError("invalid LSP Content-Length")
        body = sys.stdin.buffer.read(content_length)
        if len(body) != content_length:
            return None
        return json.loads(body.decode("utf-8"))

    def document(self, uri: str, supplied_text: str | None = None) -> tuple[pathlib.Path, str]:
        path = uri_path(uri).resolve()
        path.relative_to(self.root)
        if supplied_text is not None:
            return path, supplied_text
        opened = self.documents.get(uri)
        if opened is not None:
            return path, opened["text"]
        return path, path.read_text(encoding="utf-8")

    def analyze_document(self, uri: str, text: str) -> dict:
        path, text = self.document(uri, text)
        return compiler_analysis(self.compiler, self.root, path, text)

    def publish(self, uri: str, version: int | None, text: str) -> None:
        analysis = self.analyze_document(uri, text)
        params = {"uri": uri, "diagnostics": lsp_diagnostics(
            analysis.get("diagnostics", []), text)}
        if version is not None:
            params["version"] = version
        self.send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                   "params": params})

    def dispatch(self, request: dict) -> bool:
        method = request.get("method")
        params = request.get("params", {})
        request_id = request.get("id")
        if method == "initialize":
            self.response(request_id, {
                "capabilities": {
                    "textDocumentSync": {"openClose": True, "change": 1},
                    "completionProvider": {"triggerCharacters": ["."]},
                    "definitionProvider": True,
                    "hoverProvider": True,
                    "inlayHintProvider": True,
                },
                "serverInfo": {"name": "ombasic-lsp", "version": "0.1.0"},
            })
            return True
        if method == "initialized" or method == "$/cancelRequest":
            return True
        if method == "shutdown":
            self.shutdown_requested = True
            self.response(request_id, None)
            return True
        if method == "exit":
            return False
        if method == "textDocument/didOpen":
            document = params.get("textDocument", {})
            uri = document.get("uri")
            if uri:
                text = document.get("text", "")
                version = document.get("version")
                self.documents[uri] = {"text": text, "version": version}
                self.publish(uri, version, text)
            return True
        if method == "textDocument/didChange":
            document = params.get("textDocument", {})
            uri = document.get("uri")
            if not uri or uri not in self.documents:
                return True
            text = self.documents[uri]["text"]
            try:
                for change in params.get("contentChanges", []):
                    if "range" not in change:
                        text = change.get("text", "")
                        continue
                    range_value = change["range"]
                    start = byte_offset(text, range_value["start"])
                    end = byte_offset(text, range_value["end"])
                    if end < start:
                        raise ValueError("change range end precedes start")
                    text = text[:start] + change.get("text", "") + text[end:]
            except (KeyError, TypeError, ValueError) as error:
                print(f"ombasic-lsp: rejected document change: {error}", file=sys.stderr)
                return True
            version = document.get("version")
            self.documents[uri] = {"text": text, "version": version}
            self.publish(uri, version, text)
            return True
        if method == "textDocument/didClose":
            document = params.get("textDocument", {})
            uri = document.get("uri")
            if uri:
                self.documents.pop(uri, None)
                self.send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
                           "params": {"uri": uri, "diagnostics": []}})
            return True
        if method == "textDocument/hover":
            uri = params.get("textDocument", {}).get("uri")
            try:
                path, text = self.document(uri)
                analysis = compiler_analysis(self.compiler, self.root, path, text)
                point = len(text[:byte_offset(text, params.get("position", {}))].encode("utf-8"))
                matches = [entry for entry in inferred_offsets(analysis.get("inferred", []), text)
                           if entry[0] <= point <= entry[1]]
                if not matches:
                    prefix = text[:point]
                    member_match = re.search(
                        r"([a-z][a-z0-9_]*(?:_[a-z][a-z0-9_]*)*)\.([a-z][a-z0-9_]*)$",
                        prefix)
                    symbol = None
                    if member_match:
                        alias, member_name = member_match.groups()
                        for interface in analysis.get("interfaces", []):
                            if pathlib.PurePosixPath(interface.get("path", "")).stem != alias:
                                continue
                            symbol = next((item for item in interface.get("symbols", [])
                                           if item.get("name") == member_name), None)
                            if symbol:
                                break
                    if symbol is None:
                        self.response(request_id, None)
                    else:
                        value = f"`{symbol.get('name')}: {symbol.get('type', 'Unknown')}`"
                        value += f"\n\nEffects: {symbol.get('effects', 0)}"
                        definition = symbol.get("definition", {})
                        if definition.get("path"):
                            location = definition.get("range", {}).get("start", {})
                            value += (f"\n\nDefined at `{definition['path']}:"
                                      f"{location.get('line', '?')}`")
                        self.response(request_id, {"contents": {
                            "kind": "markdown", "value": value}})
                else:
                    _start, _end, item = min(matches, key=lambda entry: entry[1] - entry[0])
                    value = f"`{item.get('name') + ': ' if item.get('name') else ''}{item.get('type', 'Unknown')}`"
                    self.response(request_id, {"contents": {"kind": "markdown", "value": value}})
            except (OSError, ValueError, KeyError) as error:
                print(f"ombasic-lsp: hover failed: {error}", file=sys.stderr)
                self.response(request_id, None)
            return True
        if method == "textDocument/completion":
            uri = params.get("textDocument", {}).get("uri")
            try:
                path, text = self.document(uri)
                analysis = compiler_analysis(self.compiler, self.root, path, text)
                offset = byte_offset(text, params.get("position", {}))
                prefix = text[:offset]
                match = re.search(r"([a-z][a-z0-9_]*(?:_[a-z][a-z0-9_]*)*)\.[a-z0-9_]*$",
                                  prefix)
                items = []
                if match:
                    alias = match.group(1)
                    for interface in analysis.get("interfaces", []):
                        module_path = pathlib.PurePosixPath(interface.get("path", ""))
                        if module_path.stem != alias:
                            continue
                        for symbol in interface.get("symbols", []):
                            items.append({
                                "label": symbol.get("name", ""),
                                "kind": 3 if symbol.get("kind") == "function" else 2,
                                "detail": symbol.get("type", ""),
                            })
                self.response(request_id, {"isIncomplete": False, "items": items})
            except (OSError, ValueError, KeyError) as error:
                print(f"ombasic-lsp: completion failed: {error}", file=sys.stderr)
                self.response(request_id, {"isIncomplete": False, "items": []})
            return True
        if method == "textDocument/definition":
            uri = params.get("textDocument", {}).get("uri")
            try:
                path, text = self.document(uri)
                analysis = compiler_analysis(self.compiler, self.root, path, text)
                point = byte_offset(text, params.get("position", {}))
                prefix = text[:point]
                member_match = re.search(
                    r"([a-z][a-z0-9_]*(?:_[a-z][a-z0-9_]*)*)\.([a-z][a-z0-9_]*)$",
                    prefix)
                location = None
                if member_match:
                    alias, member_name = member_match.groups()
                    for interface in analysis.get("interfaces", []):
                        if pathlib.PurePosixPath(interface.get("path", "")).stem != alias:
                            continue
                        symbol = next((item for item in interface.get("symbols", [])
                                       if item.get("name") == member_name), None)
                        definition = symbol.get("definition") if symbol else None
                        source_range = definition.get("range") if definition else None
                        if not definition or not source_range:
                            continue
                        definition_path = (self.root / definition["path"]).resolve()
                        definition_path.relative_to(self.root)
                        definition_text = definition_path.read_text(encoding="utf-8")
                        start = source_range["start"]
                        end = source_range["end"]
                        location = {
                            "uri": definition_path.as_uri(),
                            "range": {
                                "start": {
                                    "line": int(start["line"]) - 1,
                                    "character": byte_column_to_utf16(
                                        definition_text, int(start["line"]),
                                        int(start["byteColumn"])),
                                },
                                "end": {
                                    "line": int(end["line"]) - 1,
                                    "character": byte_column_to_utf16(
                                        definition_text, int(end["line"]),
                                        int(end["byteColumn"])),
                                },
                            },
                        }
                        break
                self.response(request_id, location)
            except (OSError, ValueError, KeyError) as error:
                print(f"ombasic-lsp: definition lookup failed: {error}", file=sys.stderr)
                self.response(request_id, None)
            return True
        if method == "textDocument/inlayHint":
            uri = params.get("textDocument", {}).get("uri")
            try:
                path, text = self.document(uri)
                analysis = compiler_analysis(self.compiler, self.root, path, text)
                request_range = params.get("range", {})
                start = request_range.get("start", {"line": 0, "character": 0})
                end = request_range.get("end", {"line": len(text.splitlines()), "character": 0})
                hints = []
                for start_offset, _end_offset, item in inferred_offsets(
                        analysis.get("inferred", []), text):
                    if item.get("kind") != "binding" or not item.get("name"):
                        continue
                    source_range = item["range"]["start"]
                    line = int(source_range["line"]) - 1
                    character = byte_column_to_utf16(text, line + 1,
                                                     int(source_range["byteColumn"]))
                    # OMBasic identifiers are ASCII; their byte and UTF-16 lengths match.
                    character += len(item["name"])
                    point = {"line": line, "character": character}
                    point_key = (point["line"], point["character"])
                    if point_key < (int(start.get("line", 0)), int(start.get("character", 0))) \
                            or point_key > (int(end.get("line", 0)),
                                            int(end.get("character", 0))):
                        continue
                    hints.append({"position": point,
                                  "label": f": {item.get('type', 'Unknown')}",
                                  "kind": 1,
                                  "paddingLeft": True})
                self.response(request_id, hints)
            except (OSError, ValueError, KeyError) as error:
                print(f"ombasic-lsp: inlay hint failed: {error}", file=sys.stderr)
                self.response(request_id, [])
            return True
        if request_id is not None:
            self.send({"jsonrpc": "2.0", "id": request_id,
                       "error": {"code": -32601, "message": f"Method not found: {method}"}})
        return True

    def run(self) -> int:
        while True:
            message = self.read_message()
            if message is None:
                break
            if not self.dispatch(message):
                break
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=pathlib.Path)
    parser.add_argument("--root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        server = Server(args.compiler, args.root)
        return server.run()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"ombasic-lsp: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
