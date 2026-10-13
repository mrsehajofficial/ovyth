#!/usr/bin/env python3
"""Ovyth agent client — talk to the agent running on your PC from your phone.

The PC side:

    OVYTH_AGENT_SERVE=0 OVYTH_AGENT_TOKEN=$(openssl rand -hex 16) \
        OVYTH_AGENT_HOST=0.0.0.0 ovc agent

The client writes the port to ~/.ovyth/agent.port (when
OVYTH_AGENT_PORT_FILE is set on the server), so it can usually find the
agent without being told anything.

Then, on the phone (Termux):

    pip install ovyth-agent   # or: python3 phone.py <host> [port] [token]
    ovyth-agent

This is deliberately the dumbest thing that works: one socket, one line at a
time, whatever the server says is printed and whatever you type is sent. There
is no protocol to get out of sync, and the agent's own output — the tool calls,
the exit codes, the replies — is what you see, in order.

Security: this is a remote shell. The token is the only thing standing between
your phone and arbitrary code execution on the PC, so it is passed as an
argument, never written to disk, and never printed.
"""
import os
import socket
import sys
import threading


def find_port(host: str) -> int | None:
    """The port the agent published, if it published one."""
    for candidate in (
        os.environ.get("OVYTH_AGENT_PORT_FILE", ""),
        os.path.expanduser("~/.ovyth/agent.port"),
    ):
        if candidate and os.path.isfile(candidate):
            try:
                return int(open(candidate).read().strip())
            except (ValueError, OSError):
                pass
    return None


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    host = args[0] if args else os.environ.get("OVYTH_AGENT_HOST", "127.0.0.1")
    port = int(args[1]) if len(args) > 1 else (find_port(host) or 8642)
    token = args[2] if len(args) > 2 else os.environ.get("OVYTH_AGENT_TOKEN", "")

    try:
        sock = socket.create_connection((host, port), timeout=30)
    except OSError as exc:
        print(f"cannot reach the agent at {host}:{port} ({exc})")
        print("is it running? start it with:")
        print("  OVYTH_AGENT_SERVE=0 ovc agent")
        return 1
    sock.settimeout(None)  # the agent thinks for as long as it needs

    # Everything the agent says, until the socket closes.
    def reader() -> None:
        try:
            while True:
                data = sock.recv(4096)
                if not data:
                    break
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
        except OSError:
            pass
        finally:
            print("\n[agent disconnected]")
            os._exit(0)

    threading.Thread(target=reader, daemon=True).start()
    reader_thread = [t for t in threading.enumerate() if t is not threading.current_thread()][-1]

    if token:
        sock.sendall(token.encode() + b"\n")

    try:
        for line in sys.stdin:
            sock.sendall(line.encode() + b"\n")
    except (KeyboardInterrupt, OSError):
        pass
    finally:
        # The reader ends when the agent closes the socket — after ":quit" it
        # sends "bye" and does exactly that. Waiting for it is what keeps the
        # last reply from being cut off by this process exiting first.
        reader_thread.join(timeout=30)
        try:
            sock.close()
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
