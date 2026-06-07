# SPDX-License-Identifier: Apache-2.0
"""Dependency-free wire-protocol clients for fastcached benchmarking.

Three clients speak fastcached's three wire protocols directly over a raw
socket — no redis / memcached client libraries are required, which keeps the
benchmark portable and reproducible across Windows, Linux, and macOS.

Every client exposes the same small surface so the load generator can drive any
protocol uniformly::

    client = make_client("redis", "127.0.0.1", 11211)
    client.set(b"k", b"v")
    client.get(b"k")
    client.delete(b"k")
    client.incr(b"counter", 1)   # raises Unsupported on Redis
    client.ping()
    client.close()

Each call performs one blocking request -> response round trip, which is what
the latency measurement times. Sockets are created with ``TCP_NODELAY`` so the
client never adds Nagle latency of its own; the server is the variable under
test.
"""

from __future__ import annotations

import socket
import struct

# fastcached auto-detects the protocol from the first byte a client sends
# (see ProtocolAutodetect.cpp): 0x80 -> memcached binary, one of *+-:$ -> Redis
# RESP2, anything else -> memcached text.
PROTOCOLS = ("memcached-text", "memcached-binary", "redis")


class ProtocolError(RuntimeError):
    """Raised when the server returns a response the client cannot parse."""


class Unsupported(RuntimeError):
    """Raised when an operation is not available for the chosen protocol."""


class _Connection:
    """A buffered reader/writer over a TCP socket.

    Provides line-oriented reads (for the text and RESP protocols) and
    fixed-length reads (for the binary protocol) on top of a single recv
    buffer, so a server reply that arrives split across packets is reassembled
    transparently.
    """

    def __init__(self, host: str, port: int, timeout: float) -> None:
        self._socket = socket.create_connection((host, port), timeout=timeout)
        self._socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._buffer = bytearray()

    def send(self, data: bytes) -> None:
        self._socket.sendall(data)

    def _fill(self) -> None:
        chunk = self._socket.recv(65536)
        if not chunk:
            raise ProtocolError("connection closed by server")
        self._buffer.extend(chunk)

    def read_line(self) -> bytes:
        """Read one CRLF-terminated line, returning it without the CRLF."""
        while True:
            index = self._buffer.find(b"\r\n")
            if index >= 0:
                line = bytes(self._buffer[:index])
                del self._buffer[: index + 2]
                return line
            self._fill()

    def read_exact(self, count: int) -> bytes:
        """Read exactly ``count`` bytes."""
        while len(self._buffer) < count:
            self._fill()
        data = bytes(self._buffer[:count])
        del self._buffer[:count]
        return data

    def close(self) -> None:
        try:
            self._socket.close()
        except OSError:
            pass


class MemcachedTextClient:
    """Speaks the classic memcached text protocol (CRLF lines)."""

    def __init__(self, host: str, port: int, timeout: float) -> None:
        self._conn = _Connection(host, port, timeout)

    def set(self, key: bytes, value: bytes) -> bool:
        self._conn.send(b"set %b 0 0 %d\r\n%b\r\n" % (key, len(value), value))
        return self._conn.read_line() == b"STORED"

    def get(self, key: bytes) -> bytes | None:
        self._conn.send(b"get %b\r\n" % key)
        header = self._conn.read_line()
        if header == b"END":
            return None
        if not header.startswith(b"VALUE "):
            raise ProtocolError(f"unexpected get reply: {header!r}")
        length = int(header.split(b" ")[3])
        payload = self._conn.read_exact(length + 2)  # value + CRLF
        terminator = self._conn.read_line()  # END
        if terminator != b"END":
            raise ProtocolError(f"missing END terminator: {terminator!r}")
        return payload[:length]

    def delete(self, key: bytes) -> bool:
        self._conn.send(b"delete %b\r\n" % key)
        return self._conn.read_line() == b"DELETED"

    def incr(self, key: bytes, delta: int) -> int | None:
        self._conn.send(b"incr %b %d\r\n" % (key, delta))
        reply = self._conn.read_line()
        if reply == b"NOT_FOUND":
            return None
        return int(reply)

    def ping(self) -> bool:
        self._conn.send(b"version\r\n")
        return self._conn.read_line().startswith(b"VERSION")

    def flush(self) -> bool:
        self._conn.send(b"flush_all\r\n")
        return self._conn.read_line() == b"OK"

    def close(self) -> None:
        self._conn.close()


