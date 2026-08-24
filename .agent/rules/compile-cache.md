# The compile cache key

Rules for `src/apps/fastcache-cc/` and `src/FastCache/CompileCache/`: what goes
into a cache key, how paths are canonicalized so two machines derive the same one,
what a direct-mode manifest may record, and what a hit is allowed to replay.

Read this before touching `CacheKey.cpp`, `DirectManifest.cpp`, `PathCanon`,
`RootReconciler.cpp`, `CompileValue`, `CmdLine`, or anything that decides whether
a compile is cacheable.

Two failure shapes recur here and are worth holding in mind while reading: a rule
broken one way collapses the hit rate silently (every unit test still passes), and
broken the other way serves an unrelated object under a **zero exit code**. Every
rule below has already been at least one of the two.

## The five rules the end-to-end tests assert

`fastcached` and `fastcache-cc` are both installed. Three things the launcher's
cache key depends on, each of which has already caused a silent hit-rate
collapse or a mis-serve and is now covered by regression tests:

- **Preprocessing must suppress line markers** (`/EP` on MSVC, `-E -P` on GNU).
  A `# 1 "/abs/path.cpp"` marker embeds the checkout path in the hashed text.
  On MSVC that is `/EP` **alone**: `/EP` and `/P` are alternatives, not modifiers
  — `/EP` preprocesses to stdout, `/P` to a `<base>.i` file, and MSVC documents
  the pair as "to the file, without `#line`". Passing both left the launcher
  hashing an empty stdout, so a Windows key carried nothing from the source and
  an edited file was answered with the object built from the previous revision.
  That is a wrong build rather than a cold cache, and direct mode hid it, since
  its manifest hashes the source's own bytes — hence the e2e case that edits a
  source with `FASTCACHE_NO_DIRECT=1` and requires a MISS.
- **`/` introduces an option only on Windows.** On POSIX it starts an absolute
  path, and treating it as an option leaves absolute paths unrelativized.
- **Only machine-independent dependency paths may be hashed.** The dependency set
  is part of the key so a moved header re-keys; hashing a toolchain path along
  with it would re-key on the *machine* instead, and two boxes with the same
  compiler at different prefixes would stop sharing every entry they have.
- **A root and the paths a driver emits must be reconciled on both sides or
  neither.** Every root test is a string prefix comparison, so a root spelled
  differently from what the compiler echoes back matches nothing it emits — and
  that empties the keyed dependency set, silences the replay guard, and leaves the
  producing machine's paths in the stored value, all at once and all silently.
  `Cc::IPathResolver` resolves the roots and every emitted path through the same
  function; resolving only one side breaks the driver that previously worked.

- **A compile that writes a second artefact is not cached at all.** What a hit
  reproduces is the object and the dependency record; a **C++ module interface
  unit** also writes a BMI (`.ifc`, `.pcm`), and `/Yc` a precompiled header. Replay
  one and the second artefact is either missing afterwards — which fails loudly —
  or left over from a previous build, which does not. Both halves are one rule and
  neither may be dropped: the module **extensions** (`.ixx`, `.cppm`, `.ccm`,
  `.cxxm`, `.c++m`, `.mxx`) are classified as their own language and refused by
  name, and an ordinary source **promoted by a flag** (`cl /interface`,
  `-fmodule-output`, `--precompile`) is refused off a table. Until this was written
  down the first half held by accident — those extensions simply were not in
  `IsSourceSuffix`, so such a line fell through as "no source file found" and was
  passed through *in silence*, which reads exactly like a broken cache; and the
  obvious "add .ixx so modules are supported" change would have turned that
  accident into a silent wrong build. The refusal now says so under
  `FASTCACHE_VERBOSE`.

The first two break cross-checkout sharing while every unit test still passes,
the third breaks it the moment two machines differ, the fourth breaks it the
moment a root is spelled unusually, and the fifth is a wrong *build* rather than a
cold cache, so `scripts/compile-cache-e2e.sh` (POSIX) and
`run-launcher-e2e.ps1` (Windows) assert them end-to-end in CI on both
platforms.

## What the key is a function of

