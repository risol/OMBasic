#!/usr/bin/env python3
"""Run a local TLS server and exercise TLS messages through Network Actor."""

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


PROGRAM = r'''IMPORT "@std/collections/list.bas"

MESSAGE Start_Tls_Loopback()

ACTOR Tls_Loopback_Client
    RECEIVE Start_Tls_Loopback()
        SEND Network, Tcp_Connect("127.0.0.1", __PORT__, SELF)
    END RECEIVE

    RECEIVE Tcp_Connect_Completed(outcome)
        SELECT outcome
        CASE Ok(connection_id)
            SEND Network, Tls_Client_Start(connection_id, "localhost", SELF)
        CASE Error(problem)
            print($"connect-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tls_Client_Start_Completed(outcome)
        SELECT outcome
        CASE Ok(tls_id)
            SEND Network, Tls_Send(tls_id, [104, 101, 108, 108, 111], SELF)
        CASE Error(problem)
            print($"tls-start-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tls_Send_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes_sent)
            SEND Network, Tls_Receive(1, 5, SELF)
        CASE Error(problem)
            print($"tls-send-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tls_Receive_Completed(outcome)
        SELECT outcome
        CASE Ok(bytes)
            IF list.length(bytes) = 5 AND bytes[0] = 119 AND bytes[4] = 100 THEN
                print("tls=world")
            ELSE
                print($"tls-receive-bytes={list.length(bytes)}")
            END IF
            SEND Network, Tls_Close(1, SELF)
        CASE Error(problem)
            print($"tls-receive-error={problem}")
            STOP
        END SELECT
    END RECEIVE

    RECEIVE Tls_Close_Completed(outcome)
        STOP
    END RECEIVE
END ACTOR

ACTION main()
    client = SPAWN Tls_Loopback_Client
    SEND client, Start_Tls_Loopback()
END ACTION
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--case-json", required=True)
    parser.add_argument("--repository", type=Path, required=True)
    args = parser.parse_args()
    del args.case_json

    fixture = args.repository.resolve().parents[1] / "tests" / "runtime" \
        / "fixtures" / "tls"
    certificate_authority = fixture / "ca.pem"
    server_certificate = fixture / "server.pem"
    server_key = fixture / "server.key"
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(10)
    port = listener.getsockname()[1]
    failure: list[str] = []

    def serve() -> None:
        try:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(str(server_certificate), str(server_key))
            connection, _ = listener.accept()
            with connection, context.wrap_socket(connection, server_side=True) as tls:
                payload = tls.recv(5)
                if payload != b"hello":
                    failure.append(f"unexpected client payload {payload!r}")
                    return
                tls.sendall(b"world")
        except Exception as error:  # surfaced below as the scenario failure
            failure.append(str(error))
        finally:
            listener.close()

    server = threading.Thread(target=serve, daemon=True)
    server.start()
    with tempfile.TemporaryDirectory(prefix="ombasic-tls-actor-") as directory:
        root = Path(directory)
        source = PROGRAM.replace("__PORT__", str(port))
        (root / "tls_client.bas").write_text(source, encoding="utf-8")
        environment = os.environ.copy()
        environment["SSL_CERT_FILE"] = str(certificate_authority)
        result = subprocess.run(
            [str(args.compiler.resolve()), "run", "--root", str(root),
             "tls_client.bas"],
            cwd=root,
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=20,
            check=False,
        )
    server.join(timeout=10)
    if server.is_alive():
        print("local TLS server did not stop", file=sys.stderr)
        return 1
    if result.returncode != 0 or result.stdout != "tls=world\n" or failure:
        print(f"compiler exit={result.returncode}", file=sys.stderr)
        print(f"stdout={result.stdout!r}", file=sys.stderr)
        print(f"stderr={result.stderr!r}", file=sys.stderr)
        print(f"server={failure!r}", file=sys.stderr)
        return 1
    print(result.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
