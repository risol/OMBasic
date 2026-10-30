#!/usr/bin/env python3
"""Exercise the HTTP Client Actor over a locally trusted TLS connection."""

from __future__ import annotations

import argparse
import os
import socket
import ssl
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

    RECEIVE Start_Http_Client()
        service = Actor_Id(SPAWN client.Http_Client)
        SEND service, client.Http_Get("GET", "https://localhost:__PORT__/secure", "", encoding.Buffer([]), Actor_Id(SELF))
    END RECEIVE

    RECEIVE client.Http_Completed(success, status, reason, headers, body, error_code, error_offset)
        IF success THEN
            print($"https={status}|{reason}|{body.bytes[0]}|{body.bytes[4]}")
        ELSE
            print($"https-error={error_code}|{error_offset}")
        END IF
        SEND service, client.Http_Close()
        STOP
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
    del args.case_json

    fixture = args.repository.resolve().parents[1] / "tests" / "runtime" / "fixtures" / "tls"
    authority = fixture / "ca.pem"
    certificate = fixture / "server.pem"
    key = fixture / "server.key"
    listener = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    listener.bind(("::", 0))
    listener.listen(1)
    listener.settimeout(30)
    port = listener.getsockname()[1]
    failures: list[str] = []
    requests: list[bytes] = []

    def serve() -> None:
        try:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(str(certificate), str(key))
            connection, _ = listener.accept()
            with connection, context.wrap_socket(connection, server_side=True) as tls:
                tls.settimeout(30)
                request = bytearray()
                while b"\r\n\r\n" not in request:
                    part = tls.recv(4096)
                    if not part:
                        break
                    request.extend(part)
                requests.append(bytes(request))
                first_line = bytes(request).split(b"\r\n", 1)[0]
                if first_line != b"GET /secure HTTP/1.1":
                    failures.append(f"unexpected request line {first_line!r}")
                    return
                tls.sendall(
                    b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                    b"Connection: close\r\n\r\nhello"
                )
        except Exception as error:  # surfaced below as a scenario failure
            failures.append(f"server: {error}")
        finally:
            listener.close()

    server = threading.Thread(target=serve, daemon=True)
    server.start()
    with tempfile.TemporaryDirectory(prefix="ombasic-http-https-") as directory:
        root = Path(directory)
        (root / "http_client.bas").write_text(
            PROGRAM.replace("__PORT__", str(port)), encoding="utf-8")
        binary = root / "http_client"
        build = subprocess.run(
            [str(args.compiler.resolve()), "build", "--root", str(root),
             "-o", str(binary), "http_client.bas"],
            cwd=root, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=25, check=False,
        )
        if build.returncode != 0:
            listener.close()
            server.join(timeout=5)
            print(f"compiler build failed: {build.stderr!r}", file=sys.stderr)
            return 1
        environment = os.environ.copy()
        environment["SSL_CERT_FILE"] = str(authority)
        try:
            result = subprocess.run(
                [str(binary)], cwd=root, env=environment, capture_output=True,
                text=True, encoding="utf-8", errors="replace", timeout=35,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            failures.append("HTTP-over-TLS client timed out")
            result = subprocess.CompletedProcess([], -1, error.stdout or "", error.stderr or "")
    server.join(timeout=31)
    if server.is_alive():
        failures.append("local HTTPS server did not stop")
    if result.returncode != 0 or result.stdout != "https=200|OK|104|111\n" or failures:
        print(f"compiler exit={result.returncode}", file=sys.stderr)
        print(f"stdout={result.stdout!r}", file=sys.stderr)
        print(f"stderr={result.stderr!r}", file=sys.stderr)
        print(f"server={failures!r}", file=sys.stderr)
        print(f"request={requests!r}", file=sys.stderr)
        return 1
    print(result.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