- **A key determines two artefacts, so it must be a function of both.** Preprocessing
  suppresses line markers (`-E -P`, `/EP`) so a checkout path never reaches the key —
  which is what makes a key portable, and equally what made it *invariant under a header
  move*. Move a header without changing a byte of it and the token stream is identical:
  the object is still correct and was served, while the depfile, which is nothing but
  paths, named a file that is gone. That is worse than a miss, because Ninja records the
  dependency, cannot stat it, rebuilds, hits the same value, and never converges — with a
  successful exit code every time. The dependency path set is therefore part of the key
  (`objkey-v4`, `KeyInputs::dependencyPaths`), captured on the preprocess run the launcher
  already makes rather than in a second probe: measured at **+1.5% on a 45 ms preprocess**,
  because the compiler has already opened every one of those files. A move is a different
  key by construction, so the *pre-move* entry survives the move rather than being
  overwritten — which is the property `check_header_move` asserts by moving the header
  back and requiring a HIT. Anchored as `fastcache-cc: HIT`: the launcher prints
  `STALE HIT (...); recompiling` on its way to a MISS, so a bare `grep HIT` is satisfied by
  exactly the collapse the case exists to reject. `ComputeManifestKey`'s `manifest-v4` tag is
  bumped in lock-step with `objkey-v4` for a related reason — a manifest stores the object key
  *by value* and its own key never sees the object-key schema, so a manifest written by an
  older launcher keeps resolving to an older object; direct mode is on by default and
  short-circuits before the preprocessed path,
  so without the second bump the re-key never happens where it matters most.
  - **Which paths are hashed is the whole subtlety, and the exclusion cuts the opposite
    way from the inclusion.** `KeyDependencySet` normalizes each path through
    `DirectManifest`'s `NormalizePath` **first** — a driver echoes a path as *resolved*, so
    `build/../inc/a.hpp` and `./inc/a.hpp` arrive verbatim, and unnormalized they are two
    key entries for one header and two different keys on two machines whose generators
    spell an include directory differently. Then it keeps a path that canonicalizes to a
    `<SRCROOT>`/`<BUILDTREE>` token and **drops** toolchain content, judged by
    `DirectManifest`'s own `IsToolchainHeader` so that this filter, the manifest's and the
    replay guard's cannot disagree: a path under neither root, *and* a vcpkg tree nested
    under the build tree, which canonicalizes but is still the producing machine's. That is
    content already covered collectively by the compiler identity in the key, and hashing it
    would mean two machines with the same compiler at different install prefixes share
    *nothing at all* — 476 of a real TU's 635 headers are toolchain, and a manifest naming
    them would be machine-specific. The set is sorted and deduplicated because
    `/showIncludes` repeats a header once per inclusion site and emission order is a
    property of the driver.
    - **A relative path is classified by what it resolves to, never by its spelling** —
      which is why `KeyDependencySet` takes a working directory at all, threaded from
      `RunCached` through `CompileWorkingDirectory()`, the one place `main.cpp` answers
      that question for both this filter and the replay guard. Both rules above ask what a
      path *names*; "is it absolute" answers a different question, and the two invert for a
      vendored or relocatable toolchain reached through a relative include path (issue
      #64). `-isystem ../toolchain/include` makes the driver report
      `../toolchain/include/foo.h`, which lies under no root — so a spelling test *kept and
      hashed* the very file it drops when the driver spells the same header
      `/home/dev/toolchain/include/foo.h`, and a build tree at a different depth then keyed
      every TU that touches it differently. Resolving first collapses the two branches into
      one: a relative project header becomes the token its absolute spelling would produce
      (so one header reached two ways is one entry), and a relative toolchain header is
      dropped exactly as an absolute one is. What the key gives up — a *vendored* header
      move no longer re-keys — is what `Cc::MissingReplayedDependency` still covers, since
      it probes a relative replayed path for existence before writing anything; the same
      trade and the same backstop the absolute exclusion has always had. Two mechanics are
      load-bearing: the resolution is **lexical**, because `weakly_canonical` would rewrite
      an 8.3 short component to its long form and break the prefix tests two bullets down;
      and it runs on `/`-folded text, because `std::filesystem` treats `\` as a separator
      only on a Windows *host*, so a Windows path normalized on POSIX keeps its `..`
      segments and canonicalizes to `<BUILDTREE>/../../inc/a.hpp` — a token naming a file
      the path does not name. Two further host couplings live in that same lexical pass and
      were each *measured* on libc++ rather than assumed. `lexically_normal` collapses a
      leading `//` on POSIX and keeps it on Windows, where it is a UNC root name, so the
      root is carried across the pass by hand — without that, a `\\server\share` layout
      stops prefix-matching its own root on one host, classifies every project header as
      toolchain, and hands back an empty set. And the absoluteness test is asked **before**
      the collapse rather than after, because `C:/../x` normalizes to a bare `x` on POSIX
      (`C:` is an ordinary filename there for `..` to eat) while Windows cannot ascend past
      a drive root and keeps `C:/x` — so collapsing first sends an absolute path down the
      resolve-against-the-working-directory branch on one host only. Asking first is also
      what makes the relative branch normalize once, after the join, instead of on each
      side of it. The tags moved to `objkey-v4`/`manifest-v4` for this change, in the
      lock-step the manifest bullet above describes — but note which half forced it. The
      *dependency-set* re-key would not have needed a bump on its own: it changes a key only
      for a translation unit that reported a relative path, and those keys differ by
      construction, so a stale entry becomes unreachable rather than servable under rules it
      was not written by. The manifest half is what required it, and `objkey` follows the
      manifest here rather than the other way round. A working directory that is empty or
      itself relative drops
      every relative path rather than guessing, and the `dependency set: N of M` note is
      what keeps that from being silent. The *residual*, recorded deliberately: a relative
      include-dir argument (`-I../../vendor/sdk/include`) still reaches the key verbatim
      through `RelativizeArgs`, which canonicalizes only paths it can resolve — absolute
      ones — so two machines whose build directories sit at different depths still key
      apart on the arguments even though the dependency set now agrees. Closing that means
      resolving arguments against the same working directory, which is a separate change to
      a separate function.
  - **"Absolute or relative" is the wrong question on Windows, because there are three
    answers.** `C:foo` carries a drive specifier and is still not rooted: it resolves against
    *that drive's* current directory, per-process state on the producing machine that no
    cache entry records. `PathCanon::AnchorForLayout` therefore returns an `Anchor` —
    `WorkingDirectory`, `Absolute`, `DriveRelative` — where it used to return a `bool
    IsAbsoluteForLayout` whose drive test stopped at the colon and so reported all three
    Windows shapes as absolute (issue #65). Every caller switches on it with no `default:`, so
    a fourth state is a compile error at each rather than a silent fall-through. What none of
    them may do is treat it as `WorkingDirectory`, and since issue #64 that branch does
    something stronger than keep a spelling — it *resolves* the path against the compile's
    working directory, which for `C:foo` names a file that was never read. Hashing the
    spelling instead, as it used to, would let two machines whose `C:` cwd differs key
    **identically for different headers**, the same silent cross-TU mis-serve #63 closed by a
    different route. Either way the answer is the same: a drive-relative path is neither.
    **Past that the callers part, and
    the asymmetry is the substance of the fix.** The key filter needs a portable *spelling*,
    so it leaves the path to the root tests — root membership is the stronger question, and
    under a drive-relative *root* (`C:src\proj`, a Windows root by its separators) the path
    canonicalizes to a token that is portable precisely because the consumer substitutes its
    own root. Dropping on the anchor alone would have silently un-keyed that whole layout,
    and it is what the first cut of this change did. The replay guard needs something to
    *stat*, which a drive-relative path is not under any working directory it could be handed
    — `std::filesystem::operator/` reaches a drive's current directory by no route: on POSIX
    the join names nothing that exists, on Windows it resolves against the *process* cwd — so
    it skips, under the existing rule that a path which cannot be examined counts as present.
    For a drive-relative root that arm is a behaviour *change*: such a path used to be probed
    against the wrong anchor and discarded every hit carrying it. The **manifest** is the
    third caller and sides with the key filter, on the same reasoning read through its own
    failure mode: it *opens* a path rather than probing one, and an unreadable entry refuses
    the manifest while recording (`HashFileContents` yields nothing) and fails to validate
    while reading — safe in both directions — so the stronger root question is worth asking,
    and dropping on the anchor alone would silently un-cover a project header, which is the
    defect its own bullet below exists to close. The residual, recorded
    deliberately: a drive-relative path under no root is then neither keyed nor guarded, so a
    moved one would replay a stale depfile — reachable only when a build passes a
    drive-relative `-I` *and* the driver echoes it unresolved (`cl` resolves through the
    filesystem; clang-cl echoes what it was handed), and closing it would mean recording the
    producing machine's per-drive cwd in the value, which is exactly the machine-specific
    state the key exists to keep out. One diagnostic consequence: such a path dropping out of
    the key makes the launcher's `dependency set: 0 of M reported path(s) keyed` line
    reachable for a second reason, so that fingerprint no longer identifies the #66 short-name
    mismatch on its own — the two are told apart by whether the *root* is short-name spelled,
    and separating them in the counter itself is left as the follow-up it is.
    - **The ASCII rules are one rule each, and the drive-letter one had drifted into four.**
      Two of the four spellings tested it with `std::isalpha`, which is **locale-dependent** — in a codebase
      whose whole premise is that two machines derive the same key from the same content, a
      classification that varies with the running process's locale is a classification they
      can disagree about. `PathCanon::IsDriveLetter` is now the single definition and every
      site takes the letter test from it. What each site adds on top deliberately differs and
      that is not drift: `AnchorForLayout` and `IsWindowsRoot` also ask what *follows* the
      colon, while the two depfile rule-splitters do not — they are deciding where a rule
      ends, and `C:foo` is one token there however it is anchored. The one place the two
      root-shaped tests genuinely part is a bare `C:`: as a layout **root** it is the
      degenerate spelling of the drive root and `IsWindowsRoot` accepts it, while as a
      **path** the same bytes name the drive's current directory and `AnchorForLayout` calls
      it `DriveRelative`. Both halves are pinned against the *same layout* in one test, so
      the shared helpers cannot quietly merge them in either direction. The same reasoning
      moved `PathCanon::AsciiLower` into the header beside it: `IsToolchainHeader`'s
      comparison form was folding case through `std::tolower`, and under a Turkish locale
      `std::tolower('I')` is not `i` — so a root spelled `D:\PROJECT\Inc` folds one way on
      one machine and another way on the next, and the two derive different manifests and
      different dependency sets from byte-identical content. A locale-sensitive
      classification is the same defect as a host-sensitive one, and this layer exists to
      have neither.
  - **A stream driver's notes must not reach the hashed text, and which stream carries them
    is not the driver table's answer to give.** `DriverSpec::includeStream` describes the
    *compile* run; the probe is a different command line and clang moves the notes off
    whichever stream the preprocessed text is using — measured, `clang-cl /c /showIncludes`
    reports on **stdout** while `clang-cl /EP /showIncludes` reports on **stderr** (LLVM
    D46394). Routing the probe by that table therefore read an empty set on clang-cl and
    made this whole key input a silent no-op there. So `Preprocess` guesses at nothing: it
    splits stdout unconditionally (a byte-exact no-op on a stream with no notes) and unions
    the notes from both — which is the treatment `RecordManifest` already gives the same
    question, "rather than guessing which compiler produced this value". A note left in the
    text would be keyed as if it were source, and it carries an absolute path, which is
    precisely what suppressing line markers exists to prevent.
  - **The note grammar is one rule, not one string, and it is anchored.** `SplitIncludeNotes`
    and `ParseIncludePaths` both call `IncludeNotePath`, which matches after leading blanks
    (`cl` indents by nesting depth) and **nowhere else**. Matching the marker anywhere in the
    line is safe on a pure note stream and a mis-serve on the one that also carries
    preprocessed *source*: it deletes an ordinary line that merely quotes the marker out of
    the hashed bytes, so two revisions differing only in that string literal key identically
    and the second is served the first's object. This repository's own sources contain the
    literal, so it was reachable while building `fastcached` itself.
  - **A manifest that names no dependency is refused, not recorded.** A direct hit
    revalidates exactly what its manifest lists, so a manifest built from the source alone
    replays an object whose headers nobody re-checked — edit a header, leave the `.cpp`
    untouched, and the stale object is served forever with a zero exit code. That is what a
    build passing neither `-MD`/`-MF` nor `/showIncludes` produces: no stream carries notes
    and there is no depfile to read. `RecordManifest` returns instead, which costs that build
    direct mode (a permanent manifest miss, resolved by the ordinary preprocessed key) and is
    the one shape where recording nothing is strictly better than recording something. The
    launcher never injects those flags itself — the compile runs the build system's own argv,
    so what it can revalidate is bounded by what the build asked the compiler to report.
  - **A manifest classifies a path's anchor before asking whether it is toolchain content,
    and the working directory is what makes that answerable.** `IsToolchainHeader` reports
    every path outside both roots as toolchain, and a *relative* path lies under no root, so
    asking it first reported every relative path as toolchain and dropped it. A GNU build
    whose depfile carried relative header paths — a relative `-I`, or a compile run from the
    source directory, which is also how the CMake Ninja generator spells its sources —
    therefore recorded a manifest of its absolute entries alone, and an empty manifest
    validates against anything: edit a dropped header, leave the `.cpp` untouched, and the
    direct hit serves the previous object under a zero exit code, *permanently*, because a
    direct hit never reaches `RecordManifest` to repair the manifest that let it through.
    `Cc::IsCheckable` and `Cc::PortableForm` are the other two consumers of that classifier
    and both already ordered it this way, in as many words; this was the third and did not
    (issue #57 is the same defect reaching the TU source rather than a header). The
    classification is now three-valued — `Project` / `Toolchain` / `Unanchored` — for the same
    reason `PathCanon::Anchor` is, and built on it: "outside both roots" and "not placeable at
    all" are different facts with opposite consequences, and collapsing them to two is
    precisely what went wrong. `PathRole` is not a second spelling of `Anchor` — `Anchor`
    describes the path, `PathRole` decides what the manifest does about it — and the mapping
    between them is the switch, which carries no `default:` for the reason the other two
    callers' do not.
    - **A resolved relative path is recorded as a canonical token, not kept relative** —
      the opposite of what `KeyDependencySet` does with the same input, deliberately. A key
      input is only ever *digested*; a manifest entry has to be *localized back to a file*
      on the validating machine, which a relative entry can only do against that machine's
      working directory. `cc -c ../src/t.cpp` run from `build/` and from `build/sub/`
      relativizes to the same argument list and so shares a manifest key while naming
      different files — resolving before canonicalizing is what removes that, and it is why
      `CanonicalSourceToken` is one function both `TryDirectMode` and `RecordManifest` call
      rather than two spellings of `PathCanon::Canonicalize(cmd.source, …)`.
    - **An unanchored path refuses the manifest rather than being dropped.** Reachable only
      when the working directory itself is unavailable, so it costs essentially nothing —
      and dropping is the silent stale serve this whole classification exists to prevent,
      while refusing merely costs one compile direct mode.
    - **`current_path()` must be re-spelled in the layout's vocabulary before anything
      compares it against a root.** `getcwd(3)` answers with the kernel's *resolved* path,
      so a build under a symlinked prefix (macOS `/tmp` → `/private/tmp`, any symlinked
      `/home` or `/mnt`) reports a working directory sharing no string prefix with the root
      it is actually inside — and every root test here and in `PathCanon` is a string prefix
      comparison. `AnchorWorkingDirectory` matches by filesystem *identity*
      (`weakly_canonical` on both sides, longest root first as `CanonicalizeOne` does) and
      hands back the **root's own spelling** with the tail appended, so the one value that
      comes from the environment speaks the same language as the ones that come from
      configuration. Found by `compile-cache-e2e.sh` on macOS, where `mktemp -d` returns a
      `/var/…` path and `getcwd` reports `/private/var/…`; it silently cost direct mode
      rather than failing, which is why it is asserted end-to-end. This is *not* a fix for
      issue #66 — that is about the paths a **driver emits**, where both sides would have to
      move together; here only the cwd is re-spelled, and against roots that are already the
      layout's own.
    - **The "normalize, then put the layout's separators back" rule is spelled once,
      in `NormalizeForLayout`.** `std::filesystem` answers with the **host's** preferred
      separator while every root and absoluteness test is the **layout's**, so on a
      Windows host `/w/src/a.hpp` comes back backslash-separated and
      `IsAbsoluteForLayout` — which for a POSIX layout asks only about a leading `/` —
      reads an absolute path as relative. `PortableForm` had the only copy, inline, and
      the manifest side turned out not to have it; Windows CI is what said so, on this
      very change. The correction runs **one way only, deliberately**: a Windows layout
      keeps whatever separators it arrived with, because `PathCanon` spells a Windows
      root either way (`C:/src/proj` is a Windows layout) and every prefix test unifies
      separators before comparing — only the POSIX direction can mislead, since there a
      backslash is an ordinary filename character rather than a separator spelled
      differently. `ResolveAgainst`'s *join* stays the host's path arithmetic on
      purpose, and its contract says so: it resolves against a directory on this machine
      and its result is handed straight to `HashFileContents`, so it only ever runs
      where the layout and the host agree.
    - **`manifest-v4` moved alone, and the lock-step with `objkey-v*` is one-way.** An
      `objkey` bump must drag the manifest tag with it (a manifest points at an object key by
      value); the reverse costs nothing, since an unreachable manifest is re-recorded next
      compile and points at the same still-valid objects. The bump is *required* here because
      the defect is invisible to the key: a build with an absolute TU source but relative
      header paths keeps the same `canonicalSource` and args across this fix, so its
      under-recorded manifest keeps the same key, keeps being found, and keeps validating.
      Re-keying is the only thing that retires those entries.
    - **The emptiness guard did not move to a count, and that was the decision.**
      `RecordManifest` still refuses on `includes.empty()` — "did the compile report a
      dependency record at all?", which is the right question and `includes` is the right
      thing to ask it of. A count over `manifest.entries` cannot replace it: after the fix a
      one-entry manifest is *legitimate* (a TU whose every header is toolchain content is
      completely covered by that entry plus the stamp), so a threshold would refuse correct
      manifests while still not catching a misconfigured root. The zero-entry case it was
      standing in for is closed a layer down instead — `BuildManifest` takes the TU as its
      own `sourcePath` field and refuses when it cannot record it, which turns the invariant
      its doc-comment used to *state* into one it *enforces*. What the caller adds is a
      diagnostic, not a veto: `manifest: N entries from M reported dependency path(s) plus
      the source`, for the same reason the key's `dependency set: N of M` line exists — the
      recorded manifest cannot report this itself, because it still validates.
  - **`Cc::MissingReplayedDependency` stays as the backstop**, and still runs before a hit
    writes anything; a stale hit falls through to the real compile, whose STORE repairs the
    entry. Its filter is load-bearing in both directions: probing a depfile's rule target
    would make every hit a miss, because the target is the object file and it does not exist
    yet (hence `ParseDepFilePaths`, which excludes it, rather than a whitespace split);
    probing an absolute path outside both roots would make two machines with different system
    include prefixes miss on *every* compile forever, each re-storing the other's record.
    `/showIncludes` is covered alongside the depfile because Ninja reads it as `deps = msvc`;
    `MsvcDiagnostics` is not, because a diagnostic quotes a path rather than declaring a
    dependency on it.
  - **The residual, recorded deliberately:** two machines whose compilers print the *same*
    `--version` banner from *different* install prefixes still share a key and can still
    replay each other's toolchain paths. Closing that would mean hashing those absolute
    paths, i.e. giving up cross-machine sharing wholesale for the population it affects, so
    the guard above is what covers it and the trade is left where `DirectManifest` already
    put it.
  - **A root must be spelled the way the driver spells what it emits, and on Windows the
    drivers disagree.** Every root test is a string prefix comparison
    (`IsToolchainHeader`, `PathCanon::CanonicalizeOne`), so a root carrying an 8.3 short
    component matches nothing `cl` reports: `cl` resolves an include through the filesystem
    and prints `C:\Users\runneradmin\...`, while clang-cl echoes the spelling it was handed
    and prints `C:\Users\RUNNER~1\...`. Measured on a GitHub runner, where `%TEMP%` is the
    short form. Two failures follow from the one mismatch and they hide each other: every
    path is classified as outside both roots, so the keyed dependency set is **empty** (the
    two layouts of a moved header key together) *and* `ReplayGuard` skips every path it
    would have checked (so nothing reports it) — and the stored `/showIncludes` region is
    never canonicalized either, so the value carries the producing machine's absolute paths.
    Two symptoms to recognise it by, since neither the key nor the guard will say a word:
    a replayed note that kept the driver's mixed separators (`...\src\inc/h1.h`) was never
    tokenized, where a localized one is uniformly native (`...\src\inc\h1.h`); and the
    launcher's `dependency set: N of M reported path(s) keyed` line reads `0 of M` with M
    non-zero — the probe reported paths and every one was filtered out, which is a
    different fault from `0 of 0` (a driver that reports nothing on the preprocess line).
    `run-launcher-e2e.ps1` therefore puts its scratch trees beside the **build tree**
    rather than under `%TEMP%`; it does not try to expand a short name, because nothing
    dependably does — `Resolve-Path`, `Get-Item` and `[IO.Path]::GetFullPath` all preserve
    it, and `Scripting.FileSystemObject` was tried and echoed it back unchanged.
  - **The reconciliation translates the emitted paths INTO the build's spelling; it does
    not respell the roots (issue #66).** There are two spellings of every root and the
    launcher needs both, for opposite reasons. **Matching** must use the spelling the
    filesystem reports, because that is what a driver reports. **Emitting** must use the
    spelling the build system uses, because a replayed depfile's rule target has to be
    byte-identical to the `-o` path the build passed. Resolving the roots and using that
    form everywhere satisfies the first and breaks the second — measured: a build tree
    reached through a symlink stored `.../link/build/a.o:` and replayed
    `.../real/build/a.o:`, which Ninja rejects outright (`expected depfile ... to mention
    ...`) while make matches no rule at all and silently drops every header dependency.
    `RootReconciler` (`apps/fastcache-cc/RootReconciler.cpp`) holds both layouts and is the only
    thing that sees the resolved one: it canonicalizes an emitted path against the
    resolved roots and localizes the token into the as-given roots. Everything downstream
    — the roots on the wire, the key, the manifest, the replay guard, the localized
    regions — keeps speaking the build system's own spelling exactly as before, so no
    protocol version moves and nothing else had to learn about this. Consequences that
    are each load-bearing:
    - **It is its own translation unit, for the reason `CacheProtocol.cpp` already
      records.** `main.cpp` is in no test target, so logic that lives there has no unit
      coverage at all — the mistake this list notes having been made once with the wire
      framing. `RootReconciler.cpp` is compiled into both the launcher and
      `fastcache-cc-tests`, and its tests drive it through a table-backed fake
      `IPathResolver`: the conditions it exists for (an 8.3 short component, a `subst`
      drive, a junction) cannot be created on the host running the tests, and two of the
      three cannot be created on any host that is not Windows, so a fake stating the
      aliasing directly is what makes every case reproducible everywhere.
    - **The translation is PathCanon's own two operations, not a third prefix test.**
      `Canonicalize` against one layout, `Localize` into the other. A rule written out
      again here is a rule that can come to disagree with the one everything else
      applies, which is the whole failure mode this entry documents.
    - **Only paths the COMPILER authored are reconciled; one the BUILD SYSTEM authored is
      already the spelling this build wants, and it is named BY VALUE.** A depfile is the
      single grammar carrying both, so `Cc::RootReconciler::Region` takes the object path and
      returns that span verbatim wherever it appears. Respelling it hands the build system
      back an output it never asked for, and a build whose `-o` does not share a spelling
      with `FASTCACHE_BINARY_DIR` then gets a depfile Ninja rejects outright and make
      matches against no rule at all. **By value and not by position**, because position
      does not say what a path is: `-MP` emits a phony rule per header whose TARGET is a
      path the compiler reported, and exempting every target would leave those unreconciled
      and so uncanonicalized, sending a consumer a depfile that points `-MP`'s
      deleted-header protection at files it cannot stat. Canonicalization on the daemon
      still rewrites every span, target included — a consumer needs the target pointing
      into ITS build tree.
    - **Symmetry is the property, and it is why this is a seam rather than a call at
      each comparison.** Expanding only the emitted paths breaks clang-cl exactly as
      spelling only the root long breaks `cl`. `Cc::IPathResolver`
      (`apps/fastcache-cc/PathResolve.hpp`) is where the filesystem lives, and it lives
      in the launcher because `PathCanon` also runs on the **daemon**, over a producing
      machine's roots that do not exist there (`CompileCacheHandler::HandleStore`), so it
      may never touch a filesystem. Hence `PathCanon::RewritePaths` taking the transform
      as a parameter: the grammar that finds path spans stays in the library.
    - **A path already spelled the way this build spells things is returned UNTOUCHED, and
      that is the correctness case rather than an optimization.** Resolution rewrites a
      symlink anywhere in a path, not only in the root prefix, so round-tripping an
      in-tree one (`src/inc -> src/real-inc`) would key it under this machine's real
      subpath while a machine holding the same content without that symlink keys under the
      plain one. Two byte-identical checkouts would stop sharing every entry — the property
      the launcher exists to provide, traded away to repair a spelling that was never wrong
      here. So `Translate` asks the as-given layout FIRST and only falls through to the
      resolved round trip when that fails. Measured: two such checkouts key identically,
      and at the same key a launcher built before this change produces.
    - **That fast path is also why it costs nothing, and the memo is what bounds the rest.**
      The resolver memoizes per parent DIRECTORY. Measured on this repository's own
      `CompileCacheHandler.cpp`: 1099 reported paths, 60 filesystem calls (all of them
      toolchain headers, which lie under no root by either spelling), and 271-276 ms per
      cache hit against 271-276 ms before — inside the noise. Per-path resolution with
      neither fast path nor memo is the version the issue worried about: roughly 1099 calls,
      ~25 ms, which on a 45 ms preprocess is not a rounding error.
    - **`Resolve` leaves the leaf as spelled, so an argument naming a directory must use
      `ResolveDirectory`.** The memo works per parent, so the final component of whatever
      is handed to `Resolve` is never resolved — fine for an include note (the name comes
      from an `#include` directive and is already long) and wrong for an `-I` pointing at
      a symlinked include directory, whose *own* last component is the aliased one. The
      argument list and the translation unit therefore go through `ResolveDirectory`,
      which also makes an `-I` resolve the same way the headers reported from under it
      do; there are few enough arguments that resolving each completely costs nothing.
    - **A relative path is returned verbatim.** It resolves against the compile's working
      directory and is therefore already machine-independent; absolutizing it would either
      re-key it for nothing or, when the working directory lies under neither root, push it
      outside both and have `KeyDependencySet` drop it. `RootReconciler::IsInTree` has to
      agree, and it asks its question of the RECONCILED path against the AS-GIVEN layout —
      the same two values that decide whether the path reaches the key. Asking the resolved
      layout instead answers a question nothing else asks, and a compile could then key
      nothing while the diagnostic reported it in-tree.
    - **A trailing separator is trimmed off each root, and leaving it is worse than a
      no-op.** `PathCanon::Layout` takes roots without one — `IsSegmentPrefix` requires a
      separator AFTER the root, so `/x/build/` matches nothing under `/x/build` — and a
      build system exporting one is doing nothing wrong. Untrimmed, nothing under that root
      canonicalizes (so the stored value keeps this machine's absolute paths) AND the path
      then gets a second chance through the resolved root, which `weakly_canonical` returns
      without the separator, after which `JoinLocalized` adds one of its own and the
      replayed rule target reads `/x/build//a.o`. A bare root (`/`, `C:\`) is left alone:
      it IS its trailing separator, and trimming `C:\` to `C:` would also flip the
      separator style `JoinLocalized` derives from it.
    - **The "never throws" contract is guarded at the entry points, not around the
      filesystem calls.** Every `std::filesystem` call here takes an `error_code`, so
      guarding only those looks sufficient — but on Windows `std::filesystem::path`
      *stores* a `wstring`, so constructing one from a narrow string converts through the
      active code page and `path::string()` converts back, either of which throws on a
      character the code page cannot represent. Those conversions happen before any step
      runs. `main()` has no catch of its own, so an escape breaks the build over a path
      the launcher merely failed to tidy up.
    - **No schema tag moved, and the reconciliation is why it did not have to.** Because it
      is identity wherever the spellings already agreed, `objkey-v3`/`manifest-v3` and
      `CompileValueVersion` all stay: verified, a launcher built from the previous commit
      stores entries this one HITs, and a manifest it recorded still direct-hits. Where the
      spellings did NOT agree the cache was not working, so there is nothing to invalidate.
    - **The diagnostic is verbose-gated, which reverses the original call.** It started
      ungated, on the reasoning that this defect is invisible and a diagnostic nobody
      enables is as silent as none. Two rounds of narrowing failed to find a condition
      that means "broken" reliably enough to justify four unsilenceable lines on the
      compiler's stderr for every translation unit: a source outside both roots is an
      ordinary CMake layout (`add_subdirectory(../shared shared)`, a superbuild,
      ExternalProject), and a message that cries wolf on a healthy build is the one that
      gets ignored when it is right. What tipped it is that `RootReconciler` now REPAIRS
      the mismatch rather than merely detecting it, so this is a backstop and not the
      mechanism.
      **The condition stayed narrow even so**, and reports the roots not containing the
      SOURCE rather than "nothing was keyed", because the broader one has an innocent
      reading: `/showIncludes` never names the primary source, so on MSVC a translation
      unit including only third-party headers outside the roots — Qt, a vendored SDK,
      anything the four-entry `ToolchainMarkers` table does not know about — reports paths
      and keys none of them while being perfectly healthy. `0 of 0` stays quiet too: that
      is a driver reporting nothing on the preprocess line, a different fault this message
      would misdescribe.
    - **The e2e cases create the second spelling deliberately** rather than relying on one:
      `compile-cache-e2e.sh` symlinks a root, `run-launcher-e2e.ps1` substitutes a drive
      (`subst`, not an 8.3 name — 8.3 creation is off on many volumes). Both compile one
      tree through both spellings and require the second to HIT the first's entry.
      **Which paths get which spelling is the whole design of the POSIX case**, and getting
      it wrong makes the case vacuous: the roots and the OUTPUT paths are the aliased
      spelling while the source and include paths are the real one, which is what `cl`
      actually does — a build system spells everything one way and the compiler reports its
      dependencies resolved the other way. Spelling the outputs the same way as the roots is
      what a real build does (`-o` and `FASTCACHE_BINARY_DIR` come from one generator) and
      it is what makes the second property testable at all: the replayed depfile's rule
      target must be byte-identical to the one the compiler wrote. A third leg repeats it
      with roots carrying a trailing separator. Both assertions were verified by
      reintroducing the bug they catch and watching them fail, and the whole case is checked
      against a launcher built from the previous commit, where it must fail with
      `dependency set: 0 of 2`. The Windows case names its object by the real path in both
      legs. That was load-bearing when it was written, because a fused `/Fo<path>` reached
      the key verbatim and would have keyed the legs apart for an unrelated reason; the
      bullet below has since made every path-valued flag relativize in its fused spelling
      too, so the precaution is now belt-and-braces rather than the thing holding the case
      up. Every other case keeps its roots unambiguous,
      so a regression in the reconciliation cannot present as a failure of something else.
## Relativizing a path-valued flag

- **A flag's value is relativized off one table, or it is relativized in one spelling
  only.** A path-valued flag can be written two ways — `/Fo <path>` and `/Fo<path>` —
  and the separated form needs no table at all: the value is a bare argument, so it
  reaches the source-path branch on its own. Only the *fused* form has to be split, and
  the table that split it listed the include-dir prefixes and nothing else. So the object
  output was relativized in the spelling nobody uses and left absolute in the spelling
  **every** build system driving MSVC writes, which put the producing machine's object
  path into every Windows key: two checkouts at different roots could never share an
  entry, and the launcher's whole reason for existing was off on that platform. It passed
  unnoticed because the unit test asserting `/`-still-introduces-an-option happened to use
  the separated form, and because the Windows cross-depth e2e case was hitting an entry an
  earlier case had stored from the same directory with the same `/Fo` path — a spurious
  pass that would have survived cross-depth sharing being broken outright (fixed by giving
  each case its own string literal, the device `check_header_move` already used). The three
  tables are now one, `CmdLine`'s `PathValueFlags()`: it answers whether a bare occurrence
  consumes the next argument, which flag names the object output, which flags the preprocess
  line must drop, and whose fused value the key relativizes. Consequences that are each
  load-bearing: a row carries a **driver family**, because the family is *not* derivable
  from the introducer — MSVC drivers accept `-` for every option, and `-MT` names a
  dependency target for a GNU driver while selecting the static multithreaded runtime for
  an MSVC one, so a row matched on `-` alone would make `cl -MT` swallow the source file.
  Which *introducers* may match is still decided by the **layout**, not the host and not
  the driver, for the reason recorded above: on POSIX a leading `/` starts an absolute
  path, and matching `/I` there splits a checkout rooted at `/Infra`. And the drop list no
  longer spells `/Fo` or `-MF` itself — it drops every row whose role is not `IncludeDir`,
  so a spelling added to the table is dropped by construction rather than by someone
  remembering the fourth place.
  - **This did not bump `objkey-*` / `manifest-*`, and that is the deliberate half.**
    (The tags now read `v4`, moved by issue #64's manifest half — a different change, for
    a reason this one does not reach; the re-key here rides along in that invalidation
    event rather than costing a second. The reasoning below is kept because it is the case
    for *not* bumping, and it stands on its own.)
    The tag versions the key *construction* and the rules the stored value is written
    under; both are unmoved by this change, and `ComputeKey`'s golden vector did not move
    for it either. What changed is one *input*, for exactly the builds whose command line
    carried a machine-specific string it should never have carried. Old entries stay
    correct in their own terms and simply stop being addressed — they miss and are
    rewritten. The mis-serve a tag exists
    to prevent is unreachable here: an old key could only become a new key if a build
    literally passed the text `<BUILDTREE>`, and a `/Fo` path that was already relative
    canonicalizes to itself and does not move at all. A bump would meanwhile invalidate
    every POSIX entry, where nothing changed. Direct mode needs no bump either, and not by
    luck: `ComputeManifestKey` takes the relativized args too, so a manifest key moves
    exactly where an object key does, in lock-step, for exactly the affected builds — the
    property whose *absence* is what forced the `manifest-v2`/`v3` bumps, and whose
    presence is why `manifest-v4` had to be argued from the manifest side rather than
    from this one.
## The digest

- **A key that is 128 bits wide is not a key with 128 bits of strength, and four
  lanes of one polynomial are one lane.** The object key was four CRC32C digests of
  the same blob, distinguished only by a leading salt byte. CRC is affine over
  GF(2), so with `A` the per-byte state-update operator and `S_i` the state after
  salt `i`, `quarter_i XOR quarter_j` is `A^len(blob) * (S_i XOR S_j)` — a value
  that depends on the blob's *length* and on nothing else about it. Matching one
  quarter therefore forced all four and the key carried **32 bits**. Measured
  before the fix: one distinct XOR value across 2000 random equal-length 512-byte
  blobs, and a full 32-hex-char collision after 86,125 equal-length inputs — the
  birthday bound for 32 bits, against the ~10^5 entries a shared team cache
  reaches. Equal length is not an exotic condition, it is the ordinary shape of a
  source edit (`return 1;` → `return 2;`) and the blob is dominated by preprocessed
  text; and the consequence is not a miss but an unrelated translation unit's object
  file served under a **zero exit code**. Two dead ends worth not re-walking:
  varying the salt *length* per lane does not help (still one distinct XOR value —
  the salt only changes `S_i`), and four CRC lanes with *distinct* polynomials do
  work but cap out below 128: three of the four best-studied CRC-32 polynomials
  (Castagnoli, Koopman, Koopman-K/2) carry the factor `(x+1)` and only IEEE 802.3
  does not, so the least common multiple of the four has degree 126, not 128. The
  digest is now MurmurHash3 x64_128 in `Core/MurmurHash3.hpp` — an existing,
  published algorithm rather than a bespoke construction, which is the whole point: its conformance is checkable
  against SMHasher's verification value `0x6384BA69`, and a construction assembled
  here would have nothing to be checked against. Consequences that are each
  load-bearing: `objkey-v3` and `manifest-v3` moved together (to v3) because that was one
  invalidation event, and `HashFileContents` moved with them — it paired one CRC32C
  with the byte count, which is 32 bits against exactly the same-length case, and it
  is what a *direct hit* revalidates against, so a collision there does not miss, it
  decides an edited header is unchanged. `header-state-v1` deliberately did **not**
  move, because nothing is stored under it and a version with no work to do is the
  mistake `PathCanon::CanonError` already records. Domain separation between the
  three key spaces is now the leading schema tag rather than the salts, and each piece
  of a key is **length-prefixed** (`kind`, big-endian `u64` length, bytes) rather than
  terminated by a separator byte. That second half is not cosmetic, and it was found
  in review of this very change: terminating a value with a byte that can occur
  *inside* a value is not a framing at all, so `{compilerId="cc\0d", preprocessed="x"}`
  and `{compilerId="cc", preprocessed="d\0x"}` digested identically — the same silent
  cross-TU mis-serve, reached by a different route, and reachable rather than
  theoretical because preprocessed text can carry a raw NUL and a build system can
  pass an argument containing `0x01`. Fixed here rather than filed for the reason
  `HashFileContents` was: `v3` re-keys the whole cache once, so finding it later would
  have cost a `v4` invalidation for a defect of the class this change exists to close.
  The length must be big-endian for the same reason the digest's block loads must be
  little-endian — a *host*-order length makes the key differ between machines.
  The residual, recorded deliberately: MurmurHash3 is not collision-resistant against
  an **adversary**, and that is accepted because the key is not a security boundary —
  anyone who can STORE can already write a wrong object under a correct key. Closing
  it would mean a keyed or cryptographic hash *and* a trust model for STORE, which is
  a different change from this one.
  - **The digest must be bit-identical on every machine that shares the cache, and
    the `x64` in its name is a variant, not a target.** MurmurHash3 defines `x86_128`
    (four 32-bit lanes) and `x64_128` (two 64-bit lanes); they produce *different*
    digests, so the variant is part of the format. Nothing in the implementation is
    architecture-specific, and four hazards that would have made it so are closed by
    construction: `char` is signed on x86-64 Linux and **unsigned on aarch64**, so a
    byte widened through a plain `char` sign-extends differently and would have split
    Apple Silicon from everything else — everything is `std::byte`/`std::uint8_t`;
    block loads go through `ReadLittleEndian` rather than a native-order read; that
    same read would be unaligned and therefore UB whatever the hardware tolerates,
    which is also what `-fsanitize=alignment` traps under `clang-debug`; and rotation
    uses `std::rotl` rather than a shift pair that is UB at zero. A divergence here
    would not fail, it would silently split the cache in two with every machine
    missing on every entry the others wrote — so the SMHasher vector is checked on
    the arm64 `macos-14` job as well as x86-64 Linux and Windows, and it sweeps all
    256 tail lengths, which is where such a slip surfaces first.

## Store caps, and a refusal the client can read

- **A cache that can fail a build is not optional, and a refusal nobody can read
  is not a refusal.** The launcher STOREs an object by streaming one frame, and the
  daemon refused an over-cap frame by replying and then closing — while the sender
  was still writing. Two independent defects met there. Nothing in `fastcache-cc`
  suppressed SIGPIPE (the daemon does, in `Net/BlockingSocket`, but the launcher
  deliberately does not link the library), so the write that met the closed peer
  **killed the launcher with signal 13**; the build system saw a compile die of a
  signal even though the object file it asked for was already complete and correct
  on disk, and no retry could ever converge because the outcome was deterministic.
  Measured on a 356 MB object (C++23 templates plus `-g`) against the 256 MiB
  default cap, and reproduced here at 80 MB against the 64 MiB floor
  `SessionContext` keeps regardless of `--storage-max-value`. The launcher had
  correct fall-back logic for every store failure and reached none of it: a signal
  is not a return value. Three consequences, each load-bearing and each at a
  different layer, because any one alone leaves a hole the others do not cover:
  - **Suppression is per socket, not process-wide.** `SO_NOSIGPIPE` on macOS/BSD,
    `MSG_NOSIGNAL` on Linux, with `::signal(SIGPIPE, SIG_IGN)` only as a last
    resort for a platform with neither. The daemon can take the process-wide
    ignore; the launcher cannot, because an ignored disposition is **inherited
    across exec** and the launcher spawns the preprocessor and the real compiler —
    a process-wide ignore here silently changes how every compiler it fronts
    behaves. On macOS the socket option is also preferred over the newer
    `MSG_NOSIGNAL` *even though the SDK declares it*: the macro comes from the SDK
    while `CMAKE_OSX_DEPLOYMENT_TARGET` lets the binary run on an older kernel,
    and a flag the kernel does not know fails the send rather than the signal.
  - **The daemon drains an over-cap frame before refusing it**, exactly as it
    already does for an unknown opcode, so the sender's write completes and it
    can read the typed `payload-too-large` naming *both* numbers — the one message
    that tells an operator which way to move `--storage-max-value`. This is the
    "a rejection can be a reply instead of a close" rule two bullets up, applied to
    the one path that was still closing. `ByteReader::Skip` discards in chunks, so
    the memory the cap protects is still never taken. Bounded at a small multiple
    of the cap rather than by a knob of its own, because the cap is already the
    operator's statement of the largest thing this server will handle.
  - **The launcher declines the store before connecting**, at
    `FASTCACHE_MAX_STORE_BYTES` (256 MiB by default, `0` disables). Surviving the
    refusal is not enough: without a ceiling every rebuild of that translation
    unit pays the full transfer to be told no, and one 356 MB entry would dominate
    a cache sized for thousands of ordinary objects. The default matching the
    daemon's `--storage-max-value` default is a *chosen coincidence*, not a
    negotiation — there is deliberately no handshake, so raising one and not the
    other leaves the other refusing.

  The residual, recorded deliberately: the 64 MiB floor in `SessionContext` means
  a cap *below* it cannot be tested end-to-end without a genuinely large fixture,
  so the e2e drives the client ceiling and `TcpClient_test` pins the socket half
  (its `HangUpPeer` case terminated the test binary with signal 13 before the fix —
  verified by re-neutering it, since a regression test for a fatal signal that
  cannot be seen to fail is worth nothing).

## Accepted trade-offs

These are argued in place above and are **not** open work — do not "fix" one
without reopening the argument:

- MurmurHash3 is not collision-resistant against an adversary. Accepted because
  the key is not a security boundary: anyone who can STORE can already write a
  wrong object under a correct key. Closing it needs a keyed hash *and* a trust
  model for STORE.
- Two machines whose compilers print the same `--version` banner from different
  install prefixes share a key and can replay each other's toolchain paths.
  Hashing those paths would give up cross-machine sharing wholesale;
  `Cc::MissingReplayedDependency` is the backstop instead.
- The 64 MiB floor in `SessionContext` means a cap below it cannot be exercised
  end-to-end without a genuinely large fixture, so the e2e drives the client
  ceiling and `TcpClient_test` pins the socket half.

## Open work

- **[#104](https://github.com/LASTRADA-Software/fastcached/issues/104)** — a
  drive-relative path under no root is neither keyed nor guarded, so a moved one
  replays a stale depfile. clang-cl only, and only for a drive-relative `-I`.
- **[#105](https://github.com/LASTRADA-Software/fastcached/issues/105)** — the
  `dependency set: 0 of M` line now has two causes and fingerprints neither.
- **[#64](https://github.com/LASTRADA-Software/fastcached/issues/64)** — a
  relative include-dir argument still reaches the key verbatim through
  `RelativizeArgs`, so two build trees at different depths key apart on the
  arguments even though their dependency sets now agree.
