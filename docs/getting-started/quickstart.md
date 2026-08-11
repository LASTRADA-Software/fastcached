# Quickstart

Start fastcached and round-trip a value via `telnet`.

## 1. Run the daemon

```sh
./fastcached
```

It listens on `127.0.0.1:6674` — fastcached's own port. The number selects no
protocol: everything below reaches the same daemon on the same port, because
the wire format is detected per connection.

## 2. Store and fetch a value (ASCII text protocol)

```text
$ telnet 127.0.0.1 6674
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
$ redis-cli -p 6674
127.0.0.1:6674> SET greeting hello EX 60
OK
127.0.0.1:6674> GET greeting
"hello"
```

See [Protocols overview](../protocols/overview.md) for the
auto-detection rules.
