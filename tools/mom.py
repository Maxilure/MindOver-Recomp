#!/usr/bin/env python3
"""mom.py -- client for the game's debug console (src/debug_console.h).

Start the game with --debug_console=<socket path>, then:

    tools/mom.py -s <socket> state
    tools/mom.py -s <socket> players
    tools/mom.py -s <socket> "read [0x8259B190]+80 u32 4"
    tools/mom.py -s <socket> "wait state 153 30000" a "wait exit"
    tools/mom.py -s <socket>            (no command: interactive, one per line)

Each argument is one command line, sent in order on one connection; the
replies are printed. The socket can also come from $MOM_SOCKET. Exit code: 0
when every command answered "@end ok", 1 at the first error (the rest are not
sent), 2 when the game isn't reachable. --wait-socket N waits up to N seconds
for the socket to appear (a game that is still starting).
"""

import argparse
import os
import socket
import sys
import time


def connect(path, wait_s):
    deadline = time.monotonic() + wait_s
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError as e:
            if time.monotonic() >= deadline:
                print(f"mom.py: can't reach the game at {path}: {e}", file=sys.stderr)
                sys.exit(2)
            time.sleep(0.25)


def run(sock, reader, line):
    """Sends one command, prints its reply; returns True when it succeeded."""
    sock.sendall(line.encode() + b"\n")
    while True:
        reply = reader.readline()
        if not reply:
            print("mom.py: the game closed the connection", file=sys.stderr)
            sys.exit(2)
        reply = reply.rstrip("\n")
        if reply.startswith("@end "):
            status = reply[5:]
            if status == "ok":
                return True
            sys.stdout.flush()  # replies first, then the error (stderr is unbuffered)
            print(f"error: {status[6:]}", file=sys.stderr)
            return False
        print(reply)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-s", "--socket", default=os.environ.get("MOM_SOCKET"))
    ap.add_argument("--wait-socket", type=float, default=0, metavar="SECONDS")
    ap.add_argument("commands", nargs="*")
    args = ap.parse_args()
    if not args.socket:
        ap.error("no socket: pass -s or set MOM_SOCKET")
    sock = connect(args.socket, args.wait_socket)
    reader = sock.makefile("r", encoding="utf-8", errors="replace")
    if args.commands:
        for line in args.commands:
            if not run(sock, reader, line):
                sys.exit(1)
        return
    for line in sys.stdin:  # interactive
        line = line.strip()
        if line:
            run(sock, reader, line)


if __name__ == "__main__":
    main()
