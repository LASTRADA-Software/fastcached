# Quickstart

Start fastcached and round-trip a value via `telnet`.

## 1. Run the daemon

```sh
./fastcached --port 11211
```

## 2. Store and fetch a value (ASCII text protocol)

```text
$ telnet 127.0.0.1 11211
> set greeting 0 60 5
> hello
< STORED
> get greeting
< VALUE greeting 0 5
< hello
< END
> quit
```

## 3. Try the meta protocol

The same connection accepts modern meta commands:

```text
> mg greeting v
< VA 5
< hello
> ms counter 1 T60
> 0
< HD
> ma counter v
< VA 1
< 1
```

## 4. Try it from a Redis client

fastcached's RESP2 handler is reachable on the same port:

```sh
$ redis-cli -p 11211
127.0.0.1:11211> SET greeting hello EX 60
OK
127.0.0.1:11211> GET greeting
"hello"
```

See [Protocols overview](../protocols/overview.md) for the
auto-detection rules.
