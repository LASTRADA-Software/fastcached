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
| [contour-terminal/endo](https://github.com/contour-terminal/endo) | `687b90a042a3eee96f50f8f46cf811a03155b786` | 168 | `tui/**` (154 files, all of `src/tui`), `platform/{Types,Wakeup,Clock,SignalHandler,SystemPipe,PlatformError,WinsockInit}.hpp`, the three per-platform `platform/{linux,posix,windows}/*Wakeup.cpp`, `platform/{SignalHandler,SystemPipe,WinsockInit}.cpp`, `testing/SuppressWindowsDialogs.hpp` |
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

`src/tui` upstream is 154 files and ~46k lines; with the nineteen supporting files below and
three local changes' new test files ("Local changes") the copy is **176 files, 49,443 lines**.

## The nineteen supporting files

`tui` does not stand alone: it includes headers from three sibling libraries. The
set copied here is the **measured transitive closure** of those includes — every
non-`tui`, non-system header reachable from `src/tui`, followed until it
terminates. It is nineteen files and 2,438 lines: thirteen headers, plus **six
implementation files** — the three per-platform implementations of
`endo::platform::Wakeup`, which `tui/platform/TerminalInput.cpp` calls into,
`platform/SignalHandler.cpp`, which `tui/runtime/` calls into, and
`platform/{SystemPipe,WinsockInit}.cpp`, which only the vendored runtime TESTS
reach. None of them reaches any further into either upstream.

**Thirteen of the nineteen are header-only, and the closure has been wrong about
that three times — read past the first, because they are not the same mistake.**

The first: `platform/Wakeup.hpp` declares a class whose methods are defined
elsewhere, and reading `#include` lines cannot see that — a header graph and a
symbol graph are different graphs. The build did not see it either: a static
archive never resolves symbols, so it compiled clean and simply carried two
undefined references for whoever linked it first.

The second: `platform/SignalHandler.hpp` is a **pure-static declaration header**,
and its definitions live in a file upstream keeps in a **sibling target** —
`endo-platform`, not the `tui` target this closure is walked from. Nothing about
the header says so. It sits in the same directory as headers whose definitions
*were* imported, and every one of its members being `static` means every call site
compiles.

The third: `platform/SystemPipe.cpp` and `platform/WinsockInit.cpp`, reached when
the vendored runtime tests were built. This one moved the ROOT rather than missing
a file from a fixed one — adding test translation units enlarged what the closure
is a closure OF, and a set that was complete stopped being complete without
changing. It also cascaded one level, which neither earlier miss did.

So the sentence above — *the measured transitive closure of those includes* —
describes the thirteen headers and **cannot** describe the six `.cpp` files, which
were hand-picked. That `plus` is the unguarded part of this criterion, and
**a closure is only ever complete with respect to the root it was walked from**;
an upstream project's target boundaries are invisible from inside its source tree,
and the root moves whenever the build takes on new translation units.
`fastcache-tui-linkprobe` found the first two and the test link found the third:
each named its gap in one link, where reading finds none of them.

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
Nothing under `src/` may reach `<coro/...>` or `<platform/Clock.hpp>`, whether by
including one or by including a vendored header that does. `ctest -R vendor-vocabulary`
enforces it.

The vendored copies exist for exactly one reason: to keep the vendored sources
compiling **unmodified**. They are the price of the verbatim constraint, not a
second opinion about how to write a coroutine.

Two facts that make this much less alarming than it sounds, both measured:

- **`coro/*` and `platform/Clock.hpp` are reached only from `tui/runtime/`, which is
  now built** (#1374). They are compiled as part of `fastcache-tui`, so the second
  coroutine type and the second clock are real objects in this build rather than
  files sitting on disk. That was decided deliberately and the reasoning is in
  `vendor/CMakeLists.txt`: `runtime/` is the only descriptor-parking coroutine API
  available, `IReactor` has none (#1372), and the alternative failed in a worse
  direction — a coroutine resumed on the wrong thread under IOCP is green everywhere
  and wrong in production.

  **This previously said `runtime/` was not built and was enforced by a configure-time
  refusal. Both halves are now gone**, and the rule that survives is the one that was
  actually load-bearing: **nothing under `src/` may reach `<coro/...>` or
  `<platform/Clock.hpp>`.** No arrangement of the vendored target can say that: the
  include root `endo/` is exposed `PUBLIC` and necessarily carries `coro/` and
  `platform/` beside `tui/`, so first-party code can reach the second vocabulary by
  construction, and narrowing the root would hide `<tui/...>` from the consumers that
  need it. So it is a scan, `scripts/check-vendor-vocabulary.cmake` (#1377). It
  follows every include resolving into `vendor/endo` through the vendored headers,
  because `<tui/runtime/TuiRuntime.hpp>` puts `coro::Task` in scope with no forbidden
  spelling anywhere under `src/`. The one legitimate crossing, the adapter layer, is
  an exemption row with a reason, and a row that stops describing a crossing is
  refused.

  The built library's external surface is libunicode, `platform/{Types,Wakeup,Clock,
  SignalHandler}.hpp`, `coro/*` and `crispy/FNV.hpp`. `platform/SystemPipe.hpp` and
  `platform/WinsockInit.hpp` are reached by the vendored TESTS only, so their
  implementations are sources of `fastcache-tui-tests` and not of the library —
  which is also what keeps `ws2_32` off it.
- **The two vocabularies meet in one place.** Vendored code serves the vendored TUI
  only; no vendored file reaches `FastCache::*`, and first-party code reaches the TUI
  through a single adapter layer. If you find yourself wanting to cross that boundary
  somewhere else, that is the signal to widen the adapter, not to add a second
  crossing.

## Local changes

Three local changes, 23 files between them; the other 153 files are byte-identical to their upstream
blobs. A later change can touch a file an earlier one did, so a file can be named by more than one row.
Both halves are checked rather than asserted: `vendor/MANIFEST` records each changed file's
UPSTREAM hash beside its current one, and `ctest -R vendor-verbatim` refuses a file that differs
from upstream unless a row below names it, and refuses a row naming a file that does not.

Every local change goes in its own commit, never folded into the import, and gets a row here
saying what, why, and which upstream it belongs to. **The table is read by that check**: a file
counts as declared only when a row names it in backticks, so prose here explains and declares
nothing.

**One branch per upstream carries every local change**, so the fixes can go back as one pull request
per upstream: endo's on `fastcached/upstream` in the endo checkout, contour's on a `fastcached/upstream`
in a contour checkout. Each row names its commit on that branch, one commit per change. No row today
changes a contour-origin file (`coro/*`, `crispy/FNV.hpp`).

| files | what | why | upstream | prepared as |
|---|---|---|---|---|
| `vendor/endo/tui/CMakeLists.txt`<br>`vendor/endo/tui/InputEvent.hpp`<br>`vendor/endo/tui/MockTerminalOutput.cpp`<br>`vendor/endo/tui/MockTerminalOutput.hpp`<br>`vendor/endo/tui/Screen.cpp`<br>`vendor/endo/tui/Terminal.hpp`<br>`vendor/endo/tui/TerminalInput.hpp`<br>`vendor/endo/tui/TerminalOutput.hpp`<br>`vendor/endo/tui/TerminalQuery_test.cpp`<br>`vendor/endo/tui/VtParser.cpp`<br>`vendor/endo/tui/platform/Terminal.cpp`<br>`vendor/endo/tui/platform/TerminalInput.cpp`<br>`vendor/endo/tui/platform/TerminalInputWin32.cpp`<br>`vendor/endo/tui/platform/TerminalOutput.cpp`<br>`vendor/endo/tui/platform/TerminalOutputWin32.cpp`<br>`vendor/endo/tui/platform/TerminalShared.cpp`<br>`vendor/endo/tui/platform/TerminalWin32.cpp`<br>`vendor/endo/tui/runtime/TerminalEventSource.hpp`<br>`vendor/endo/tui/runtime/platform/TerminalEventSourcePosix.cpp`<br>`vendor/endo/tui/runtime/platform/TerminalEventSourceWin32.cpp` | Terminal's capability queries share one reply loop on an injected `IClock`; input read while waiting that is not the reply is handed back through `TerminalInput::unread()` and delivered first by `poll()` and `TerminalEventSource::wait()`; the seven inline `ColorSchemeReport` workarounds are gone; `queryDecMode` returns `DecModeStatus`, cursor position and cell size return `std::expected<..., QueryUnanswered>`; new `queryDeviceAttributes()` (DA1) with `DeviceAttributesReport` and `advertisesSixel()`; `TerminalQueryInput` test seam and `TerminalQuery_test.cpp`. Endo's `shell/ui/Prompt.cpp` caller changes too, and is not vendored | #1375: a probe ate keystrokes typed before its reply and during a timeout, the timeout could only run in real time, and "never asked", "no reply", "declined" and "not implemented" read as one value. DA1 is the Sixel half of #134's rung detection | endo (contour-terminal/endo) | `D:/endo` branch `fastcached/upstream`, commit `9ae3c66b35a37aa7df8648900edfc8535ed746e6`, on fork point `687b90a0`; unpublished, pending owner |
| `vendor/endo/tui/CMakeLists.txt`<br>`vendor/endo/tui/Terminal.hpp`<br>`vendor/endo/tui/TerminalInputWin32_test.cpp`<br>`vendor/endo/tui/TerminalQuery_test.cpp`<br>`vendor/endo/tui/platform/Terminal.cpp`<br>`vendor/endo/tui/platform/TerminalInputWin32.cpp`<br>`vendor/endo/tui/platform/TerminalShared.cpp`<br>`vendor/endo/tui/platform/TerminalWin32.cpp` | `queryDecMode` moves into `TerminalShared.cpp`, so the Windows arm sends DECRQM and reads the reply through the shared loop; the Windows stub and `DecModeStatus::NotImplemented` go. Raw mode on Windows also asks for `ENABLE_WINDOW_INPUT`. New `TerminalInputWin32_test.cpp` runs against the real console, serialised by a named mutex, and SKIPs by name where there is none: the console mode carries both flags, a size record arrives as a resize with the window's geometry, a buffer resize the console makes is reported, the console's own DECRQM answer for DECTCEM reads Set then Reset, and an unanswered DECRQM is NoReply at the deadline | #134: the Windows arm answered every DEC mode query NotImplemented, so synchronized output could never be detected on Windows. Measured on Windows 11 (26200): conhost answered DECRQM 2026 not-recognized and Windows Terminal answered it reset, both through the shared loop. The flag is the documented condition for size records and is not what made resizing work on the hosts measured: both delivered a window resize with it cleared | endo (contour-terminal/endo) | `D:/endo` branch `fastcached/upstream`, commit `ee25be66e1ece8f2637c3a0aade3ad85153e0eff`, on `9ae3c66b`; unpublished, pending owner |
| `vendor/endo/tui/CMakeLists.txt`<br>`vendor/endo/tui/Sixel.cpp`<br>`vendor/endo/tui/Sixel_test.cpp` | `medianCut` sorts a bucket by a total order on the whole pixel, the widest channel first, instead of by the widest channel alone. New `Sixel_test.cpp`: a fixture with the tie shape, the same pixels reversed quantizing to the same palette, and the encoding's FNV-1a digest equal to a recorded constant | #134: `std::ranges::sort` left pixels equal in the sort channel in the library's tie order and the median split followed it, so the Sixel rung's bytes differed per standard library. Measured with the test: the previous sort gave three digests on libstdc++, libc++ and MSVC, a stable sort one digest on all three but still failed the reversal case, this change `0xbace46a6344de1d9` on all three | endo (contour-terminal/endo) | `D:/endo` branch `fastcached/upstream`, commit `c85ba078c18d0f5ea24376f317961485af327f40`, on `ee25be66`; unpublished, pending owner |

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

**Read upstream BLOBS, never a working tree.** This is the one sentence that will
save the next person re-syncing, and it is not obvious until it has cost you an hour.

Both source clones run `core.autocrlf=true` and **neither upstream ships a
`.gitattributes`**, so their **working trees hold CRLF while their blobs hold LF** —
46,046 CR bytes across `src/tui` alone, belonging to the clone rather than to
upstream. **`git archive` applies that conversion too**, which is the part that
surprises: it reads the repository, so it looks like it should be immune, and it is
not. The first import here did exactly that and arrived with all 46,046 of them,
failing its own byte-identity check.

It has teeth in *this* repository specifically. `.gitattributes` enforces
`* text=auto eol=lf`, so a CRLF copy is silently normalised on commit — the working
tree you verified and the blob you committed are then different bytes, and the
identity check that passed before the commit says nothing about what landed. And a
CRLF `.sh` here does not misbehave, it **fails to start at all**.

So the reader is pinned in the command, not left to remember it:

    -c core.autocrlf=false -c core.eol=lf

`scripts/`-adjacent tooling aside, the durable part is the **mechanism**: the import
script that produced this tree reads blobs at a pinned SHA, compares every file
against its blob, asserts the count, and refuses on any CR byte. Re-run that rather
than re-deriving it.

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
file's `_fcTuiNotBuilt`, with one of two reasons: it reaches `stb_image` (image
decoding), or nothing on a stats panel's path reaches it — an interactive editor's
widgets, present and working and simply not called here. There was a third,
`coro` (the event runtime), retired in #1374 when `runtime/` joined the build;
`QuestionComponent.cpp` moved to the second reason rather than out of the list,
because it is still not reached by anything built here.

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
