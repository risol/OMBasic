#!/usr/bin/env python3
"""Run a local HTTP server and exercise the stdlib HTTP Client Actor."""

from __future__ import annotations

import argparse
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


PROGRAM = '''IMPORT "@std/encoding.bas"
IMPORT "@std/http/client.bas"

MESSAGE Start_Http_Client()

ACTOR Http_Client_Probe
    VAR service = Actor_Id()
    VAR response_count = 0

    RECEIVE Start_Http_Client()
        service = Actor_Id(SPAWN client.Http_Client)
        SEND service, client.Http_Get("GET", "http://127.0.0.1:__PORT__/probe?x=1", "", encoding.Buffer([]), Actor_Id(SELF))
    END RECEIVE

    RECEIVE client.Http_Completed(success, status, reason, headers, body, error_code, error_offset)
        IF success AND response_count = 0 THEN
            response_count = 1
            print($"http-first={status}|{reason}|{body.bytes[0]}|{body.bytes[4]}")
            SEND service, client.Http_Get("GET", "http://127.0.0.1:__PORT__/probe?x=2", "", encoding.Buffer([]), Actor_Id(SELF))
        ELSE IF success THEN
            print($"http-second={status}|{reason}|{body.bytes[0]}|{body.bytes[4]}")
            SEND service, client.Http_Close()
            STOP
        ELSE
            print($"http-error={error_code}|{error_offset}")
            STOP
        END IF
    END RECEIVE


END ACTOR

ACTION main()
    probe = SPAWN Http_Client_Probe
    SEND probe, Start_Http_Client()
END ACTION
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--case-json", required=True)
    parser.add_argument("--repository", type=Path, required=True)
    args = parser.parse_args()
    del args.case_json, args.repository

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(30)
    port = listener.getsockname()[1]
    server_errors: list[str] = []
    observed_request: list[bytes] = []
    server_state = ["not accepted"]

    def serve() -> None:
        try:
            connection, _ = listener.accept()
            server_state[0] = "accepted; waiting for request headers"
            with connection:
                connection.settimeout(30)
                for index in range(2):
                    request = bytearray()
                    while b"\r\n\r\n" not in request:
                        part = connection.recv(4096)
                        if not part:
                            break
                        request.extend(part)
                    first_line = bytes(request).split(b"\r\n", 1)[0]
                    observed_request.append(bytes(request))
                    server_state[0] = f"received {len(request)} request bytes"
                    expected_line = f"GET /probe?x={index + 1} HTTP/1.1".encode()
                    if first_line != expected_line:
                        server_errors.append(f"unexpected request line {first_line!r}")
                        return
                    if b"Connection: keep-alive\r\n" not in request:
                        server_errors.append("request did not ask to keep the connection alive")
                        return
                    connection.sendall(
                        b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
                    )
        except Exception as error:  # surfaced as a scenario failure
            server_errors.append(f"server: {error}")
        finally:
            listener.close()

    server = threading.Thread(target=serve, daemon=True)
    server.start()
    with tempfile.TemporaryDirectory(prefix="ombasic-http-client-") as directory:
        root = Path(directory)
        (root / "http_client.bas").write_text(
            PROGRAM.replace("__PORT__", str(port)), encoding="utf-8")
        binary = root / "http_client.exe"
        build = subprocess.run(
            [str(args.compiler.resolve()), "build", "--root", str(root),
             "-o", str(binary), "http_client.bas"],
            cwd=root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=25,
            check=False,
        )
        if build.returncode != 0:
            listener.close()
            server.join(timeout=5)
            print(f"compiler build failed: {build.stderr!r}", file=sys.stderr)
            return 1
        process = subprocess.Popen(
            [str(binary)],
            cwd=root,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        try:
            stdout, stderr = process.communicate(timeout=35)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate(timeout=3)
            server_errors.append("HTTP client process timed out")
        result_code = process.returncode
    server.join(timeout=31)
    if server.is_alive():
        server_errors.append("local HTTP server did not stop")
    if (result_code != 0 or stdout != "http-first=200|OK|104|111\nhttp-second=200|OK|104|111\n"
            or server_errors or len(observed_request) != 2):
        print(f"compiler exit={result_code}", file=sys.stderr)
        print(f"stdout={stdout!r}", file=sys.stderr)
        print(f"stderr={stderr!r}", file=sys.stderr)
        print(f"server={server_errors!r}", file=sys.stderr)
        print(f"request={observed_request!r}", file=sys.stderr)
        print(f"server state={server_state[0]}", file=sys.stderr)
        return 1
    print(stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
