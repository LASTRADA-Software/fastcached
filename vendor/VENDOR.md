# Vendored third-party source

Everything under `vendor/` other than this file and `MANIFEST` is third-party code copied
**verbatim** from upstream. It is not this project's code, it is not formatted, analysed or styled
by this project's rules, and it is kept byte-identical to a named upstream release so that
improvements made here can be sent back -- and, for a cryptographic library above all, so that the
upstream audit still describes what is compiled here.

## The copies

Each row is one third-party root of `scripts/lib/third-party-roots.txt`: a directory holding
one verbatim copy, hashed file by file into `MANIFEST` and checked by `ctest -R vendor-verbatim`.
The two counts are recomputed from the tree by `ctest -R vendor-figures`, for every root the roots
file names, so a new copy needs a row here the day it arrives.

| root | upstream | files | lines |
|---|---|---|---|
| `vendor/monocypher` | Monocypher 4.0.3, "`vendor/monocypher`" below | 6 | 4,182 |

**`vendor/endo` is gone (#1596).** It was a verbatim copy of endo's terminal UI, with the slice of
endo's platform layer and contour's coroutine vocabulary that UI needed, behind `fastcache-cli
live-stats`. That code is core-cpp's now (`core::tui`, fetched by CPM at a release tag), and every
local change this copy carried is in core-cpp's v0.1.0: the four merged into endo as
[contour-terminal/endo#184](https://github.com/contour-terminal/endo/pull/184), which core-cpp
imported at `f774a210`, and #1546's `DelayAwaiter` change, which core-cpp took from this tree's copy.
A fix to that code is a pull request against
[contour-terminal/core-cpp](https://github.com/contour-terminal/core-cpp), never an edit here.

## `vendor/monocypher`: Monocypher 4.0.3

The Ed25519 and X25519 implementation behind `src/FastCache/Core/{Ed25519,X25519}` (#178), which
give every node an identity of its own in place of the cluster's shared key.

| | |
|---|---|
| upstream | [Monocypher](https://monocypher.org), source at [LoupVaillant/Monocypher](https://github.com/LoupVaillant/Monocypher) |
| version | **4.0.3**, released 2026-06-15. Its changelog's first line is "Fixed timing leak vulnerability in EdDSA/Ed25519 signatures", which is the reason for no earlier version |
| taken from | the release tarball `monocypher-4.0.3.tar.gz`, from `https://monocypher.org/download/` |
| tarball sha256 | `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66` |
| tarball sha512 | `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965` |
| tag | `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f` |
| licence | dual, BSD-2-Clause OR CC0-1.0 at the user's choice (`LICENCE.md`, kept verbatim) |

**How that provenance was established, so the next re-sync can repeat it rather than trust it.**
The GitHub release asset and the monocypher.org download are byte-identical (the sha256 above),
and the tarball matches the sha512 monocypher.org publishes beside it. Its sources match the git
tag apart from the release's version stamp -- the tag's sources say `__git__` where the tarball's
say `4.0.3` -- and a `LICENCE.md` note about test files the tarball does not ship. So the copy is
taken from the TARBALL: it is what upstream released, and the tag is not byte-for-byte that.

**Six files, and why only these.** `src/monocypher.c`, `src/monocypher.h`,
`src/optional/monocypher-ed25519.c` and `src/optional/monocypher-ed25519.h` are what the build
compiles -- the RFC 8032 Ed25519 this tree uses is the `optional/` half, over SHA-512, not the
library's default EdDSA over BLAKE2b. `LICENCE.md` and `AUTHORS.md` are the licence and the
attribution. The tests, the manual, the makefile, the README and the changelog build nothing and
attribute nothing, so they are left upstream; each file's hash is its `MANIFEST` line.

**What the build does with it.** `cmake/Monocypher.cmake`, which is ours, declares one static
library, `fastcache-monocypher`, from the two `.c` files compiled as C++ with upstream's
`MONOCYPHER_CPP_NAMESPACE` -- that file says why C++, why the namespace, and why it is built
unconditionally. It has no dependency, so `fastcache-cc` can
link it without FastCache.

**Who may use it.** Only `src/FastCache/Core/Ed25519.cpp` and `src/FastCache/Core/X25519.cpp`
include a Monocypher header; `ctest -R crypto-seam` refuses any other first-party file that does.
Everything else goes through those two headers and `Core/Hkdf.hpp`, which is where a new primitive
is added and checked against its RFC's vectors.

**How to re-sync, and how to send a change back.** Download the new release tarball, check it
against the sha512 monocypher.org publishes, and extract the six files from the ARCHIVE (for
instance `tar -xzOf monocypher-X.Y.Z.tar.gz monocypher-X.Y.Z/<path> > vendor/monocypher/<path>`),
never from a checkout, whose sources carry `__git__`. Then
`cmake -DFASTCACHED_SOURCE_DIR=<root> -DFASTCACHED_VENDOR_WRITE_MANIFEST=RESYNC -DFASTCACHED_VENDOR_RESYNC_ROOT=vendor/monocypher -P scripts/check-vendor-verbatim.cmake`,
update the table above and the row in "The copies", and run the vectors. A change goes to
LoupVaillant/Monocypher as a pull request first; a local change would be a row below, and there is
none.

## Local changes

None: all six files of `vendor/monocypher` are byte-identical to the release. `vendor/MANIFEST`
records each file's UPSTREAM hash beside its current one, and `ctest -R vendor-verbatim` refuses a
file that differs from upstream unless a row of a table in this section names it, and refuses a row
naming a file that does not.

A local change goes in its own commit, never folded into the import, and gets a row here -- a table
whose columns are the files, what, why, upstream and how it was prepared, each file in backticks --
saying what, why, and which upstream it belongs to. **The table is read by that check**: a file counts as declared only when a
row names it in backticks, so prose here explains and declares nothing.

## How `vendor/` is kept out of this project's rules

The convention is simply that **everything under `vendor/` is third-party**. It is
spelled once in each tool that needs it, because they cannot share a variable, and
each spelling names this directory:

| where | what it does | what happens without it |
|---|---|---|
| `.clang-format-ignore` | the formatter declines the path, for every caller | `clang-format -i` rewrites **146 of 165** vendored files on the first gate run, and verbatim is gone on day one |
| `.clang-tidy` `HeaderFilterRegex` | already an inclusion list naming `src/` only, so `vendor/` is outside it -- and it names this project's own directories UNDER `src/`, which is what keeps `vendor/monocypher/src/` outside it too | `WarningsAsErrors: "*"` fails the build on naming rules upstream has no reason to satisfy |
| `scripts/local-gate.sh` | splits tracked headers into first-party and vendored, and asks each half its own question | the gate refuses every tree carrying a vendored header, advising a fix that is itself the `deps-leak` defect |
| `scripts/check-succeed-not-skip.cmake` | `src/`-anchored, so it scans this repository's own tests | the first upstream sync adding a `SUCCEED` reddens a check about *our* code |
| `cmake/Monocypher.cmake` | included before the pedantic/clang-tidy includes, clears `CXX_CLANG_TIDY`, and makes the include directories `SYSTEM` | the same, for Monocypher |


`local-gate.sh` also refuses when `vendor/` exists but git tracks nothing inside it:
a convention that has stopped describing anything reads exactly like one being
honoured, and the two halves of that split would then be taken over an empty set.
