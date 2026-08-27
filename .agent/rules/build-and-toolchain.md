# Build system, toolchain and language pitfalls

Rules about the things that differ between compilers, standard libraries, hosts
and tool versions — and about the local gate that exists to catch them before CI
does.

Read this before changing a header everything includes, a randomness or timing
seam, `cmake/portable/CompileCache.cmake`, or anything a test harness's
determinism rests on.

## The local gate

`scripts/local-gate.sh` is the gate. Run it before pushing.

- **A local gate cannot see a configuration it does not build, and advice nobody
  runs is not a gate.** `scripts/local-gate.sh` is that advice as a script:
  clang-format at the pinned version, then `clang-debug` and `gcc-release`, refusing
  to run `ctest` against a build that did not complete. It exists because this
  paragraph was already here and was skipped twice in one branch -- once for a GCC
  `-Wnull-dereference` through an inlined `memcpy` that clang emits at no level, and
  once for a clang-tidy check the binary on `PATH` had never heard of. Both cost a
  full CI cycle for a configuration the developer already had.

  The default agent
  preset is `clang-debug`: one compiler, one standard library, `-O0`, sanitizers on.
  CI is four more — GCC at `-O3`, clang-cl, MSVC, and clang against **libc++** on
  macOS — and each of the three defects that reached CI on the Raft branch was
  invisible to every configuration below it. GCC 14 at `-O3` reports
  `-Wnull-dereference` inside `std::optional::value_or` where clang does not;
  clang-tidy 22 knows checks clang-tidy 20 has never heard of; and libc++'s
  `uniform_int_distribution` is a different function from libstdc++'s. Before
  pushing a change that touches a header everything includes, a randomness or
  timing seam, or anything a test harness's determinism rests on, build **at least
  one release configuration and one non-clang compiler** locally —
  `cmake --preset gcc-release` and `clang-release` both exist and both run in WSL.
  - **libc++ does not have every C++23 library feature libstdc++ has, and the
    failure is a compile error on one platform only.** `std::views::enumerate`
    compiles under GCC 13's libstdc++ and does not exist in libc++ at all, so
    `FleetView.cpp` built clean locally and broke the macOS package job -- a
    configuration whose *first* compile of the change is in CI. This is a different
    shape from the `uniform_int_distribution` case above: not two implementations
    disagreeing, but one of them not shipping the header's contents. Before reaching
    for a C++23 *library* facility that this codebase does not already use
    somewhere macOS compiles, check that it does -- `grep` for it in non-test code
    is enough, since the package job builds the library and every app. The
    workaround is nearly always a C++20 spelling: `std::views::iota` over the
    index range says what `enumerate` says and is ten years older.
- **`clang-format` and `clang-tidy` after every change — at the version CI pins.** Both jobs
  run the `$CLANG_TOOLS_VERSION` binary (`.github/workflows/build.yml`), and successive LLVM
  releases do not agree with each other: the style job compares against a *newer formatter*,
  and the clang-tidy job enables *checks that did not exist* in an older one. So a tree that is
  clean under whichever binary happens to be on `PATH` can still be rejected — a red build for
  code nobody mis-wrote, and one no local run catches unless it uses the same version. Name the
  version explicitly rather than relying on `PATH`:
  `git ls-files '*.h' '*.hpp' '*.cpp' | xargs clang-format-$V --dry-run --Werror --style=file`,
  and `-DCMAKE_CXX_CLANG_TIDY=clang-tidy-$V` **in a build directory of its own**. Found three
  times in one branch: four files reformatted by 22 after 20 had passed them, four
  `find(...) != npos` tests that only 22 reports as `readability-container-contains`, and five
  `std::lock_guard`s that only 22 reports as `modernize-use-scoped-lock`. **The preset alone is
  not that sweep**, and that is the trap: `clang-debug` sets `CMAKE_CXX_CLANG_TIDY=clang-tidy`,
  which on a machine carrying both resolves to whichever `PATH` finds first — 20 in this
  project's WSL image, where 22 sits right beside it as `clang-tidy-22`. So a `clang-debug`
  build reports "clang-tidy clean" in exactly the way that means nothing, and the version it
  used is printed nowhere. Configure a second build directory naming the version, and run that.
