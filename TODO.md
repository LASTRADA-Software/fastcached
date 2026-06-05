# TODO items

- [ ] CoW storage needs to expose the on-disk format version, such that we can identify old stores on newer fastcached versions with a newer on-disk format (to gracefully bail, or migrate)
- [ ] Add valgrind memcheck CI job to ensure no memory issues on clang release build
- [ ] Github CI should create a static `fastcached` binary and provide it as artifact.
- [ ] Add `fastcached stats` command to show in-process stats of the currently running instance (assuming we auto-detect it via config file)
- [ ] Add `fastcached live-stats` command to show in-process stats live on the terminal (using Sixels, if supported by the connected terminal, otherwise Unicode or ASCII art)
- [x] Add support for Tracy (via CMake variable, off by default)
- [x] disk storage should be btrfs-like copy-on-write to allow O(1) blocking disk saves
