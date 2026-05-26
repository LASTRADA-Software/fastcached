# TODO items

- [ ] Add support for Tracy (via CMake variable, off by default)
- [ ] Add valgrind memcheck CI job to ensure no memory issues on clang release build
- [ ] disk storage should be btrfs-like copy-on-write to allow O(1) blocking disk saves
- [ ] create a `fastcached.service` file for Systemd, possibly also a `fastcached.sock` for activation.
- [ ] Github CI should create a static `fastcached` binary and provide it as artifact.
- [ ] Add `fastcached stats` command to show in-process stats of the currently running instance (assuming we auto-detect it via config file)
