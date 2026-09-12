# Vendored third-party source

Everything under `vendor/` is third-party code copied **verbatim** from upstream.
It is not this project's code, it is not formatted, analysed or styled by this
project's rules, and it is kept byte-identical to a named upstream commit so that
improvements made here can be sent back.

That last clause is the reason for all the rest. `fastcached` is downstream of two
repositories the same author maintains, so a fix made here is worth more upstream
than it is here — but only while the copy is still diffable against upstream. A
copy that has been reformatted, renamed, re-namespaced or "just slightly adapted"
cannot be contributed back, and the ability is lost silently: nothing fails, the
build stays green, and the discovery comes months later when somebody tries.

## What is here, and where it came from

**Two upstreams.** This is the single most important fact in this file, because
the directory is called `endo` and four fifths of it is endo's — so a reader will
assume one fork point, and be wrong about five files in a way that is invisible
from the contents.

| upstream | fork point | files | what |
|---|---|---|---|
| [contour-terminal/endo](https://github.com/contour-terminal/endo) | `687b90a042a3eee96f50f8f46cf811a03155b786` | 161 | `tui/**` (154 files, all of `src/tui`), `platform/{Types,Wakeup,Clock,SignalHandler,SystemPipe,PlatformError}.hpp`, `testing/SuppressWindowsDialogs.hpp` |
| [contour-terminal/contour](https://github.com/contour-terminal/contour) | `243d776aed99bcc0f634b0608189609fd4e548ba` | 5 | `coro/{Task,Cancellation,WhenAny,UniqueCoroHandle}.hpp`, `crispy/FNV.hpp` |


**Why `coro/` and `crispy/` come from contour and not from endo:** endo's own
`.gitignore` lists `/src/crispy` and `/src/coro`. They are not part of endo's
repository — endo fetches them in from contour and, in its own CMakeLists' words,
they are "overwritten on every fetch". So endo's copy is a working copy that no
endo commit describes, and **a change to those five files is a pull request against
contour, not against endo.** They were verified byte-identical between contour
`243d776a` and endo's working copy before the earlier provenance was chosen.

Both repositories are Apache-2.0, and so is this copy. `endo/tui` was last touched
upstream at `37d875f8` (2026-08-15).

`src/tui` upstream is 154 files and ~46k lines; with the twelve headers below the
copy is **166 files, 47,643 lines**.

## The twelve extra headers

`tui` does not stand alone: it includes headers from three sibling libraries. The
set copied here is the **measured transitive closure** of those includes — every
non-`tui`, non-system header reachable from `src/tui`, followed until it
terminates. It is twelve headers and 1,597 lines, all header-only, and none of
them reaches any further into either upstream.

That number is worth stating because the obvious estimate is an order of magnitude
larger. `coro` and `endo-platform` are together about 12,000 lines, and "vendor the
libraries `tui` links" would have meant taking all of it. `tui` reaches a small leaf
set of them, so that is what is here.

## Two of some things — read this before using any of it

This tree now contains, on disk, **a second coroutine library, a second
cancellation type and a second clock**, beside the ones in `src/FastCache/Async`
and `src/FastCache/Core`.

For fastcached code the authoritative ones are **always** the first-party ones:
`FastCache::Async::Task`, `FastCache::Async::Cancellation`, `FastCache::IClock`.
Nothing under `src/` may include `<coro/...>` or `<platform/Clock.hpp>`.

The vendored copies exist for exactly one reason: to keep the vendored sources
compiling **unmodified**. They are the price of the verbatim constraint, not a
second opinion about how to write a coroutine.

Two facts that make this much less alarming than it sounds, both measured:

- **`coro/*` and `platform/Clock.hpp` are reached only from `tui/runtime/`, and
  `tui/runtime/` is not built.** They sit on disk so the copy is include-closed and
  a later ticket that wants the runtime needs no second import. **Nothing compiles
  them today.** The built library's entire external surface is libunicode,
  `platform/Types.hpp`, `platform/Wakeup.hpp` and `crispy/FNV.hpp`.
- **The two vocabularies meet in one place.** Vendored code serves the vendored TUI
  only; no vendored file reaches `FastCache::*`, and first-party code reaches the TUI
  through a single adapter layer. If you find yourself wanting to cross that boundary
  somewhere else, that is the signal to widen the adapter, not to add a second
  crossing.

## Local changes

**None.** All 166 files are byte-identical to their upstream blobs, which is
checked rather than asserted — see below.

Every local change goes in its own commit, never folded into the import, and gets a
row here saying what, why, and which upstream it belongs to.

## How to send a change back

**Diff against UPSTREAM, not against a local commit.** The import commit is the
obvious anchor and it is the wrong one: this project rebases every branch before it
merges, so that SHA does not survive, and a range anchored to a commit that no
longer exists fails by printing an error about an unknown revision — months later,
to whoever is trying to upstream a fix. The fork points in the table above are
immutable and are already the thing being diffed *from*.

1. Work out which upstream owns the file, from the table above. `coro/` and
   `crispy/` are contour's; everything else is endo's.
2. Take the diff against that upstream at its fork point. This tree's
   `vendor/endo/X` is upstream's `src/X` in both cases:

   ```sh
   git -C <upstream> -c core.autocrlf=false -c core.eol=lf show <fork-point>:src/<path> \
       | diff -u --label "a/src/<path>" --label "b/src/<path>" - vendor/endo/<path>
   ```

3. Open the pull request against that repository.
4. When it merges, re-sync (below) and delete the row from "Local changes".

## How to re-sync

Read upstream **blobs**, never a working tree. Both source clones used for the
original import ran `core.autocrlf=true` and neither upstream ships a
`.gitattributes`, so their working trees hold CRLF while their blobs hold LF —
46,046 CR bytes across `src/tui` alone, belonging to the clone rather than to
upstream. `git archive` applies that conversion too. A copy taken from a working
tree is a copy of one machine's checkout settings, and `.gitattributes` here would
then normalise it on commit, so the difference only shows up as a failed
byte-identity check afterwards — or, worse, not at all.

```sh
git -C <endo> -c core.autocrlf=false -c core.eol=lf archive <sha> src/tui | tar -x --strip-components=1 -C vendor/endo
git -C <repo> -c core.autocrlf=false -c core.eol=lf show <sha>:src/<path> > vendor/endo/<path>
```

Then compare every file against its upstream blob and **assert the count**, because
a loop over an empty set also reports no differences. Update the fork points above.

## What the build does with it

`vendor/CMakeLists.txt` — which is **ours**, not upstream's — declares one static
library, `fastcache-tui`, from 21 translation units per platform.

`endo/tui/CMakeLists.txt` is part of the verbatim copy and is deliberately **not**
used: it names `endo-platform`, `coro`, `stb_image` and three endo-only CMake
helper functions, none of which exist here, and editing it would put a local change
inside the contribution diff on day one.

Every vendored translation unit this target does **not** build is named in that
file's `_fcTuiNotBuilt`, with one of three reasons: it reaches `stb_image` (image
decoding), it reaches `coro` (the event runtime), or nothing on a stats panel's
path reaches it — an interactive editor's widgets, present and working and simply
not called here.

The two lists are asserted at configure time to **partition** the vendored `.cpp`
files on disk, so the counts are derived rather than written down, and a re-sync
that adds a file refuses by name instead of leaving it built by nothing and
classified by nothing. That assertion exists because the prose version of this
paragraph was wrong on the day it was written: it claimed 40 upstream units and 19
excluded, where upstream's target is 48 per platform and 27 are excluded, and
`HyperlinkEmitter.cpp` appeared in no list and no rationale at all.

Unbuilt sources remain **on disk, verbatim** — subsetting the build is not
subsetting the copy — and building one is moving its row from one list to the
other, not a new import.

`libunicode` is a genuine new third-party dependency, fetched `find_package`-first
then CPM like every other, pinned at **0.9.3** as a floor: earlier versions have a
`scan_text()` bug that drops a zero-width codepoint following an ASCII base, and a
terminal view that mismeasures one cluster corrupts every column to its right.

## How `vendor/` is kept out of this project's rules

The convention is simply that **everything under `vendor/` is third-party**. It is
spelled once in each tool that needs it, because they cannot share a variable, and
each spelling names this directory:

| where | what it does | what happens without it |
|---|---|---|
| `.clang-format-ignore` | the formatter declines the path, for every caller | `clang-format -i` rewrites **146 of 165** vendored files on the first gate run, and verbatim is gone on day one |
| `.clang-tidy` `HeaderFilterRegex` | already an inclusion list naming `src/` only, so `vendor/` is outside it | `WarningsAsErrors: "*"` fails the build on naming rules upstream has no reason to satisfy |
| `scripts/local-gate.sh` | splits tracked headers into first-party and vendored, and asks each half its own question | the gate refuses every tree carrying a vendored header, advising a fix that is itself the `deps-leak` defect |
| `scripts/check-succeed-not-skip.cmake` | `src/`-anchored, so it scans this repository's own tests | the first upstream sync adding a `SUCCEED` reddens a check about *our* code |
| `vendor/CMakeLists.txt` | added before the pedantic/clang-tidy includes, and clears `CXX_CLANG_TIDY` on the target as well | see the two rows above |


`local-gate.sh` also refuses when `vendor/` exists but git tracks nothing inside it:
a convention that has stopped describing anything reads exactly like one being
honoured, and the two halves of that split would then be taken over an empty set.