class RedisRespClient:
    """Speaks Redis RESP2. Note: fastcached has no INCR over Redis."""

    def __init__(self, host: str, port: int, timeout: float) -> None:
        self._conn = _Connection(host, port, timeout)

    @staticmethod
    def _encode(*args: bytes) -> bytes:
        parts = [b"*%d\r\n" % len(args)]
        for arg in args:
            parts.append(b"$%d\r\n%b\r\n" % (len(arg), arg))
        return b"".join(parts)

    def _read_reply(self) -> object:
        line = self._conn.read_line()
        kind, rest = line[:1], line[1:]
        if kind in (b"+", b"-", b":"):
            return rest
        if kind == b"$":
            length = int(rest)
            if length < 0:
                return None
            payload = self._conn.read_exact(length + 2)
            return payload[:length]
        if kind == b"*":
            count = int(rest)
            return [self._read_reply() for _ in range(count)] if count >= 0 else None
        raise ProtocolError(f"unexpected RESP reply: {line!r}")

    def set(self, key: bytes, value: bytes) -> bool:
        self._conn.send(self._encode(b"SET", key, value))
        return self._read_reply() == b"OK"

    def get(self, key: bytes) -> bytes | None:
        self._conn.send(self._encode(b"GET", key))
        reply = self._read_reply()
        return reply if reply is None or isinstance(reply, bytes) else None

    def delete(self, key: bytes) -> bool:
        self._conn.send(self._encode(b"DEL", key))
        return self._read_reply() == b"1"

    def incr(self, key: bytes, delta: int) -> int | None:
        raise Unsupported("Redis INCR is not implemented by fastcached")

    def ping(self) -> bool:
        self._conn.send(self._encode(b"PING"))
        return self._read_reply() == b"PONG"

    def flush(self) -> bool:
        self._conn.send(self._encode(b"FLUSHALL"))
        return self._read_reply() == b"OK"

    def close(self) -> None:
        self._conn.close()


class MemcachedBinaryClient:
    """Speaks the memcached binary protocol (24-byte big-endian header)."""

    _REQUEST_MAGIC = 0x80
    _HEADER = struct.Struct(">BBHBBHIIQ")  # magic,opcode,keylen,extlen,dtype,status,bodylen,opaque,cas
    _OP_GET = 0x00
    _OP_SET = 0x01
    _OP_DELETE = 0x04
    _OP_INCREMENT = 0x05
    _OP_FLUSH = 0x08
    _OP_VERSION = 0x0B
    _STATUS_OK = 0x00
    _STATUS_KEY_NOT_FOUND = 0x01

    def __init__(self, host: str, port: int, timeout: float) -> None:
        self._conn = _Connection(host, port, timeout)

    def _send(self, opcode: int, key: bytes = b"", value: bytes = b"", extras: bytes = b"") -> None:
        body_len = len(extras) + len(key) + len(value)
        header = self._HEADER.pack(
            self._REQUEST_MAGIC, opcode, len(key), len(extras), 0, 0, body_len, 0, 0
        )
        self._conn.send(header + extras + key + value)

    def _recv(self) -> tuple[int, int, bytes]:
        """Return (status, extras_len, body) of one response packet."""
        raw = self._conn.read_exact(self._HEADER.size)
        _, _, _, extras_len, _, status, body_len, _, _ = self._HEADER.unpack(raw)
        body = self._conn.read_exact(body_len)
        return status, extras_len, body

    def set(self, key: bytes, value: bytes) -> bool:
        self._send(self._OP_SET, key, value, extras=struct.pack(">II", 0, 0))
        status, _, _ = self._recv()
        return status == self._STATUS_OK

    def get(self, key: bytes) -> bytes | None:
        self._send(self._OP_GET, key)
        status, extras_len, body = self._recv()
        if status == self._STATUS_KEY_NOT_FOUND:
            return None
        if status != self._STATUS_OK:
            raise ProtocolError(f"binary get status {status:#x}")
        return body[extras_len:]

    def delete(self, key: bytes) -> bool:
        self._send(self._OP_DELETE, key)
        status, _, _ = self._recv()
        return status == self._STATUS_OK

    def incr(self, key: bytes, delta: int) -> int | None:
        extras = struct.pack(">QQI", delta, 0, 0)  # delta, initial, exptime(fail-on-miss=0)
        self._send(self._OP_INCREMENT, key, extras=extras)
        status, _, body = self._recv()
        if status == self._STATUS_KEY_NOT_FOUND:
            return None
        if status != self._STATUS_OK:
            raise ProtocolError(f"binary incr status {status:#x}")
        return struct.unpack(">Q", body)[0]

    def ping(self) -> bool:
        self._send(self._OP_VERSION)
        status, _, _ = self._recv()
        return status == self._STATUS_OK

    def flush(self) -> bool:
        self._send(self._OP_FLUSH)
        status, _, _ = self._recv()
        return status == self._STATUS_OK

    def close(self) -> None:
        self._conn.close()


_CLIENTS = {
    "memcached-text": MemcachedTextClient,
    "memcached-binary": MemcachedBinaryClient,
    "redis": RedisRespClient,
}

# Operations each protocol can drive (used by workloads to stay within support).
SUPPORTED_OPS = {
    "memcached-text": frozenset({"set", "get", "delete", "incr"}),
    "memcached-binary": frozenset({"set", "get", "delete", "incr"}),
    "redis": frozenset({"set", "get", "delete"}),
}


def make_client(protocol: str, host: str, port: int, timeout: float = 5.0):
    """Connect a client for ``protocol`` to ``host:port``.

    :raises KeyError: if ``protocol`` is not one of :data:`PROTOCOLS`.
    """
    return _CLIENTS[protocol](host, port, timeout)