- **When `clang-debug` cannot be built, get the sanitizer from GCC instead.** That
  preset is the only one that runs ASan, and on a host where it cannot configure at
  all the tempting conclusion is that no sanitizer coverage is available locally. It
  is: GCC has ASan too, and a throwaway tree costs one configure.

  ```sh
  cmake -S . -B out/build/gcc-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
        -DFASTCACHED_ENABLE_TLS=ON -DPEDANTIC_COMPILER_WERROR=OFF
  ```

  **ASan only, never `address,undefined`.** UBSan's instrumentation defeats GCC's
  constant evaluation of the option tables' lambda addresses, so `Options.hpp`'s
  `TableIsWellFormed` `static_assert` stops compiling -- a build error with nothing
  to do with the code under test, and an easy reason to abandon the idea.

  A use-after-free reached CI through two jobs while this was thought unavailable.
  The local run that catches it takes about a minute.

- **A memory bug reports only if something disturbs the freed block, so run the
  WHOLE suite.** The case that failed in CI passes under ASan on its own: nothing
  reuses the allocation, so nothing is read across a boundary the interceptors
  watch, and the test reports success. The same held for a standalone repro --
  decode, read, nothing in between: clean; sixty allocations inserted between the
  two: `heap-use-after-free` immediately. A quiet sanitizer run on one test is not
  evidence that a diagnosis is wrong.

- **After reverting files to reproduce a bug, check that the build SUCCEEDED.**
  Reverting two files to the parent commit left an unrelated translation unit
  failing to compile; ninja stopped, and the binary that ran was the *previous* one.
  It passed, which looked like proof the bug was not real. `ninja: build stopped`
  scrolls past above a green test run, and a reproduction that did not rebuild is
  not a reproduction.

- **A sanitizer that is on in the cache is not a sanitizer that is on in the build.**
  `cmake/portable/Sanitizers.cmake` initialised `SANITIZER_COMPILE_OPTIONS` to `""`
  as a normal variable, published the real flags through
  `set(... CACHE INTERNAL ...)`, and then `list(APPEND)`ed one more flag to the same
  name. Under **CMP0126 NEW** -- which `cmake_minimum_required(3.28)` selects -- a
  cache `set` no longer removes a normal variable of that name, so the empty one
  keeps shadowing the cache. The append therefore starts from nothing and
  `add_compile_options` receives `-fno-sanitize-recover=undefined` and no
  `-fsanitize=` at all, while `CMakeCache.txt` says
  `ENABLE_SANITIZER_ADDRESS:BOOL=ON` and the configure log prints
  `[Sanitizers] Enabling: address,undefined`. Every signal an author would check says
  yes; not one object is instrumented -- and not on a re-configure, on a **completely
  fresh build directory**, which is what CI makes. So the project's sanitizer gate had
  never run: not locally, and not in the `clang-tidy` job that ends with
  `ctest --preset clang-debug`. Turning it on found a heap-use-after-free in
  `EpollSocket::Close`, a leaked coroutine frame per dial that `IReactor::CancelPending`
  now prevents, and a plain use-after-free in a test fake that had been passing 300
  consecutive runs.
  The flags are assembled in locals and published once now. Two things to keep: the
  check that actually answers the question is `grep -o -- '-fsanitize=[a-z,]*'` **on
  `build.ninja`**, since the cache and the log are precisely the two places that lie;
  and this is the same class as the `USE_COMPILER_CACHE` configure probe -- a tool
  that silently does nothing is worse than one that is off, because the second is
  visible.
## Language and ABI pitfalls

