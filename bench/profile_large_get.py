# SPDX-License-Identifier: Apache-2.0
"""Tiny driver to profile the large-value GET hot path under callgrind.

Stores one large value, then issues N back-to-back GETs of it over a single
keep-alive connection (memcached text protocol). Kept deliberately small —
callgrind instruments every instruction, so a few thousand ops already give a
clean, deterministic instruction-count profile without a long wall-clock wait.
"""
from __future__ import annotations

import socket
import sys


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 12500
    value_bytes = int(sys.argv[3]) if len(sys.argv) > 3 else 64 * 1024
    gets = int(sys.argv[4]) if len(sys.argv) > 4 else 3000

    payload = bytes((i & 0xFF) for i in range(value_bytes))
    sock = socket.create_connection((host, port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # Store the value once.
    sock.sendall(b"set big 0 0 %d\r\n%s\r\n" % (value_bytes, payload))
    assert _read_line(sock) == b"STORED\r\n"

    # Hot loop: GET the large value `gets` times, fully draining each response.
    get_cmd = b"get big\r\n"
    # Expected response size: "VALUE big 0 <n>\r\n" + value + "\r\n" + "END\r\n"
    header = b"VALUE big 0 %d\r\n" % value_bytes
    expected = len(header) + value_bytes + len(b"\r\n") + len(b"END\r\n")
    for _ in range(gets):
        sock.sendall(get_cmd)
        _read_exactly(sock, expected)

    sock.sendall(b"quit\r\n")
    sock.close()
    print(f"drove {gets} GETs of {value_bytes}-byte value")
    return 0


def _read_line(sock: socket.socket) -> bytes:
    buf = b""
    while not buf.endswith(b"\r\n"):
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf


def _read_exactly(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            break
        buf.extend(chunk)
    return bytes(buf)


if __name__ == "__main__":
    raise SystemExit(main())
