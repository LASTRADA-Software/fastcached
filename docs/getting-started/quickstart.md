# Quickstart

Start fastcached and round-trip a value with the client that ships beside it.

## 1. Run the daemon

```sh
./fastcached
```

It listens on `127.0.0.1:6674` — fastcached's own port. The number selects no
protocol: everything below reaches the same daemon on the same port, because
the wire format is detected per connection.

## 2. Store and fetch a value

`fastcache-cli` is the first-party client — see
[fastcache-cli](../tools/fastcache-cli.md) for the full reference:

```sh
$ fastcache-cli set greeting hello --ttl 60
$ fastcache-cli get greeting
hello
$ fastcache-cli ttl greeting
key     greeting
exists  true
ttl     59
$ fastcache-cli del greeting
1
```

A verb that only succeeds or fails prints nothing and says so through its exit
status; one that answers a question prints the answer, and one that answers
several prints a two-column record.

A miss exits `1`, a daemon that cannot be reached exits `3`, and a usage
mistake exits `2` — so a shell script can tell those three apart:

```sh
$ fastcache-cli get nothing-here; echo "exit=$?"
exit=1
```

## 3. Ask it for stats

Human-readable by default, machine-readable on request:

```sh
$ fastcache-cli stats
source                    info
fastcached_version        fastcached-0.2.0
used_memory               94
maxmemory                 8589934592
total_commands_processed  6
...
$ fastcache-cli stats --format=json
{"source":"info","fastcached_version":"fastcached-0.2.0","used_memory":94,...}
$ fastcache-cli stats --format=tsv
```

The first field is the **source**, because the sources differ enormously in
richness and a number means little without knowing which answered. RESP `INFO`
carries 8 fields; the daemon's `/metrics` endpoint carries over a hundred. Start
the daemon with `--metrics` and the same command reports `source metrics` and far
more of them.

## 4. Speak the protocols by hand

The wire formats are still plain text, which is useful when you are
debugging the protocols themselves rather than the cache.

The ASCII text protocol over `telnet`:

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

The same connection accepts the modern meta commands:

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

And the Redis handler answers `redis-cli` on the same port:

```sh
$ redis-cli -p 6674
127.0.0.1:6674> SET greeting hello EX 60
OK
127.0.0.1:6674> GET greeting
"hello"
```

See [Protocols overview](../protocols/overview.md) for the
auto-detection rules, and [Redis RESP](../protocols/redis-resp.md) for
which Redis commands are answered.