- **A return type is not part of a function's name on Linux, and MSVC's mangling
  hides that.** `Core/HostPort.hpp` added an `inline FastCache::ParsePort(
  std::string_view)` returning `std::optional<std::uint16_t>` while
  `Config/CliParser` already had a `FastCache::ParsePort(std::string_view)`
  returning `std::expected<std::uint16_t, ConfigError>`. That is **not an
  overload**, and no compiler can say so: each translation unit sees exactly one
  of the two declarations, so both compile, and the Itanium ABI does not encode a
  return type in a free function's mangled name -- so both definitions claim the
  identical symbol, the linker keeps `CliParser`'s strong one over the header's
  weak inline, and every caller of the header version silently reaches the other.
  It reads an `expected` as an `optional`: a SIGSEGV on the first call, from code
  that is correct in isolation. Renamed to `ParseTcpPort`, with the reason at the
  declaration. Three things worth keeping:
  - **Windows cannot find this and will report the tree as green.** MSVC's
    mangling *does* include the return type, so the two are distinct symbols
    there and both link. This branch had 1730 passing MSVC tests at the moment
    Linux was segfaulting, which is the whole argument for running the Linux
    gate locally rather than discovering it in CI a phase later.
  - **A standalone reproducer will not reproduce it**, because the bug is in the
    *link*, not the code: the same calls compiled against the header alone are
    correct and pass under ASan. What identified it was `nm -C` on the library
    object, showing a strong `T FastCache::ParsePort(...)` that the test binary
    had no business resolving to.
  - **The two implementations were not merged**, deliberately. The CLI's version
    distinguishes "not a number" from "out of range" because an operator needs
    to be told which; an `optional` cannot carry that. Collapsing them to share
    one body would trade a real diagnostic for a de-duplication nobody asked for
    -- the same reasoning that keeps the dispatch counters split.

- **`main` is not exempt from cognitive complexity, and the fix is extraction rather
  than a raised threshold.** The node's `main` reached 70 against clang-tidy's limit
  of 60 as the admin endpoint was wired in. Both blocks that came out --
  `AdminEndpoint::Start` and `AdoptActivatedListener` -- are coherent decisions with
  one answer each, which is why the number was a symptom worth listening to rather
  than a rule to argue with. The six bare `return 2`s it left behind became
  `ExitUsage` for the same reason: seven copies of a magic exit code is the
  table-shaped defect this list keeps recording.

- **Never run `clang-format -i` with a version other than the pinned one.** As a
  *checker* an older binary is worth something; as a *formatter* it rewrites code
  the pinned version already blessed, and the diff is invisible in review because
  every line of it is "just formatting". Running `clang-format-18 -i` on
  `FleetView.cpp` to tidy three added lines reflowed all three column tables --
  code the change never touched -- and `Check C++ style` rejected 80 lines at
  clang-format 22, none of them new.

  The repair is not to reformat again but to **restore the untouched region
  byte-for-byte** from the last commit that passed the style job, then re-insert
  only the new lines, and prove it: a diff of that region against the good commit
  must show insertions and *zero* deletions.

  So: run a non-pinned binary as `--dry-run` on the lines **you** added, never with
  `-i`, and never let it touch a file you are only passing through.

  **"The apt mirror has no `clang-format-22`" is not a reason to format with 18.**
  LLVM ships the official binaries on PyPI, so the pinned version is one download
  away on any host with outbound HTTPS and no root:

  ```sh
  pip download "clang-format==${CLANG_TOOLS_VERSION}.1.0" -d /tmp/cf --no-deps
  python3 -m zipfile -e /tmp/cf/clang_format-*.whl /tmp/cf22
  install -m755 /tmp/cf22/clang_format/data/bin/clang-format ~/.local/bin/clang-format-22
  ```

  With that on `PATH`, `scripts/local-gate.sh` finds it by name and the whole tree
  can be formatted exactly as `Check C++ style` will judge it -- which is strictly
  better than hand-matching a style guide and then finding out in CI. Reach for the
  byte-for-byte restore above only when even this is unavailable.

  **`clang-tidy` ships the same way, and it matters more.** Successive releases add
  *checks*, so an older binary is not merely a laxer formatter -- it is silent about
  entire categories. `modernize-use-scoped-lock` and
  `readability-math-missing-parentheses` do not exist in 18, and 22's
  `bugprone-unchecked-optional-access` follows a value through a binding that 18's
  does not, so a file clean under 18 arrived at CI with thirteen findings.

  ```sh
  pip download "clang-tidy==${CLANG_TOOLS_VERSION}.1.0" -d /tmp/ct --no-deps
  python3 -m zipfile -e /tmp/ct/clang_tidy-*.whl /tmp/ct22
  ```

  **Run it in place, or through a wrapper that does.** `clang-tidy` finds its own
  resource headers *relative to the binary*, so copying just the executable out of
  the wheel produces `'stddef.h' file not found` -- and every check then reports
  against a translation unit that did not parse, which looks like a wall of real
  findings and is nothing of the kind.

  It also needs a **clang** compile database, generated with **module scanning
  off**. Pointed at a GCC one it inherits flags clang does not know; and a database
  from a module-scanning generator carries `@…modmap` arguments that do not exist
  until that target has been built, so the translation unit fails to parse and the
  file reports nothing at all. Configure a throwaway tree:

  ```sh
  cmake -S . -B out/build/tidy22 -G Ninja \
        -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DFASTCACHED_ENABLE_TLS=ON
  ```

