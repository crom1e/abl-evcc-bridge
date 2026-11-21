"""Non-interactive telnet client for salto_bridge.

Usage: SALTO_TOKEN=... python telnet.py <host> [--wait SEC] [command ...]
Logs in with the token, sends the commands and prints everything that comes back
(including log lines) for SEC seconds after the last command.
"""
import os
import socket
import sys
import time


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    host = args.pop(0)
    wait = 3.0
    if len(args) >= 2 and args[0] == "--wait":
        wait = float(args[1])
        args = args[2:]
    token = os.environ.get("SALTO_TOKEN", "")
    if not token:
        print("SALTO_TOKEN is not set", file=sys.stderr)
        return 2

    s = socket.create_connection((host, 23), timeout=5)
    s.settimeout(0.2)

    def drain(seconds):
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = s.recv(4096)
            except socket.timeout:
                continue
            if not data:
                return False
            sys.stdout.write(data.decode("utf-8", "replace").replace("\r\n", "\n"))
            sys.stdout.flush()
        return True

    drain(0.5)
    s.sendall((token + "\n").encode())
    if not drain(0.5):
        return 1
    for cmd in args:
        s.sendall((cmd + "\n").encode())
        if not drain(0.8):
            return 1
    drain(wait)
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
