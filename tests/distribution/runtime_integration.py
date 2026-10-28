import queue
import subprocess
import sys
import threading


def run_scenario(executable: str, server_mode: str, client_mode: str) -> None:
    server = subprocess.Popen(
        [executable, server_mode],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    ready = queue.Queue(maxsize=1)

    def read_ready_line() -> None:
        assert server.stdout is not None
        ready.put(server.stdout.readline())

    reader = threading.Thread(target=read_ready_line, daemon=True)
    reader.start()
    try:
        try:
            line = ready.get(timeout=15)
        except queue.Empty:
            stdout, stderr = server.communicate(timeout=5)
            raise RuntimeError(
                f"{server_mode} did not announce its port\n"
                f"stdout: {stdout}\nstderr: {stderr}"
            )
        parts = line.strip().split()
        if len(parts) != 2 or parts[0] != "PORT":
            stdout, stderr = server.communicate(timeout=5)
            raise RuntimeError(
                f"{server_mode} failed to start\nline: {line!r}\n"
                f"stdout: {stdout}\nstderr: {stderr}"
            )
        client = subprocess.run(
            [executable, client_mode, parts[1]],
            capture_output=True,
            text=True,
            timeout=25,
            check=False,
        )
        if client.returncode != 0:
            _, server_stderr = server.communicate(timeout=10)
            raise RuntimeError(
                f"{client_mode} exited with {client.returncode}\n"
                f"stdout: {client.stdout}\nstderr: {client.stderr}\n"
                f"server stderr: {server_stderr}"
            )
        server_stdout, server_stderr = server.communicate(timeout=10)
        if server.returncode != 0:
            raise RuntimeError(
                f"{server_mode} exited with {server.returncode}\n"
                f"stdout: {server_stdout}\nstderr: {server_stderr}"
            )
    finally:
        if server.poll() is None:
            server.kill()
            server.wait(timeout=5)


def main() -> int:
    executable = sys.argv[1]
    run_scenario(executable, "server", "client")
    run_scenario(executable, "unreachable-server", "unreachable-client")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