- **A sweep that cannot prove the tool ran is worth nothing, and reads like
  success.** Every way of getting clang-tidy wrong above -- an unset execute bit
  (the wheel does not carry one), a wrapper that cannot exec, a missing resource
  dir, an unparseable `@modmap` -- produces *silence*, and silence filtered through
  `grep 'error:'` is indistinguishable from a clean file. This branch shipped to CI
  twice on sweeps that had reported clean while executing nothing.

  `scripts/tidy-sweep.sh` is that lesson in executable form: it canaries the binary
  against a real source file first, treats exit codes ≥ 126 as fatal rather than as
  findings, and refuses to print a clean verdict it did not earn. Use it, and if you
  write a one-off loop instead, make it fail loudly the same way.

- **A `bool` in the middle of a config struct costs seven bytes, and four of them
  fail the build.** `clang-analyzer-optin.performance.Padding` permits 24 bytes more
  padding than an optimal field order would give, and `NodeConfig` is almost entirely
  `std::string` and `std::filesystem::path` -- so a one-byte member between two of
  those is padded out to a full eight rather than costing one. Adding `--dashboard`
  and `--tls-self-signed` beside the settings they configure took the struct to 38
  bytes against an optimal 6, and the `clang-tidy` job -- the only one that runs the
  analyzer -- rejected it.

  Keep every `bool` and byte-wide enum in **one run**, which that struct already half
  did with its trailing flags. Two of the four moved were pre-existing (`logLevel`,
  `serviceScope`), and that was the point: moving only the two new ones lands on
  *exactly* 24, which passes and leaves the next contributor's `bool` to break it
  again. Moving all four lands on 8. A field's position there is a layout constraint,
  not where it reads most naturally, so the run says so in a comment.

  The check is `BaselinePad - OptimalPad > 24` and both terms are arithmetic over
  `sizeof`, so it reproduces **without the analyzer**: sum the field sizes, subtract
  from `sizeof(T)`, and compare against the same sum laid out by descending
  alignment. Worth knowing because it gives an exact number where re-running CI gives
  a yes or no -- and because a host without the pinned clang-tidy can still check it.

