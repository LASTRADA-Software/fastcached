# SPDX-License-Identifier: Apache-2.0
"""Quick concurrent large-value GET throughput probe (wall-clock).

Spawns N threads, each on its own keep-alive connection, hammering GET of a
pre-stored large value for a fixed duration. Prints aggregate ops/sec. Used
for fast A/B checks of server-side changes that callgrind (instruction count)
cannot see — e.g. socket buffer sizing that only matters under real I/O.
"""
from __future__ import annotations

import socket
import sys
import threading
import time


def worker(host: str, port: int, value_bytes: int, stop: threading.Event, counts: list[int], idx: int) -> None:
    s = socket.create_connection((host, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    header = b"VALUE big 0 %d\r\n" % value_bytes
    expected = len(header) + value_bytes + 2 + len(b"END\r\n")
    n = 0
    while not stop.is_set():
        s.sendall(b"get big\r\n")
        got = 0
        while got < expected:
            chunk = s.recv(expected - got)
            if not chunk:
                break
            got += len(chunk)
        n += 1
    counts[idx] = n
    s.close()


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 12900
    value_bytes = int(sys.argv[3]) if len(sys.argv) > 3 else 64 * 1024
    threads_n = int(sys.argv[4]) if len(sys.argv) > 4 else 16
    seconds = float(sys.argv[5]) if len(sys.argv) > 5 else 4.0

    payload = bytes((i & 0xFF) for i in range(value_bytes))
    setup = socket.create_connection((host, port))
    setup.sendall(b"set big 0 0 %d\r\n%s\r\n" % (value_bytes, payload))
    setup.recv(100)
    setup.close()

    stop = threading.Event()
    counts = [0] * threads_n
    threads = [threading.Thread(target=worker, args=(host, port, value_bytes, stop, counts, i))
               for i in range(threads_n)]
    start = time.monotonic()
    for t in threads:
        t.start()
    time.sleep(seconds)
    stop.set()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - start

    total = sum(counts)
    print(f"{total / elapsed:,.0f} ops/sec  ({total} gets in {elapsed:.2f}s, {threads_n} conns, {value_bytes}B)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
