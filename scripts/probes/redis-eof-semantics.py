#!/usr/bin/env python3
"""Determine what a REFERENCE Redis does when a client half-closes.

This is the measurement behind the EOF rule in `.agent/rules/wire-and-protocol.md`.
It is committed so the rule can be re-run rather than re-argued: a citation people
can check is one they stop litigating, and a figure without its conditions outlives
its own truth.

The question (#671): on a request/reply surface, does EOF-without-close mean
"this peer is gone" or "this peer has finished sending"?

    Usage:
        redis-server --port 6390 --save "" --appendonly no --daemonize yes
        python3 scripts/probes/redis-eof-semantics.py 6390
        redis-cli -p 6390 shutdown nosave

Cases 6 and 7 shell out to `redis-cli`; the rest need nothing but the standard
library. Every scenario has a control, because a null result that cannot be
distinguished from a negative answer is not evidence -- case 2 is what makes case 1
mean something, and case 7 is what makes case 6 mean something.

Recorded result, `redis-server 7.0.15` on `Linux 5.15.167.4-microsoft-standard-WSL2`,
stock configuration:

    1 -> server closed, no reply      5 -> +OK, $7 written, then EOF
    2 -> reply delivered              6 -> blocked clients 1 -> 0
    3 -> +PONG delivered              7 -> blocked client stays
    4 -> server closed promptly

which is the rule: EOF means finished-sending; a server answers what is already
determined and abandons what is still pending.
"""

import os
import socket
import subprocess
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 6390

# **Every key is unique per run, and that is load-bearing rather than tidy.**
# Cases 1, 2 and 5 push values nobody ever pops, so against a server that is not
# restarted between runs the second run's BLPOP returns instantly from the first
# run's leftover -- printing "reply delivered" for case 1, which is the OPPOSITE
# of the recorded observation and of the rule this probe is committed to support.
# A probe cited as re-runnable that reverses its own verdict on the second run is
# worse than no citation. Unique names rather than a DEL sweep, because two people
# running it against one server must not disturb each other either.
RUN = "%d-%d" % (os.getpid(), time.time_ns() % 1_000_000)


def key(name):
    """A key private to this run. See RUN."""
    return "eof671-%s-%s" % (name, RUN)


def connect():
    return socket.create_connection(("127.0.0.1", PORT), timeout=10)


def resp(*args):
    """Encode a RESP array command."""
    out = ("*%d\r\n" % len(args)).encode()
    for a in args:
        b = a.encode()
        out += b"$%d\r\n" % len(b) + b + b"\r\n"
    return out


def read_reply(sock, seconds=4.0):
    sock.settimeout(seconds)
    try:
        data = sock.recv(400)
    except socket.timeout:
        return "<TIMEOUT: nothing arrived>"
    if data == b"":
        return "<EOF: server closed with no reply>"
    return repr(data)


def drain(sock, seconds=3.0):
    sock.settimeout(seconds)
    chunks = []
    while True:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            chunks.append(b"<TIMEOUT>")
            break
        if data == b"":
            chunks.append(b"<EOF>")
            break
        chunks.append(data)
    return b"".join(chunks)


def blocking_case(key, half_close):
    """BLPOP, optionally half-close, then push from a second connection."""
    a = connect()
    a.sendall(resp("BLPOP", key, "0"))
    time.sleep(0.4)  # let the server register the block
    if half_close:
        a.shutdown(socket.SHUT_WR)
        time.sleep(0.4)  # let the server observe the EOF, if it acts on it
    b = connect()
    b.sendall(resp("LPUSH", key, "value-for-" + key))
    b.settimeout(5)
    b.recv(100)
    out = read_reply(a)
    a.close()
    b.close()
    return out


def client_list():
    return subprocess.run(["redis-cli", "-p", str(PORT), "CLIENT", "LIST"],
                          capture_output=True, text=True).stdout


def blocked_rows(listing):
    return [ln for ln in listing.splitlines() if "cmd=blpop" in ln]


print("1. BLPOP + HALF-CLOSE, then a push arrives")
print("   ->", blocking_case(key("halfclosed"), half_close=True))

print("\n2. BLPOP, NO half-close, then a push arrives   [control]")
print("   ->", blocking_case(key("open"), half_close=False))

print("\n3. PING + immediate HALF-CLOSE")
c = connect()
c.sendall(resp("PING"))
c.shutdown(socket.SHUT_WR)
print("   ->", read_reply(c))
c.close()

print("\n4. BLPOP + HALF-CLOSE, nothing ever pushed")
a = connect()
a.sendall(resp("BLPOP", key("lonely"), "0"))
time.sleep(0.4)
a.shutdown(socket.SHUT_WR)
print("   ->", read_reply(a, seconds=3.0))
a.close()

print("\n5. SET + GET + BLPOP pipelined, then half-close (nothing ever pushed)")
print("   separates 'answer what is determined, then close' from 'close on EOF, PING")
print("   only survived by racing the close'")
c = connect()
c.sendall(resp("SET", key("probe5"), "written") + resp("GET", key("probe5")) + resp("BLPOP", key("probe5-block"), "0"))
c.shutdown(socket.SHUT_WR)
print("   ->", drain(c))
c.close()

print("\n6. Is the blocked-and-half-closed client gone from the SERVER's own view?")
a = connect()
a.sendall(resp("BLPOP", key("probe6-block"), "0"))
time.sleep(0.4)
before = client_list()
a.shutdown(socket.SHUT_WR)
time.sleep(1.0)
after = client_list()
print("   blocked clients before half-close:", len(blocked_rows(before)))
print("   blocked clients after  half-close:", len(blocked_rows(after)))
a.close()

print("\n7. Control for 6: a blocked client that does NOT half-close stays")
b = connect()
b.sendall(resp("BLPOP", key("probe7-block"), "0"))
time.sleep(0.4)
print("   blocked clients while attached:  ", len(blocked_rows(client_list())))
b.close()