- **A `static_assert` that anchors a table's length on an enumerator BY NAME is a
  guard that fires only when nothing is wrong.** Casting an enumerator to an index
  is safe exactly while the table holds one row per enumerator in enumerator order,
  and this tree used to spell that rule five ways across eight tables -- of which
  **six failed open in the one situation the rule exists for**. Four were anchored
  by name:

  ```cpp
  static_assert(Table.size() == static_cast<std::size_t>(Enum::TheLastOneToday) + 1);
  ```

  Append an enumerator and *forget* the row: `size()` still equals
  `TheLastOneToday + 1`, so it compiles, and the lookup then reads past the end --
  reproduced verbatim under ASan as a `global-buffer-overflow` four bytes past
  `RefusalTable`. Append one and *remember* the row: the size is now one greater
  and the assert fails. It is inverted, and nothing about reading it says so. Two
  more tables had no length tie at all, and one of those, `Consensus::RoleTable`,
  was reached by a linear scan that fell back to `RoleTable.front()` -- so a
  missing row returned a **follower's** traits for a leader, silently, which is
  worse than the crash. `Core/EnumTable.hpp` is the one spelling now: `Last`
  states the enum's own count, `EnumTable<Enum, Row>` takes its extent from that
  so a short table cannot be *declared*, and `RowsInEnumeratorOrder(table,
  &Row::member)` checks the extent and every row's position in one assert.
  Consequences that are each load-bearing:
  - **The row carries the enumerator it describes, and that is what makes the
    order checkable at all.** A bare array of values can only have its length
    asserted -- which is why the three bare ones (`RoleNames`,
    `ProposalRefusals`, `PickErrorTable`) recorded their order in trailing
    comments no compiler reads. Giving them struct rows is not decoration; it is
    the difference between a checked invariant and a documented hope.
  - **`Last` is a cost to measure, not to assume.** It was free for these six
    because nothing in the tree switches over any of them -- grepped, not
    guessed. Adding a sentinel to an enum whose callers deliberately omit
    `default:` turns every one of those `switch`es into a build error, and this
    codebase omits `default:` on purpose in several places (`PathCanon::Anchor`,
    `DirectManifest::PathRole`) precisely so a new state is a compile error. That
    trade has to be made rather than inherited.
  - **A proved order turns the lookup into an index rather than a search.**
    `TraitsOf` scanned `RoleTable` and carried an eight-line comment re-deriving
    the `std::array`-iterator portability argument that `Core/Ranges.hpp` already
    documents; with the position guaranteed there is nothing to search for, and
    the comment went with the scan.
  - **The guard was verified by reintroducing the defect at all six sites.** One
    throwaway enumerator appended with no row, and each of the six now fails to
    compile naming its own table. A completeness guard that has never been seen
    to fail is exactly the thing this entry is about.

## Line endings

Line endings are LF everywhere, and that is a `.gitattributes` rule
(`* text=auto eol=lf`) rather than an instruction to set `core.autocrlf`. The
config is per-clone and per-developer, so without the rule two people editing one
file disagree about what a line ending is — and the disagreement is invisible
until a diff comes back as *every line changed* for a two-line edit, which is how
it was found. Stored content was already LF, so the rule changed nothing that is
committed, only what lands on disk at checkout. `*.sh` keeps a row of its own even
though the general rule covers it, because the consequence there is specific: a
CRLF shebang makes the kernel look for an interpreter whose name ends in a
carriage return, so such a script does not misbehave — it fails to start at all.
## The portable compile-cache module

- **`cmake/portable/CompileCache.cmake` must stay stock-CMake-only, and must never fail a
  configure.** Same constraint as `Cli/UsageDoc` and `Protocol/CompileCacheWire`,
  for the same reason: the file is *meant* to be copied verbatim into other
  projects, so a dependency on anything else here breaks it where nobody in this
  repository would notice. It is also included at `CMakeLists.txt:164`, before CPM
  is bootstrapped at `:183`, so `CPMAddPackage`/`FetchContent` are not available to
  it even locally — the `FASTCACHE_AUTO_INSTALL` fetch therefore uses bare
  `file(DOWNLOAD)`. Not with `EXPECTED_HASH`, which aborts the configure on a
  mismatch *even when `STATUS` is captured* (measured); the SHA-256 the release
  publishes is compared by hand instead, which is the same guarantee without the
  abort. Every other way the fetch can fail — unpublished platform, no network, a
  binary that will not run here — ends the same way, in one `message(STATUS)` and a
  fall-through, because a project that vendored this file to get a *faster* build
  must not lose the ability to build at all when GitHub is unreachable.
  `ctest -R compile-cache` covers both halves offline, the decline paths through a
  sandbox and the install path through a `file://` mirror.
## `USE_COMPILER_CACHE` in full

`USE_COMPILER_CACHE` (default ON, `cmake/portable/CompileCache.cmake`) fronts the compiler
with our own `fastcache-cc` when it is on `PATH` and a daemon answers — at
`127.0.0.1:6674` by default, or wherever `FASTCACHE_ADDR=host:port` points;
`FASTCACHE_SOURCE_DIR`/`FASTCACHE_BINARY_DIR` are injected from the source and build
trees. Configure proves the cache works by compiling one tiny file through the
launcher (~0.1 s) and requiring a `HIT`/`MISS`, because a launcher that cannot
reach its daemon still compiles fine and would otherwise cost every TU a failed
connect in silence. When nothing answers it falls back to `sccache`. When
*nothing* is installed, `-DFASTCACHE_AUTO_INSTALL=ON` (default OFF) fetches a
prebuilt `fastcache-cc` for the host from the latest stable release instead,
staged per user so a machine downloads it once; `cmake/README.md` is the note
for projects vendoring the module. A cache hit reproduces only the object file,
so with either launcher active the module scan and precompiled headers are
turned off and MSVC debug info is forced to `/Z7`
(a modmap flag makes the launcher's preprocess step fail, and a PCH or shared
PDB is a second artefact no hit can reproduce).

## Code coverage

`cmake --preset clang-coverage`, build, then `--target coverage`. That target runs the
**whole** CTest suite under instrumentation and writes
`out/build/clang-coverage/coverage/`: `html/index.html` to browse, `coverage.lcov` for
Codecov, `report.txt` and `percent.txt`. CI runs that same target, so there is one code
path (`scripts/coverage.sh`) rather than two that drift.

**Clang source-based, never gcov — because of how many processes this suite is.**
`catch_discover_tests` gives each of ~2000 `TEST_CASE`s its own process, and the
script-driven tests spawn daemons and launchers besides. gcov merges counters into a
shared `.gcda` per object file as each process exits, so concurrent writers race; that
race is what every `lcov --ignore-errors mismatch,inconsistent` on the internet is
suppressing, and a suppressed error there is under-counted coverage reported as a clean
run. LLVM keys each raw profile on the binary's own module signature and merges into it
under a lock, so no suppression and no serialization is needed.

**`%8m`, never `%p`.** Measured on this tree, 2228 test processes produce **55** raw
profiles totalling 20 MB. `%p` would have written one multi-megabyte file *per process*.

**A compiler cache and coverage cannot be combined.** Coverage mapping data is embedded
in the object file and names its sources by **absolute path**, while `fastcache-cc`
exists precisely so an object built under one checkout root can serve a compile under
another. A hit replays a perfectly valid object carrying the *producer's* paths, and
llvm-cov then reports files that do not exist here — a report about somebody else's
tree, with nothing failing to say so. The preset sets `USE_COMPILER_CACHE=OFF`, and
`cmake/Coverage.cmake` refuses to configure if a launcher is set anyway, since
`-DCMAKE_CXX_COMPILER_LAUNCHER=` bypasses that option.

**`*_test.cpp` is excluded from the report, and that is load-bearing.** Tests here live
*next to* the implementation, so ~150 `*_test.cpp` files compile straight into the test
binaries. Counting them measures the tests testing themselves — thousands of near-100%
lines that move the total a long way and mean nothing. The one `ignore_regex` in
`scripts/coverage.sh` is the single source of truth for what is measured; `.github/codecov.yml`
restates it only so Codecov's view cannot silently disagree.

**Every misconfiguration is a `FATAL_ERROR`, not a warning that disables itself.** A
non-Clang compiler, Windows, an enabled sanitizer, no test suite, a missing `python3`, a
compiler-cache launcher, or an `llvm-profdata` whose major version differs from the
compiler's — the raw profile format is versioned, and a mismatch otherwise surfaces as a
complaint about the *file* rather than about the tool. A coverage build that quietly
instruments nothing still compiles, still runs the suite and still writes a report, so
every signal an author would check says it worked. Same reasoning as the sanitizer entry
above, which had already been found in exactly that state.

**The CI job publishes and gates on nothing.** `coverage` in `build.yml` reports the
figure to the job summary, uploads the HTML, and pushes lcov to Codecov once
`CODECOV_TOKEN` exists; both Codecov statuses are `informational`. The long-term target
is >90% and raising it is separate work — a threshold added before anyone has had the
chance to move the number only teaches everyone to ignore the signal. A failing suite
still gets its report rendered, to read while debugging, and then re-raises so no number
measured from a red build is published.
