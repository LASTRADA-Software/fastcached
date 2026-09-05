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

## The compiler identity is the driver AND the target it generates for

A banner identifies the **driver**. A driver's code generation is not a function of
the driver alone, and for `clang-cl` it is not even close: clang detects the MSVC
installation it will be compatible with and sets `-fms-compatibility-version` from
it, and clang's Microsoft C++ ABI gates version-specific code generation on that
value. Same `clang-cl.exe`, different MSVC beside it, different object.

Measured on clang-cl 22.1.3, one binary:

| where it runs | effective `-fms-compatibility-version` |
| --- | --- |
| a developer command prompt | `19.51.36252`, from the MSVC install |
| a Windows **service** (no `INCLUDE`, no prompt) | `19.33`, clang's built-in default |

A `noexcept` function type mangles `P6AXXZ` below 19.12 and `P6AXX_E` from 19.12 on,
so this reaches defined **and** undefined symbols.

It is not an MSVC-only shape. Stock `g++` on Ubuntu x86_64 and aarch64 prints one
`--version` first line, so an architecture-independent translation unit keyed the
same on both — the same defect with no MSVC anywhere near it.

- **The key folds the target; the fingerprint does not.** They answer different
  questions and one string cannot serve both. A fingerprint decides which **worker**
  may serve a client, so folding the target in would split a developer-prompt
  launcher from a service-run worker — the mismatch #145 removed, reintroduced for a
  fact the dispatch line now states outright. A key decides which **object** may be
  served, and there the target is load-bearing. `CacheCompilerId` builds the first;
  `CompilerBanner` alone still feeds `CachedToolchainFingerprint`, and the compile
  node must keep computing it the same way or the two ends stop matching in silence.
- **A dispatched compile states the target on the line, first.** A worker otherwise
  re-derives it from its own machine. `--target=<triple>` goes **ahead** of the
  build's own arguments, so a `--target=` or `-m32` the build states still wins, as
  it does locally — which is also why probing the ambient default is sufficient and
  the compile's own command line is not needed. Appending it would override the
  build, and compiling for a target the build did not ask for is a wrong object
  rather than a failed one.
- **`cl` has no target to ask for, so its BANNER has to carry one — which is why it
  must be a real banner**
  ([#195](https://github.com/LASTRADA-Software/fastcached/issues/195)).
  `TargetDiscovery::None` is right about MSVC: which code generator runs is decided
  by which `cl.exe` is invoked, and no command line restates it. So the whole of an
  MSVC identity is the banner — in `CacheCompilerId` *and* in
  `CachedToolchainFingerprint`, the one compiler where the two still say the same
  thing. It was the constant `cl` on every MSVC ever installed, because `cl` answers
  `--version` by printing its banner and *then* exiting 2, and the banner requires a
  zero exit. Bare `cl` prints the same line and exits 0, so asking it that way
  (`DriverSpec::versionFlags`, MSVC's row **empty**) is what makes
  `Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36252 for x64` the identity
  — version *and* target, out of a driver that can be asked for neither. The x86 and
  x64 `cl.exe` of one toolset share an include tree exactly, so until then they were
  one toolchain to the key, to the fingerprint, and to the scheduler alike.
- **One flag closes both halves.** The cc1 triple EMBEDS the compatibility version:
  `--target=x86_64-pc-windows-msvc19.11.0` produces an object byte-identical to
  `-fms-compatibility-version=19.11`. It therefore also closes the case where a
  `clang-cl` on a POSIX worker — which after #145 had nothing Windows-specific left
  in its digest — could return an ELF object to a client expecting COFF.
- **Read the `-cc1` line's `-triple`, never the `Target:` header.** Both name a
  triple and the header comes first, but the header is UNVERSIONED
  (`x86_64-pc-windows-msvc`). A parser that took whichever it found first would pin
  the architecture, drop the code-generation contract, and look entirely correct
  doing it.
- **Validate what comes back, and reject a leading dash.** The value is read
  positionally — the argument after `-triple` — so an option carrying none leaves
  the next FLAG in its place, and `-emit-obj` satisfies every other test for a
  triple. It would reach both a cache key and a `--target=` argument.
- **Discovering a target and stating one are different questions.** `gcc` can be
  asked and cannot be told: it is a fixed-target driver with no `--target=`, so its
  target belongs in the **key**, which decides which object may be served, and must
  never reach a **dispatched line**, where the driver would reject the flag and fail
  the compile. `TargetPinPrefixFor` is the seam, and an empty prefix means exactly
  that — identified, not pinned. Only `cl` is neither: which code generator runs is
  decided by *which* `cl.exe` is invoked, and no command line can restate that.
- **Which line is authoritative is the driver's property, not the parser's.** `gcc`
  prints no `-cc1` invocation — its frontend is `cc1plus` and takes no `-triple` —
  so its `Target:` header is the whole answer. On a clang driver that same header is
  the unversioned half-answer. Two mechanisms, chosen by the table; one reader
  trying both lines would silently downgrade every clang driver whose frontend line
  it happened to miss.
- **An empty triple means UNCHANGED, not an empty field.** `CacheCompilerId` returns
  the banner byte for byte, so a driver that states nothing keeps every entry it has
  written. Appending an empty field would re-key every cache in the fleet to buy
  nothing.
- **`cc` and `c++` name a policy, not a product, so the banner decides.** On macOS
  `/usr/bin/c++` is Apple clang and is CMake's default; by name it lands in the
  `gcc` row. That was harmless while the two rows were identical and stopped being
  harmless when that row began deciding how a target is found. The correction is a
  string test over the banner already in hand, never a second spawn — and it must
  not touch `clang-cl`, whose banner is `clang version ...` byte for byte as plain
  clang's, so the NAME is the only thing separating those two drivers. Correcting
  the flavour *after* the line is parsed is safe only because the `gcc` and `clang`
  rows agree on every column the parse reads; a test asserts that, and if it ever
  fails the correction has to move above `ParseCommand`.
- **The two halves are framed, not concatenated.** `("ab", "c")` and `("a", "bc")`
  would otherwise key alike. A newline separates them because a banner is one line
  by construction and a triple is `[A-Za-z0-9._-]`, so neither half can contain it.
- **The preprocessed text does not discriminate, so do not lean on it.** `_MSC_VER`
  changes with the compatibility version but need not survive preprocessing: a TU
  including `<cstdio>` preprocesses to **byte-identical** text at 19.29 and 19.51.
  The hazard is not confined to include-free translation units.
- **The triple is folded verbatim, and over-specification is the deliberate half.**
  Clang's Microsoft ABI gates on major.minor, so two MSVC builds differing only in
  patch generate identically and key apart — a lost match on every Visual Studio
  point release. It is not truncated because the same field means something else
  elsewhere: on Apple targets it is the **deployment target**, which does change code
  generation. One rule cannot serve both, and a per-platform rule would be guessing
  about triples this code has never seen. Over-specifying costs a recompile;
  under-specifying serves an object built by a different code generator.
- **Failing open is safe in ONE direction only.** A probe that answers nothing on
  one end leaves that end keying as it did before, so the two machines key apart and
  stop sharing — a miss. Answering nothing on **both** ends puts both back on the
  banner alone, which is the original defect, in silence. It never turns a working
  match into a wrong one and never repairs a pair that was already wrong, so a
  driver that has a mechanism and declined to use it is reported.
- **The probe is not cached, and the reason is the direction of the error.** The
  fingerprint's stamp covers the compiler binary and its include roots, none of
  which move when the MSVC install beside `clang-cl` is upgraded — so a triple
  memoized against it goes stale in the one direction that yields a *wrong hit*
  rather than a miss. It costs a second driver spawn per invocation on a clang
  driver, beside the `--version` one already paid there.

## What the key is a function of

- **A key determines two artefacts, so it must be a function of both.** Preprocessing
  suppresses line markers (`-E -P`, `/EP`) so a checkout path never reaches the key —
  which is what makes a key portable, and equally what made it *invariant under a header
  move*. Move a header without changing a byte of it and the token stream is identical:
  the object is still correct and was served, while the depfile, which is nothing but
  paths, named a file that is gone. That is worse than a miss, because Ninja records the
  dependency, cannot stat it, rebuilds, hits the same value, and never converges — with a
  successful exit code every time. The dependency path set is therefore part of the key
  (`objkey-v6`, `KeyInputs::dependencyPaths`), captured on the preprocess run the launcher
  already makes rather than in a second probe: measured at **+1.5% on a 45 ms preprocess**,
  because the compiler has already opened every one of those files. A move is a different
  key by construction, so the *pre-move* entry survives the move rather than being
  overwritten — which is the property `check_header_move` asserts by moving the header
  back and requiring a HIT. Anchored as `fastcache-cc: HIT`: the launcher prints
  `STALE HIT (...); recompiling` on its way to a MISS, so a bare `grep HIT` is satisfied by
  exactly the collapse the case exists to reject. `ComputeManifestKey`'s `manifest-v*` tag moves
  in lock-step with `objkey-v*` for a related reason — a manifest stores the object key
  *by value* and its own key never sees the object-key schema, so a manifest written by an
  older launcher keeps resolving to an older object; direct mode is on by default and
  short-circuits before the preprocessed path,
  so without the second bump the re-key never happens where it matters most. The coupling is
  ONE-WAY, though, and both tags currently read `v5`: an `objkey` bump must drag the manifest
  tag with it, while the reverse is free — and free is the direction `v5` itself moved in
  (issue #111), the manifest half forced and the object half taken only to keep the two from
  sitting numerically apart.
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
    - **Sharing the classifier is not sharing the QUESTION, and the missing half was
      "under a root".** `IsToolchainHeader` answered it with a bare `starts_with` and no
      segment-boundary check while `PathCanon::Canonicalize` — which every one of the three
      then hands the path to — has always required the boundary, so under a source root
      `/home/dev/proj` the sibling `/home/dev/project-x/a.hpp` was project content to the
      classifier and under no root to the canonicalizer
      ([#562](https://github.com/LASTRADA-Software/fastcached/issues/562)). It failed SAFE
      — `starts_with` only ever classifies *more* paths as project content, so such a header
      was hashed and revalidated rather than dropped, and the unsafe direction (a project
      header called toolchain, which revalidates forever) could not be produced by it. What
      it cost was the invariant this bullet states, and an invariant false in one of three
      places is not available to reason from in the other two. One
      `PathCanon::RelateToLayout` now answers it — a `RootRelation` rather
      than a `bool`, for the reason below — taking NATIVE forms on purpose: `IsSegmentPrefix`
      sees only comparison
      forms whose separator is `/`, while `DirectManifest`'s `ToComparable` folds to `\`
      to match its backslash-spelled toolchain markers, so handing one's comparison form to
      the other asks for a boundary byte that cannot be there. Do not "unify separator
      handling" into `IsSegmentPrefix`; its single-separator test is correct for its input
      domain and a second branch there would be dead.
      - **The near miss the bug produced is kept, and is now ASKED rather than inferred.**
        `PathDisposition::Uncanonical` and `ManifestFault::Uncanonical` used to be whatever
        fell out of the gap between the two predicates. They are the one root fault of the
        three an operator repairs by editing a *root*, so closing the gap without naming the
        state would have turned a typo'd root into `outside roots` and sent them looking for
        a file that is exactly where they put it. `RootRelation::NearMiss` is that question
        asked directly, and the "under neither" half is load-bearing, since a build tree
        spelled as the source root's sibling makes a path a near miss of one root and
        legitimately under the other — which is why `RelateToLayout` answers `Under` before
        `NearMiss` rather than looping at the call site. The three consumers then agree the
        path lies outside the roots while still reporting *which* kind of outside.
      - **And the near miss is READ OFF the classification, never asked afterwards.**
        `Cc::ClassifyAgainstRoots` returns `Project` / `NearMissRoot` / `Toolchain` in one
        answer, with the marker scan winning, because a marker match re-examined against the
        roots is overruled by them: `<root>-deps/vcpkg_installed/.../core.h` character-
        prefixes `<root>` with no boundary, so a separate near-miss call reported an
        ordinary vcpkg layout as a misspelled root — refusing every manifest that included
        it, and probing it in the replay guard so every hit was discarded on a machine whose
        dependencies sit elsewhere. A short source root (`C:\P`) makes that total: every
        Windows SDK header becomes a "near miss". Two questions of one path, answered by two
        calls, is how the second one gets to overrule the first — which is also why there is
        no `IsNearMissRoot` predicate beside `IsToolchainHeader`: a name shaped like its peer
        invites a caller to reintroduce exactly that shape.
      - **The replay guard still probes such a path, and that is a difference of QUESTION,
        not of predicate.** Its exclusion of outside-roots paths rests on the toolchain stamp
        covering them collectively, so a machine with a different toolchain has a different
        key and never sees the value; a near miss is covered by nothing, so the argument does
        not reach it and skipping it would replay a path naming a file that may be elsewhere
        here — issue #53 exactly. The manifest REFUSES over one for the mirror reason: a
        dropped near miss leaves a hit revalidating everything except the header being
        edited, so it is the only outside-roots path that is a refusal rather than a drop.
      - **A fourth spelling was looked for and one exists, deliberately.**
        `NormalizeForLayout`'s helper decides "inside a root" with `weakly_canonical` +
        `lexically_relative` — filesystem identities rather than spellings, and it needs the
        TAIL rather than a bool. Different question, stated at the site; not a copy.
    - **The compiler identity that "covers" them is a string a compiler had to say, so
      every driver is asked the one way it answers**
      ([#195](https://github.com/LASTRADA-Software/fastcached/issues/195)). Dropping 476
      of 635 headers rests entirely on the clause above, and the identity doing the
      covering is `CompilerBanner` — `main.cpp`'s `toolchainStamp`, which is what
      `ComputeKey`, `ComputeManifestKey` and `ValidateManifest` all fold in. `cl` has no
      `--version`: asked for one it prints its banner, then errors, then exits 2, and the
      banner requires a **zero exit** so a driver that cannot answer falls back to its
      normalized basename rather than fingerprinting on an error message. So every MSVC
      compiler ever installed identified as the string `cl` — and an MSVC key covered
      neither the toolchain headers (dropped) nor the compiler (a constant). Measured: a
      TU compiled under 14.51 was STORED, and the identical compile under 14.44 HIT the
      same key and received an object embedding `_MSC_FULL_VER` 195136252, which 14.44
      cannot produce. Zero exit code, no diagnostic. The gate is right; the probe was
      wrong, and how to ask is now `DriverSpec::versionFlags` — MSVC's row **empty**,
      because bare `cl` prints the same banner and exits 0. The banner names the target
      too ("... for x64"), which is the only thing separating the x86 and x64 `cl.exe` of
      one toolset: those two share an include tree exactly, so a fingerprint could not
      tell them apart either and a fleet dispatched between them.
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
    defect its own bullet below exists to close.
    - **What that left over is closed by refusing the compile, not by covering the path
      (issue #104).** A drive-relative path under no root was dropped from the key AND skipped
      by the replay guard — each filter right on its own terms, and nothing anywhere asking
      whether SOMETHING covered the path. A header moved inside it does not re-key, so the
      stale entry is still found, and nothing probes it, so nothing notices: a replayed depfile
      naming a file that is gone, under a zero exit code. Covering it is not available — it
      would mean recording the producing machine's per-drive cwd in the value, which is exactly
      the machine-specific state the key exists to keep out — so the compile is refused
      instead, the way a module interface unit is, and says so under `FASTCACHE_VERBOSE`. Six
      things about its shape are load-bearing:
      - **It is a ROOT test that begins with an anchor test, and both halves cut opposite
        ways.** Refusing on the anchor alone would take a drive-relative *root* (`C:src\proj`)
        out of the cache wholesale, and that layout is portable precisely because the consumer
        substitutes its own root. Asking the roots alone would refuse every compile that
        reaches a system header. The anchor comes first for a second reason as well:
        `AnchorForLayout` never reports `DriveRelative` under a POSIX layout, so the whole rule
        is inert there without a single canonicalization.
      - **The question is asked TWICE, of two different inputs, and neither ask is redundant.**
        The dependency-set ask is the authoritative one: it sees the paths the compiler actually
        opened, so no list of flag spellings has to be complete for it to be right — which
        matters, because `CouldNameAFile` already records in as many words that such a list is
        what cannot be kept complete. `Cc::UnkeyableArgument` asks it of the command line
        instead, and buys the ORDERING: direct mode runs before the preprocess and validates a
        manifest whose entries came through the same filter and therefore dropped the very path
        in question, so a manifest recorded by an older launcher would keep direct-hitting a
        stale object forever. Asking before direct mode retires those entries by making them
        unreachable.
      - **The two asks share ONE classifier, and issue #105 is what made that possible.**
        `PortableForm` already names this exact case `PathDisposition::DriveRelative`, so
        `RunCached` reads that tally rather than walking the paths again — two spellings of
        one classification are two places for it to drift, which is the defect
        `DispositionTable` exists to prevent. `Cc::IsDriveRelativeUnderNoRoot` remains only for
        the input the tally cannot answer for: an argument whose path the compiler never
        reported. Both reach `RootToken`, so "did a root place this" has one definition and not
        two. Note what #105 did and did not do here: it gave the drop its own NAME, which made
        the fault legible and this fix expressible — naming a drop is not covering it, and on
        its own the path still reached the key filter as a drop.
      - **No schema tag moved for the refusal itself**, and that is what the command-line ask
        bought: nothing about the key's construction, the value's framing or its canonicalization
        changed, and an affected compile simply stops consulting the cache. Closing the *whole*
        thing by bumping instead would have re-keyed every entry on every platform for a
        clang-cl-only exposure. A bump was still needed afterwards — for the direct-mode residual
        below, and not for the refusal. See the `v5` bullet.
      - **The regression test asserts the CONJUNCTION, because the defect was never inside a
        filter.** A table of the shapes a driver can report, each declaring what covers it —
        keyed, guarded, the toolchain stamp, or refused — and there is deliberately no
        `Nothing` enumerator, because a row nothing covers is the defect rather than a fifth
        kind of row. `ToolchainStamp` is a *claim* about content, true of a toolchain header at
        a fixed address and false of a path nothing can place. Verified by neutering the
        predicate and watching those two rows, and only them, change answer.
      - **`v5` closes the direct-mode residual the refusal could not reach (issue #111), and
        both tags move.** The authoritative ask runs after `TryDirectMode`, and the command-line
        ask can only see a path an *argument* carries — so a drive-relative header arriving some
        other way (an `#include "C:foo/x.h"` written out in the source, a fused flag spelling
        `PathValueFlags()` does not know) kept direct-hitting a manifest an older launcher
        recorded. That manifest was invisible to its own key for exactly the reason the `v4`
        bullet gives for its generation: the refusal changes neither `canonicalSource` nor the
        args, so the entry keeps its key and keeps being found, and `ValidateManifest` still
        validates it because the offending path was dropped from the manifest too and there is
        nothing left to fail on. Re-keying is the only thing that retires it. Four consequences:
        - **The bump had to land at or after the refusal, never before it.** A `v5` manifest
          written by a launcher without the refusal carries the identical defect, and there
          would be no tag left to retire *that* — the one retirement event would have been spent
          on the wrong generation. With the refusal underneath, such a compile is never cached
          at all, so the `v5` population is clean by construction.
        - **`objkey` moved too, and nothing forced it.** The manifest half is the whole of the
          fix; the object key's construction, framing and canonicalization are all unmoved. It
          is taken because the standing argument at the `objkey` tag decides it — two tags
          sitting numerically apart is a state nobody should have to reason about later — and
          the cold rebuild it costs is one the manifest bump has already committed every user to
          paying, so it rides in that invalidation event rather than ever costing a second.
        - **The issue recommended the opposite and was overruled at the tag, not silently.** It
          argued for moving `manifest` alone, since re-keying objects invalidates every entry on
          every platform for a clang-cl-only exposure. That asymmetry is real, but it is an
          argument about one invalidation event weighed once, while the tags-apart hazard is
          carried by every later reader and does not expire.
        - **A golden vector does not pin a bump; a retired-generation table does.** Reverting
          the tag and re-pasting the vector is one edit two hunks apart and leaves the suite
          green. `Test::RetiredGeneration` in `KeyDigestTestSupport.hpp` is a row per generation
          each key space has retired, and the live key must equal none of them, so restoring an
          old tag reproduces a retired digest and fails whatever the golden says. Rows reach
          back only to `v3`, because issue #63 moved the digest itself and `v2` and earlier are
          unreachable by construction rather than by tag.
    - One diagnostic consequence of the drop that used to happen: the launcher's
      `dependency set: 0 of M reported path(s) keyed` line was reachable for a second reason,
      so that fingerprint stopped identifying the #66 short-name mismatch on its own. The two
      were then told apart only by whether the *root* was short-name spelled, which requires
      knowing to ask — the opposite of what a fingerprint is for. Closed by the counter
      carrying its *reasons* now; see the `PathDisposition` bullet below.
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
    non-zero and every one of them counted `toolchain` — the probe reported paths and every
    one was filtered out, which is a different fault from `0 of 0` (a driver that reports
    nothing on the preprocess line) and, since the reasons were added, from a set that was
    `drive-relative` or `unanchored` instead.
    `run-launcher-e2e.ps1` therefore puts its scratch trees beside the **build tree**
    rather than under `%TEMP%`; it does not try to expand a short name, because nothing
    dependably does — `Resolve-Path`, `Get-Item` and `[IO.Path]::GetFullPath` all preserve
    it, and `Scripting.FileSystemObject` was tried and echoed it back unchanged.
  - **A drop that cannot say WHY it dropped is a counter, not a fingerprint.** `0 of M` was
    the fingerprint of the bullet above, and it was worth having precisely because #66 is
    silent from every other direction. Issue #65 then gave the same line a second cause — a
    drive-relative path under no root also drops — after which it identified neither: the two
    were tellable apart only by whether the *root* was short-name spelled, which requires
    knowing to ask, the opposite of what a fingerprint is for. `Cc::PathDisposition` names
    each outcome of the filter and the note reports the ones that happened, so
    `0 of 12 keyed (12 toolchain)` and `0 of 12 keyed (9 toolchain, 3 drive-relative)` are
    different lines (issue #105). It changes no key input and moves no schema tag; the key
    was right either way. Consequences that are each load-bearing:
    - **A table, sized from a `Last` sentinel and guarded by a `consteval` coverage check**,
      the shape `WorkerProtocol`'s `RefusalTable` and `Metrics/MetricsCatalog` already carry:
      a disposition that can be counted but not named is a drop that renders as nothing, which
      is the whole defect. The *sizing* is the half that has to be right, and `RefusalTable`'s
      spelling is the one not to copy — a length asserted against the final enumerator **by
      name** accepts a table one row short the moment an enumerator is appended after it, and
      the tally is indexed by the enumerator, so the new disposition writes one past the end.
      Derived from `Last`, the missing row value-initializes to `{ Keyed, "" }` at a non-zero
      index and the coverage check rejects it. Verified by adding an enumerator without a row
      and watching the build stop.
    - **`IsToolchainHeader` was deliberately NOT split**, though the issue's illustration
      implies it. It is the one rule three callers must agree on, and separating "a marker
      matched" from "under neither root" does not isolate #66 anyway: under a short-name root
      a genuine `/usr/include` header and a project header both land outside the roots, so
      the split adds a column without adding a fingerprint. What separates the faults is
      anchor-shaped (`Unanchored`, `DriveRelative`) against content-shaped (`Toolchain`).
      The non-split is not free, though, and the bullet below is its price paid in full
      rather than recorded as a residual.
    - **A drive-relative path is reported as drive-relative only when it is also under no
      root, and the second half is what makes it precise.** Under no root the anchor is the
      only fact that says what to change, and calling it `toolchain` is the very word a
      short-name root produces — reporting it would re-collapse the two. Under a
      drive-relative *root* the path canonicalizes, so a marker match there is ordinary
      vendored content and `toolchain` is true; reporting THAT as drive-relative would put
      the loudest reading of the new vocabulary on a healthy build, which is the same defect
      from the other side. `IsToolchainHeader` cannot separate them — it tests its markers
      before any root, deliberately — so the root question is asked again in that one
      branch, and only for a drive-relative path. That is the price of not splitting the
      classifier, and it is one extra root test on a shape no ordinary build produces: the
      toolchain drop, 476 of a real translation unit's 635 paths, still costs exactly one.
    - **`Uncanonical` is a third root-spelling fault that had no name at all.**
      `/x/build-other/a.h` under a `/x/build` root is a root off by a suffix, and while it
      counted as "toolchain" it was indistinguishable from an ordinary system header — though
      it is the one of the three an operator repairs by editing a root. It arrived here by
      accident at first: `IsToolchainHeader`'s prefix match was character-wise and
      `Canonicalize`'s segment-wise, so such a path fell out of the gap between them. That
      gap was itself the defect (#562, above) and is closed; the fault survived it by being
      asked for directly, as `Cc::ClassifyAgainstRoots`'s third outcome. A state produced by
      an inconsistency and a
      state produced by a predicate look identical from the outside and are not the same
      thing to reason about — the first is only ever as reliable as the bug.
    - **The tally counts reported OCCURRENCES**, so it sums to `M` while the keyed set is
      what survived sort-and-deduplicate. `/showIncludes` repeats a header once per inclusion
      site, so the two genuinely differ on every real translation unit and the note's numbers
      do **not** add up there — which the operator documentation says out loud, because a
      reader who assumes they should goes hunting for paths that vanished. `Reported()` is
      derived from the tally rather than carried beside it, because two counters for one fact
      are two counters that drift.
    - **The renderer lives in `DependencyProbe.cpp` rather than `main.cpp`**, the lesson
      `CacheProtocol.cpp` and `RootReconciler.cpp` are each recorded as having been extracted
      for, and each collapse was reintroduced to check the cases can see it: folding
      `DriveRelative` back into `Toolchain` fails exactly one case with `"2 toolchain"`,
      folding `Uncanonical` in fails exactly one other, and labelling a vendored tree under a
      drive-relative root by its anchor fails exactly one third with `"2 drive-relative"`.
  - **A manifest refusal names the path it refused over, and the same rule reaches the
    manifest a release after it reached the key.** `BuildManifest` returns
    `ManifestFailure` — a `ManifestFault` and the offending path — and every refusal
    inside it fills in both. The reasoning is `PathDisposition`'s exactly, arriving late
    only because the manifest side looked less silent than it was: a refusal here costs a
    translation unit direct mode **permanently** while the compile succeeds, the object
    still caches under the ordinary preprocessed key, and nothing else in the log mentions
    it — so "why does this TU never cache" is a whole investigation and the answer is one
    path (issue #68). Three things that are each load-bearing:
    - **The commonest refusal was not `BuildManifest`'s at all.** `RecordManifest` asks
      `CanonicalSourceToken` first and returned in **complete** silence when it declined,
      before `BuildManifest` ran — so a TU under neither root, which is an ordinary
      `add_subdirectory(../shared shared)` layout, produced no line even under
      `FASTCACHE_VERBOSE`. Giving `BuildManifest`'s error a path while leaving that guard
      alone would have fixed the case an operator almost never reaches. Issue #57 closed
      pointing straight at it.
    - **Five faults, not the two the refusal paths happen to have.** `Unanchored` is a
      working directory, `OutsideRoots` is a layout, `ToolchainLike` is a TU nobody
      expected to compile from, `Uncanonical` is a root spelled almost
      right, `Unreadable` is a file — each names a different thing to go and repair, which
      is what a fingerprint is for. `ToolchainLike` exists because `IsToolchainHeader`
      tests its markers before any root, so "toolchain" and "under no root" arrive as one
      answer -- the root question is asked again in that one branch, the same correction
      `PathDisposition::DriveRelative` makes. `Unreadable` in particular had been sharing
      `DirectError::Malformed` with "these bytes off the wire are corrupt", so the two
      printed identically; `DirectError` is now decode-only and the two vocabularies do not
      meet. The table is an `EnumTable` checked by `RowsInEnumeratorOrder`, the same
      declaration `DispositionTable` uses — this landed while the branch was in review and
      is the seventh table to reach it, which is the point of there being one (issue #112).
      `DescribeManifestFailure` lives in `DirectManifest.cpp` for the same reason
      `DescribeDropped` does — `main.cpp` is in no test target — and keeps its own bounds
      check anyway, because `Last` is the extent and so still indexes one past the end.
    - **`TryDirectMode`'s identical guard stays silent, deliberately.** It is the same
      derivation from the same inputs as the recording side, so it refuses exactly when
      `RecordManifest` does and for exactly the same reason: a note there prints every such
      fact twice per translation unit. And the whole diagnostic stays verbose-gated, which
      is the #66 reversal below holding rather than being re-litigated — the issue asked for
      it ungated, and a source outside both roots is still an ordinary layout rather than a
      fault. What changed is that the gate now has something worth reading behind it.
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
  mistake `PathCanon::CanonError` *was* — an error type no code path could produce,
  deleted by issues #59/#69 along with the wire status `0x06` it fed. Domain separation between the
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

## A path a compiler wrote is not this process's text

- **`RootReconciler::Path` is where a tool-emitted path becomes text, because it is
  the funnel every one of them already passes through** (`All` and `Region` both
  come here) and because nothing downstream can do it. The launcher's roots come
  from `argv`, which on a Windows host declaring the UTF-8 code page is UTF-8, while
  `cl.exe` writes `/showIncludes` in the CONSOLE OUTPUT code page — `C3 BC` for
  U+00FC under CP 65001, `81` under CP 850, measured. Untranslated, such a path
  prefix-matches no root, so a project header classifies as toolchain content:
  dropped from the key, stat'ed by nothing, and a header edited inside it serves the
  stored object under a zero exit code.

- **Two candidate encodings, in this order and no other**: UTF-8 if the bytes are
  UTF-8 (clang and gcc always, `cl` under a UTF-8 console), else the one code page
  the host names. Never a ladder — a single-byte page decodes nearly everything, and
  Windows even maps CP-1252's five unassigned bytes to the matching C1 controls, so
  trying pages until one "works" produces a path naming a file that does not exist
  rather than a refusal.

- **The DECODED form is what comes back, not merely what the decision is made on.**
  Returning the raw bytes for a path the reconciler translates no further was tried
  and is wrong twice: the key and the manifest prefix-match against roots that are
  UTF-8, and `Region`'s result goes into a value SHARED between machines which the
  daemon canonicalizes against those same roots. One encoding in a stored value is
  the rule #141 settled for the wire.

- **What cannot be read is counted, and the compile is not cached.** Same hazard as
  `PathDisposition::DriveRelative` and the same answer, asked in all three places
  the count can rise: the key path, the manifest record, and again after the stored
  regions are built — `Region` walks a wider set of path spans than the dependency
  list, so the earlier ask cannot have covered them. The refusal names the recovery
  (`chcp 65001`).

- **Whether bytes must be text at all is a property of the HOST**, carried as
  `NarrowTextPolicy` and injected rather than probed — so `PortableForm` stays pure
  and both hosts' behaviour is testable on either. On POSIX nothing is transcoded
  and a legacy filename is a perfectly good filename; refusing one there would break
  a build that works today.

- **A launcher never fails a build the compiler would have completed**, and
  `std::filesystem::path`'s narrow constructor throws on a UTF-8-code-page host for
  bytes that are not. Every construction from foreign text goes through
  `Platform::PathFromNarrowText`; `MissingReplayedDependency` is the one that
  matters most, because its bytes arrive over the NETWORK — there, unreadable counts
  as *missing*, which discards the hit and recompiles rather than serving one it
  could not check.

## A compiler's WORDS are localized too, and `VSLANG` may only touch the probe

- **`VSLANG=1033` goes on the probe spawn and never on the real compile.** The
  launcher matches `cl`'s `/showIncludes` notes against the literal English
  `IncludeNoteMarker`, so a Visual Studio carrying a language pack answered every
  reader of them in a language the launcher does not read: the cache key lost its
  dependency set and no manifest was ever recorded, on every translation unit,
  forever, behind a cache that still hits on preprocessed keys and therefore looks
  healthy ([#692](https://github.com/LASTRADA-Software/fastcached/issues/692)). The
  fix is to ask the compiler to speak English — but only where **the output is
  parsed and dropped**, which is `Preprocess`. The real compile's streams are read
  by a human, and forcing English there trades a silent performance loss for
  silently anglicizing every warning and error a developer sees. `IProcessRunner`
  therefore gets a separate entry point rather than a flag, so the distinction is
  made at the call site where it can be seen. The next person here will be tempted
  by the real compile; that is the whole reason this is written down.

- **The added environment is layered on the inherited one, never substituted for
  it.** A spawn that replaced it loses `INCLUDE`, and a compiler that cannot find
  `cstddef` reports a confident syntax error rather than a missing environment. A
  test for this must assert something a replacement cannot survive — a real `PATH`'s
  directory SEPARATOR, not its length: `cmd.exe` echoes an unset variable as the
  literal `%PATH%`, so a length check passes against a runner that dropped the whole
  inherited block, which is how the first version of that test certified the very
  bug it was written to catch.

- **`VSLANG` is best effort, so the residue is NAMED rather than reported as
  nothing.** `cl` ignores it when the requested language pack is not installed, so
  no caller may conclude from a successful spawn that the child obeyed — the check
  is on the OUTPUT. When the probe still extracts nothing,
  `CarriesUnreadableIncludeNotes` separates "this build asked for no dependencies"
  from "this machine's compiler answered in a language I do not read", because one
  empty set for both prints a sentence that is true about what the launcher observed
  and false about the world. That predicate feeds a MESSAGE and nothing else, which
  is exactly why it may be loose where `IncludeNotePath` may not: the recognition
  rule is shared with `SplitIncludeNotes`, which runs over a stream that also
  carries preprocessed SOURCE, so loosening THAT deletes a source line out of the
  bytes the key hashes. Its separator table carries the full-width colon `U+FF1A` as
  well as `": "`, because MSVC's CJK catalogues use it and a fixture typed by hand
  will not discover that on its own.

- **The reading side is fixed and the WRITING side is not.** `RenderShowIncludes`
  emits the same English marker for Ninja, which matches `msvc_deps_prefix` — taken
  by CMake from the actual localized compiler — so a localized build records no
  dependencies for a dispatched compile and under-rebuilds. `VSLANG` cannot fix
  that: you cannot produce a localized prefix by forcing English. See
  [#700](https://github.com/LASTRADA-Software/fastcached/issues/700), which also
  carries the stored-region half.

## An object file is not a byte string, and `FASTCACHE_VERIFY` is where that bites

`FASTCACHE_VERIFY` compiles a sampled hit again and compares the two objects. It
compared them with `memcmp`, and on Windows that **can never succeed**: every
MSVC-family driver stamps the wall clock into the COFF header, and a cached object
was produced earlier than the fresh compile it is checked against *by construction*.
So the one instrument that turns a wrong object from invisible into loud reported a
wrong object on **every** hit, on the platform where
[#368](https://github.com/LASTRADA-Software/fastcached/issues/368) — a wrong object
served under a correct key — was actually observed
([#493](https://github.com/LASTRADA-Software/fastcached/issues/493)).

The fact was already written down three times, in
[`distributed-compilation.md`](distributed-compilation.md), in
`scripts/dist-compile-e2e.ps1` and in `docs/tools/fastcache-compile-node.md` — and
none of those is a file anyone editing `HitVerification.cpp` opens. **That is why it
is repeated here**, in the launcher's own rule file, rather than linked.

What was measured, on this tree, rather than reasoned from "ELF is reproducible":

| two compiles of one TU differing in | `cl` 14.51 | `clang-cl` | clang 20.1 / GCC 14.2 |
|---|---|---|---|
| nothing — same object path, 2 s and 300 s apart | the 4-byte `TimeDateStamp`, and **nothing else** | the same | **identical**, with and without `-g` |
| the object's directory | + `.debug$S`, `.chks64` | nothing more | identical |
| the source's directory | + `.debug$S`, `.chks64` | nothing more | identical |

Five rules follow, and the second is the one an obvious fix gets wrong:

- **ELF keeps the byte comparison.** This is a platform gap, not a total failure.
  Normalising a field there would overlook a real difference to fix a problem that
  platform does not have.
- **`.debug$S` and `.chks64` are volatile with respect to the PATH, not to time —
  so they are NOT excused.** The verifier copies the served object aside and
  compiles to the *same* output path, so a hit this machine produced differs in the
  clock alone and there is nothing to overlook. Excusing them anyway would buy
  nothing and would go silent on
  [#489](https://github.com/LASTRADA-Software/fastcached/issues/489): a hit served
  from another checkout, whose entire difference lives in `.debug$S`. That is the
  case an operator turns verification on to catch, and excusing it is #493 cured by
  no longer looking — which passes every test written for #493 itself.
- **Parsing never grants an excuse.** The one normalised region is a single header
  field whose offset is read from a two-row layout table (`/bigobj` is an allowed
  argument, so such a build is cacheable and its clock is at offset 8). The section
  walk exists only to say WHERE a difference was, so a parser defect can make a
  message vague and cannot make a wrong object pass.
- **Which image is unreadable decides the answer, and it is asymmetric.** A FRESH
  object this build cannot lay out is this build's blind spot — `HitVerdict::Unsupported`,
  refusing by name, because an operator who reads "cannot verify" as "your cache is
  broken" switches the instrument off and the guarded class goes invisible again. A
  SERVED object that will not parse while the fresh one does is `Mismatched`: that is
  a truncated transfer, and a real finding.
- **Not `/Brepro`**, for the reason `dist-compile-e2e.ps1` already gives about
  itself: it would make the comparison true about a command line no build uses.

And the acceptance shape, because a verifier is the one component whose *failure to
fire* is invisible: a correct hit verifying clean proves nothing on its own — a
verifier that stops crying wolf by no longer looking passes that perfectly. Every
change here also shows a deliberately corrupted object still answering `Mismatched`,
and an object from another checkout still answering `Mismatched`, on **real compiler
output** rather than only on synthetic bytes.

## A compiler records WHERE it was built, and the key cannot see that

The key is portable across checkouts by design — that is what the launcher is
for. A compiler with debug info on is not: it writes the compile's working
directory into the object, and the working directory appears on no command line,
so no amount of relativizing reaches it. A hit therefore replays an object naming
the producing checkout, a debugger looks for sources in a tree this machine does
not have, and nothing fails. Issue #203; issue #489 is the same defect
approached from the key end.

**Measured, one TU byte-identical in two roots differing only in name and by the
same character count, against a same-root baseline of 0 differing bytes:**

| driver | debug info off | debug info on | remedy the driver offers |
| --- | --- | --- | --- |
| `g++` (ELF) | identical | **differs, 143 B** — `DW_AT_comp_dir` | `-fdebug-prefix-map` → identical |
| `clang++` (ELF) | identical | **differs, 6 B** | `-fdebug-prefix-map` → identical |
| `clang-cl` (COFF) | identical | **differs, 23 B** | `-ffile-prefix-map` → **still 23 B** |
| `cl` (COFF) | **differs, 11 B** | **differs, 28–31 B** | none exists |

The baseline is not a formality. Write each compile to a *different* `/Fo` name
and every command line differs, `/Z7` embeds the command line, and the baseline
stops being one — the same confound that cost #493 a re-run.

- **It is not `/Z7`, and the launcher does not create the precondition.** `cl` with
  no debug flag at all already differs cross-root: `.debug$S` carries an
  `S_OBJNAME` record holding the resolved **absolute** object path, and `.chks64`
  a hash derived from it. `CompileCache.cmake`'s `/Zi`→`/Z7` rewrite widens 11
  bytes to 28; it does not open the hole, and turning debug info off does not
  close it.
- **A key-side fix cannot work by hardening argument handling.** With a fully
  relative command line — cwd at the checkout root, source and output both named
  relatively, so the two command lines are **byte-identical** — the objects still
  differ. What is embedded is the working directory and the path the driver
  resolves `/Fo` against. Neither is an argument. So #489's shape needs the
  compile *location* as a key input in its own right, which costs cross-checkout
  sharing on every `cl` compile.
- **The divergence is debug and identity records only.** `.text`, `.data`,
  `.xdata`, `.pdata`, the relocations and the symbol table are byte-identical in
  every differing pair. A wrong *artefact* under a correct key, not wrong *code* —
  which is why neither ticket can be #368's mechanism.
- **`-fdebug-prefix-map`, never `-ffile-prefix-map`.** The wider flag implies
  `-fmacro-prefix-map` and rewrites `__FILE__`, and that buys nothing here.
  `__FILE__` is **self-protecting**: the preprocessor expands it and `ComputeKey`
  hashes preprocessed output raw, so whenever the expansion is checkout-dependent
  it is checkout-dependent in the hashed text too, and whenever the hashed text
  agrees the expansion agrees. Measured through `/EP` alone — absolute source
  spelling differs, relative agrees, relative plus `/FC` differs. There is no
  arrangement in which two checkouts share a key while their `__FILE__` strings
  differ, so changing program-visible strings would fix a defect that cannot
  occur.
- **The flag names the producing checkout by construction, so the KEY has to
  relativize it.** Measured before the row existed: `RelativizeArgs` returned
  `-fdebug-prefix-map=/home/ci/checkout-aaa=/fastcache/src` byte-for-byte, so
  appending the flag alone would have produced correct objects and turned every
  cross-checkout hit into a **miss** — silently, since a miss is what a cold cache
  looks like too. `PathValueRole::PrefixMap` splits the value at the last `=`,
  the way GNU does, and relativizes only the head.
- **The replacement half is deliberately left literal.** Two machines mapping to
  different replacements write different objects, so they must compute different
  keys. That is what makes "the mapping must be identical on every machine sharing
  the cache" a property of the key rather than a line of advice — a disagreeing
  machine misses instead of mis-serving.
- **The rules are computed by one function and checked as a computation.**
  `_fc_debug_prefix_map_rules` in `cmake/portable/CompileCache.cmake`, driven by
  `ctest -R debug-prefix-map-rules` over a table of layouts. It was wrong twice
  before it was ever run, in ways invisible in the layout a developer has:
  `file(RELATIVE_PATH)` answers with a **trailing separator**, so the rewritten
  path read `../../..//src/tu.cpp` and stopped matching the spelling the build
  system passes for the same file; and for an out-of-tree build it answers
  `../../mnt/d/…/checkout` — *relative*, and carrying the entire root inside it,
  which would have replaced the checkout's path with the checkout's path and read
  as working. **Relative does not imply checkout-independent**; the test asserts
  that property separately from the expected values, because a table can be edited
  to agree with wrong code.
- **The build tree is mapped LAST, and no object comparison can check that.** GCC
  and Clang honour the **last** matching rule — measured off `DW_AT_comp_dir` on
  gcc 14 and clang 20, not inferred, after this rulebook and the code both said
  the opposite for three commits. Build-first leaves `comp_dir` as
  `../../../out/build/<name>`; source-first gives `.`. Both are
  checkout-independent, so two checkouts still produce byte-identical objects and
  every e2e reads green either way. What the wrong order costs is narrower and
  sharper: two machines whose build trees sit at the same depth under different
  NAMES (`out/build/gcc-release` against `out/build/ci-release`) relativize to one
  key — both flags tokenize to the same canonical text — while their objects
  differ. That is the #203 symptom reappearing inside the fix for it, and the only
  instrument that sees it is reading `comp_dir` directly.
- **Only the DEBUG spelling is in the table.** `-fmacro-prefix-map` and
  `-ffile-prefix-map` rewrite `__FILE__`, which lands in the preprocessed text the
  key hashes — measured on both drivers with the source named absolutely, where
  `-fdebug-prefix-map` leaves that text byte-identical and the other two do not.
  A row for either would make the flag dropped from the preprocess line (every
  role but `IncludeDir` is), so the key would hash text the real compile does not
  produce, and a dispatched compile would bake the UNMAPPED `__FILE__` into an
  object stored under the same key a locally mapped one uses. Unrecognised, they
  reach the key verbatim and two checkouts miss — the safe direction, and the
  behaviour that was already there.
- **A root with a SPACE is not mapped, and that is a refusal.** The rules are
  spliced into `CMAKE_<LANG>_FLAGS`, which is space-separated, so a rule naming
  `/Users/john doe/proj` reaches the driver as two arguments and every compile
  dies with `invalid argument '/Users/john' to '-fdebug-prefix-map'` — measured.
  `check_compiler_flag` cannot catch it, because it probes a synthetic `/a=/b`.
- **The two drivers disagree about where `<from>=<to>` splits, and neither answer
  is right for both.** Measured with a directory named `a=b`: gcc cuts at the last
  separator, clang at the first. The launcher follows gcc. Reachable only when a
  mapped root itself contains a separator, and it costs a MISS rather than a
  mis-serve — the head the launcher isolates lies under no root either way.
- **A dispatched compile carries the DIRECTORY AND ITS REPLACEMENT, never the rule**
  ([#506](https://github.com/LASTRADA-Software/fastcached/issues/506)). A worker needs
  a rule whose left-hand side is a path on the WORKER, which the client has never
  seen — so `RemoteCompileArgs` dropping the flag is right and stays, and
  `CompileRequest::compileDir` plus `compileDirReplacement` travel instead. That makes
  the replacement a contract both ends share rather than a convention each machine
  picks, which is the decision this was open on.
  - **BOTH candidate directories are mapped, because WHICH one the object records is
    the DRIVER's answer.** gcc's `-fworking-directory` is implicit under `-g` and
    emits a line marker naming the preprocessing directory, which the worker's compile
    adopts; clang emits none. Measured on gcc 14.2.0 and clang 20.1.2, reading
    `DW_AT_comp_dir` off the worker's object: `g++ -E -g` records the CLIENT's
    directory, while `g++ -E` and `clang++ -E`/`-E -g` record the WORKER's. Mapping
    only the worker's own fixes clang and leaves gcc recording the client's UNMAPPED
    path — which no object comparison can see. The first implementation of this ticket
    did exactly that, passed every unit test, and was caught only by reading
    `comp_dir` end to end. Removing the mapping fails both drivers naming DIFFERENT
    directories, which is the asymmetry itself.
  - **A model of a driver that is MORE PERMISSIVE than the driver produces WRONG
    AGREEMENT, which is worse than the disagreement it replaces.** This is the general
    rule and the two below are its instances; both were arrived at independently while
    fixing the same ticket, which is why it is written as a pattern rather than as two
    fixes. A model that is too NARROW costs a miss — the mapping does not travel, the
    object keeps an absolute path, and the two ends disagree visibly. A model that is
    too WIDE tells a worker to apply a mapping the local compile did not, so both
    objects are confidently mapped, to different things, under one key. The asymmetry
    is the whole point: err narrow.
      - Matching the rule's root against the working directory by FILESYSTEM IDENTITY
        (canonicalize both, compare) rather than by the byte prefix the driver
        implements. Measured: `-fdebug-prefix-map=/tmp/l506f/build=.` against a cwd
        reached as `/tmp/l506f-link/build` leaves `DW_AT_comp_dir` UNMAPPED, so
        identity-matching would have mapped where the driver does not.
      - Deciding whether `PWD` is usable with `std::filesystem::path::is_absolute()`
        rather than a leading `/`. Same test on POSIX, strictly wider on a Windows
        layout, where it admits `D:/work` and `\\host\share` and NEITHER Windows
        driver consults `PWD` for either — libiberty's `getpwd()` gates on `*p == '/'`,
        and LLVM does the `PWD` dance only in `Unix/Path.inc`. A MinGW client would
        have predicted a spelling its own compiler never uses. Read, not measured: no
        Windows GNU-layout driver was available, and the table below is Linux.
  - **And that directory is `$PWD`, not `getcwd(3)`** — asked through
    `CompilerWorkingDirectory`, on both sides. Both drivers report `DW_AT_comp_dir`, and
    byte-compare every `<from>` against, the `PWD` variable when it is ABSOLUTE and names
    the SAME directory as `.`; `getcwd(3)` only when it is not. Measured, gcc 14 and
    clang 20 identical, cwd `/tmp/l506f/build` reached as `/tmp/l506f-link/build`: the
    link spelling is recorded verbatim, while unset, a different real directory, a
    nonexistent path and a relative path each fall back to `/tmp/l506f/build`. So it is a
    `stat` comparison, not a spelling one — `std::filesystem::equivalent`. And the
    discriminating case: a rule written with the RESOLVED spelling matches NOTHING, so
    this cannot be answered by canonicalizing both sides and comparing identities — the
    first instance of *err narrow* above. `current_path()` is `getcwd(3)`, so the first implementation matched nothing on any
    build reached through a link, sent no pair, and left the dispatched object unmapped —
    this ticket surviving its own fix, with every counter normal. macOS meets it because
    `/var` is a symlink and `$TMPDIR` is under one; Linux `/tmp` is not, which is why it
    reached master green. Case 13 therefore runs at TWO spellings, and each compiles its
    own source or the second is served the first's cached object and never dispatches.
  - **The residue identifies the DRIVER, not the origin.** A dispatched object recording
    the CLIENT's directory does not mean it came from no worker: that is gcc's
    `-fworking-directory` marker being adopted, measured with `DISPATCHED to` in the same
    log. clang leaves the WORKER's showing for the identical fault. So macOS and Linux
    print different directories for one defect, which is a SECOND reason mapping a single
    candidate could never have held — either one breaks it alone. What discriminates is
    the launcher's spelling against its resolved form, never which directory appeared.
  - **It is the node's WORKING directory, not its scratch directory** — the ticket
    said scratch and was wrong, so "map the scratch dir" fixes nothing.
    `CompileJobRunner::Run` spawns with absolute source and object paths and never
    chdirs, and `IProcessRunner` has no directory parameter, so the worker-side
    candidate is whatever directory the node service was started in.
  - **Empty means map nothing, and that is the whole argument against a worker-side
    constant.** A worker that mapped anyway would hand a build that asked for
    nothing an object naming a directory neither machine has — the same asymmetry,
    pointing the other way. It is a test, not a branch.
  - **It cannot ride in `args`.** `IsAcceptableJobArgument` refuses any argument
    body carrying a path separator, deliberately, and both halves are full of them.
    Weakening that rule to carry a debug token would be paying a security-shaped
    price for a debug-info fix.
  - **The DIRECTORY says whether a mapping is in force, never the replacement.** An
    empty replacement is legal — `-fdebug-prefix-map=<builddir>=` maps a root to
    nothing and is a standard reproducible-build spelling, and refusing it as "half a
    pair" cost such a build distribution entirely. A replacement with no directory is
    the one malformed half, and is refused: it would map everything.
  - **The worker's OWN rule is dropped when it would also match the client's
    directory**, and this is the sharpest edge in the change. A prefix-map rule appends
    the unmatched tail, so a worker directory of `/` rewrites `/home/ci/build` to
    `.home/ci/build` and every system header to `.usr/include/...`. Measured on gcc
    14.2.0 with both rules and the worker's last, `DW_AT_comp_dir` came back
    `.tmp/…/client` — a WRONG object under a correct key, strictly worse than what this
    ticket set out to fix, and invisible to an e2e that runs the node from the fixture's
    own directory. Dropped rather than refused: the client's rule still lands, so gcc is
    fully mapped and only clang on such a node keeps the pre-#506 state, where refusing
    would cost every dispatched compile on every node installed from the shipped unit.
  - **`/` was the PRODUCTION value, and the repair was packaging.** The shipped
    `fastcache-compile-node.service` set no `WorkingDirectory=`, so systemd started the
    worker in `/` and the case above was not a corner but the ordinary Linux install
    ([#674](https://github.com/LASTRADA-Software/fastcached/issues/674)). It now names
    one, and names one the unit itself CREATES — `RuntimeDirectory=fastcache-node`,
    `WorkingDirectory=/run/fastcache-node` — because a `WorkingDirectory=` pointing at a
    path nothing creates fails the service at every boot under `ProtectSystem=strict`,
    which is a worse outcome than the defect. `/run` rather than `/tmp`: a worker
    directory that CONTAINS a client's build directory drops the rule exactly as `/`
    does, and CI builds run in `/tmp`.
    The drop above STAYS, because a unit is not the only route — `PosixDaemonHost`
    calls `chdir("/")`, so `--daemon` still lands there
    ([#784](https://github.com/LASTRADA-Software/fastcached/issues/784)), as does any
    hand-written unit. And the guard is a SCAN of the shipped file
    (`ctest -R node-working-directory`, with its own selftest): running the real unit
    needs root and a live systemd, so it is reachable only from the packaging job —
    which is not a required context and therefore reports to nobody (#684) — and no
    fixture substitutes, since every one of them starts the node from the FIXTURE's
    directory. That is precisely how #674 reached master with a dispatch e2e green.
  - **`:` is a path character here.** A Windows absolute path begins `C:\`, and a
    GNU-layout driver on Windows — mingw, or plain clang — is an ordinary client.
    Leaving `:` out of the safe set refused every such client's own directory before
    the driver family was even consulted, so the refusal named the wrong end of the
    fleet.
  - **A worker that cannot honour it REFUSES.** Either directory containing an `=`
    (gcc splits at the last, clang at the first — measured: `/tmp/l506b/eq=sign`
    mapped to `.` gives `.` on gcc 14 and `sign=.=sign` on clang 20, a WRONG
    compilation directory rather than a missing one), or a driver with no such flag.
    Silently skipping the rules returns an object whose compilation directory
    disagrees with a locally built one under the same key, which is this ticket.
  - **The client's model of the flag is the DRIVER's, and it was measured**: the
    last matching rule wins, a longer directory keeps its tail, and the match is a
    BYTE prefix — `/tmp/work=X` against `/tmp/worker` gives `Xer` on both drivers.
    Modelling it at component boundaries reads better and predicts a replacement
    neither compiler writes.
  - **Both halves are covered by `CompileCorrelation`**, on that header's own rule:
    the client knows them before sending and the runner observes them, because they
    become the two halves of the arguments on the line that is spawned. Uncovered, a
    crossed reply is a wrong `DW_AT_comp_dir` under a correct key — this ticket, one
    layer down.
  - The `#line` markers still carry the client's paths, so a dispatched object's
    line table names the producing checkout's headers. On gcc that already matches a
    local compile's, because gcc takes the unit's `DW_AT_name` from the marker — on
    **clang it comes from the INPUT FILE PATH**, so a dispatched object recorded the
    worker's `<scratch>/job-N/<name>` until
    [#660](https://github.com/LASTRADA-Software/fastcached/issues/660).
  - **`DW_AT_name` is `comp_dir`'s sibling and needed its own rule** (#660). Two
    consequences, and the second is the one that matters: the debug path named a
    directory on no developer's machine, AND the job counter advances, so two
    dispatches of ONE translation unit to ONE worker produced byte-differing objects
    under one cache key — the exact property a compile cache exists to deny.
    - **The client's spelling TRAVELS, as a REPLACEMENT, never as a path the worker
      opens.** The invariant this was stopped on — *"Nothing the client sent decides
      where a byte lands"*, sitting directly above `create_directories` — is a
      FILESYSTEM guarantee, and `DW_AT_name` is a recorded string. They are not in
      tension, and the same file was already building rules out of client-sent fields
      for `compileDir`. `SafeSourceName` still decides the file the worker creates, and
      `CompileJob_test` asserts that on the paths the worker actually created — captured
      at spawn, because `ScratchGuard` removes the directory before any later look.
    - **NO wire bump, and this was established before an encoder was written.**
      `DecodeCompilePayload` splits an EXACT arity, so a new field would have broken
      every deployed peer and moved `CurrentVersion`/`MinSupportedVersion` 3→4. It is
      not needed: `sourceName` already travels, the client simply stops truncating it,
      and both directions of a mixed fleet degrade to the previous behaviour — an old
      worker sanitizes what arrives exactly as before, and an old client sends no
      directory, so no rule is built.
    - **The WHOLE path is mapped, not the directory**, which is the narrowest rule that
      works: it matches exactly one path, so it cannot reach `comp_dir` or anything
      else, and it survives `SafeSourceName` having renamed the scratch file.
    - **It goes LAST, and the order is the difference between fixed and not fixed.**
      Measured on clang 22.1.8 with the worker-directory rule also matching the scratch
      path: this rule FIRST gives `./scratch/job-7/tu.cpp` — still broken — and LAST
      gives `../src/tu.cpp` with `comp_dir` still `.`. The first measurement of this
      said the order did not matter and was taken through **ccache**, which sits ahead
      of the real compiler on `PATH` on the machine it was measured on; pin the compiler
      by absolute path or the answer is whatever was cached.
    - **Every way of not being able to build it is NO RULE, never a refusal**, unlike
      the `compileDir` rules beside it. Those honour a mapping the client ASKED for, so
      skipping one silently is #506 itself. Nothing asked for this one, and a source
      file whose name carries a space, or an `=`, or an MSVC worker with no path-map
      switch at all, are ordinary. Skipping an `=` is also the NARROW choice: gcc cuts
      `<from>=<to>` at the last separator and clang at the first, so such a rule records
      a name NEITHER machine has, which is worse than recording the worker's.
    - **One residual, measured**: a client whose source argument is ABSOLUTE and whose
      line carries a matching `-fdebug-prefix-map` records the mapped spelling locally,
      while the dispatched object records the unmapped one, because `RemoteCompileArgs`
      drops the client's rules by design. Deterministic and strictly better than
      `job-N`, and it is [#800](https://github.com/LASTRADA-Software/fastcached/issues/800).
      MSVC keeps its own residual for the same reason `cl` has no row here at all.
  - Read `comp_dir` and `DW_AT_name`, never compare objects: two different-but-checkout-independent
    mappings compare EQUAL, which is how the rule-order defect above first read
    green. `dist-compile-e2e --case suite` case 13 runs the launcher from a
    directory the worker is not in — a case compiled in the worker's own directory
    passes with the fix reverted — and asserts the value rather than only the
    agreement, because two readers that both return nothing agree perfectly. It is
    the only case in that fixture that opens the debug records, and it is what
    caught the worker-directory-only design that every unit test had accepted.
- **`check_<lang>_compiler_flag` is asked only for an ENABLED language.** It is a
  hard `CMake Error` otherwise ("C: needs to be enabled before use"), and this
  module is included from a `project()` that lists CXX first — so the first run
  ended the configure outright, which is the one thing
  `cmake/portable/CompileCache.cmake` may never do.
- **Skipped, rejected and applied are three states and the STATUS line says
  which.** A driver that rejected the flag still caches, still shares, and still
  replays objects naming another checkout; a line reporting only the applied half
  would read the same in all three cases.

## Two servers on one wire are two VERSIONS on one wire

`AGENT.md` requires every server on this wire to canonicalize a stored value's text
regions identically, through the one `CanonicalStoredValue`. That rule was written
about a *moment* — the moment #229 made a compile node a second server and it turned
out to have no copy of the recipe. But a fleet is permanently mid-upgrade
([#173](https://github.com/LASTRADA-Software/fastcached/issues/173)), so the servers
on the wire are at several builds at once and the property has to hold **across
generations or it does not hold at all**
([#483](https://github.com/LASTRADA-Software/fastcached/issues/483)). Two routes
were open to breaking it, and neither needed anybody's install to be stale.

- **The version byte named nothing.** `PathCanon` deliberately carries no version of
  its own and says so, on the reasoning that canonical text travels only inside a
  `CompileValue` and that container carries `CompileValueVersion` — so a change to
  the canonicalization spec is *expressed by bumping that*. Correct, and enforced by
  nothing: the byte was a bare constant referenced by the encoder and the decoder.
  Widen a grammar's span match, change how a root is joined back on, move a byte of
  framing — every behaviour-pinning test in the tree is updated to the new
  expectation, as it must be, and the value ships still calling itself generation 1.
  Two servers then stamp one number on text they rewrote by different rules, which
  is a wrong value under a correct key at fleet scale.
  - **The pin is a conformance digest paired with the byte**, in
    `CompileValue_test.cpp`: a corpus of regions run through `CanonicalStoredValue`
    and `PathCanon::LocalizeRegion`, digested, and matched against the row for the
    live `CompileValueVersion`. A digest alone would not do it — the behaviour and
    the golden are one edit two hunks apart, and moving both leaves the suite green,
    which is `Test::RetiredGeneration`'s argument arriving at the same place from the
    key side. Retired rows stay, and what makes a reverted bump fail is STRUCTURAL:
    generations are unique and ascending and the live byte names the LAST row, so
    putting the byte back names an earlier one however good the digest pasted with it.
    It used to be "the live digest must reproduce none of them", which reads like the
    same guard and is not one: the digest frames `canonical.bytes`, whose leading byte
    IS the version, so two generations are unequal by construction and that check
    could not fail. And a retired digest is a dated record besides — it describes the
    corpus as that generation met it, which the next corpus row invalidates
    ([#583](https://github.com/LASTRADA-Software/fastcached/issues/583)).
  - **Measured, because a guard nobody has watched refuse is not a guard**: an edit to
    `JoinLocalized` — which changes what every consumer replays — is refused by
    **exactly one** case in the whole suite, the pin. Stated that way on purpose: the
    figure was written twice as *2006 of 2007* and *2007 of 2008*, one restatement per
    citation, and both went stale the next time anybody added a test. The count of
    cases that refuse it is the fact; the suite total is the condition it was measured
    under, and it moves. The first cut of the corpus refused it nowhere at all, because
    it exercised bare roots only as a PRODUCER; a rewrite and its inverse are one spec,
    so both ends of every shape reach the digest. That miss generalises past this
    file and is [#547](https://github.com/LASTRADA-Software/fastcached/issues/547):
    **a corpus that varies one side of a transformation and fixes the other cannot
    see a defect on the fixed side, and looks complete while doing it** — everything
    passes, which is exactly what a corpus with a hole looks like.
  - **A grammar is a spec change the digest cannot see on its own**, so the corpus's
    completeness is asked OF THE DECODER rather than restated: all 256 tag bytes are
    offered to `DecodeCompileValue` and every one it accepts must have a row. A
    hand-kept list of grammars would be exact about the ones it knows and silent
    about the one just added, which is the failure the check exists to prevent. What
    makes a new grammar a spec change is the far end: an older build meets the tag,
    `IsKnownGrammar` refuses it, the decoder calls that a malformed frame rather than
    a foreign generation, and a server whose policy for malformed bytes is *store
    verbatim* then stores the producer's absolute paths.
  - **A guard's MESSAGE is part of the guard, and two correct refusals can still
    route an author past both.** Adding a grammar fails the coverage case, which asks
    for a corpus row; adding that row moves the digest, which fails the generation
    case, whose message offered *"you widened the corpus, repin"* first. Both
    refusals were right. From the author's seat that clause reads as precisely what
    they just did — so the pair walked them into shipping a canonicalization change
    under the generation it was not written by, which is this section's own defect
    reproduced one level up, inside the fix for it. **The remedy a refusal offers
    FIRST is the one that gets taken**, so a message reachable by two different edits
    has to name both and say which is which: the repin branch is narrowed to
    behaviour ALREADY in this generation and points at the grammar case as not being
    that, and the coverage case asks for the row *and* the bump. Note how it was
    found — by reading, in review. Every test was green, and a message is not
    something a suite can be wrong about.
  - **A digest cannot tell a row that does work from one that does not**, and that
    is a property of digests rather than a flaw in this one: both rows contribute
    bytes, so a corpus can grow, read as thorough, and assert progressively less. The
    bare-root row added here was the demonstration — it was commented *"a bare root
    produces, and a bare root consumes"* and did **neither**, because nothing
    canonicalized so there was no token, so there was nothing to localize either. The
    guard that caught the `JoinLocalized` mutation fired on the *drive-root* row
    instead, and everything still passed. `RegionEffect` is the assertion the digest
    structurally cannot make: each row declares `Rewrites` or `Preserves` per side and
    is checked against what actually happened
    ([#547](https://github.com/LASTRADA-Software/fastcached/issues/547)).
    - **A declaration, not "every row must change something."** Two rows here are
      deliberately inert and always will be — canonicalizing a region that already
      carries tokens must be a no-op or canonicalizing twice would be a second
      rewrite, and the empty region exists to prove the framing survives with no text.
      A guard that has to be waived twice is a guard on its way to becoming
      decoration.
    - It also **says which row broke**. Measured: reverting the producer fix fails the
      digest once and the effect assertion four times — both sides of both bare-root
      rows, since with nothing canonicalized there is no token for the consumer to
      localize either — and each failure names its row. The digest alone can only
      report that something moved.
  - **Vary BOTH sides of a transformation, or the corpus cannot see the side it
    fixed.** The bare-root miss was producer-varied and consumer-fixed, and the
    mutation written specifically to prove the guard bites passed all 2007 cases
    because of it. The audit that followed enumerated every root shape against both
    sides and found the defect on **four cells across two shapes** — plus a bare
    WINDOWS producer absent from the corpus in any form, which neither the inertness
    check nor the one-sided check would have found alone. Two shapes that were also
    varied on one side only, drive-relative and UNC, turned out to be **correct** —
    which is the point: nothing distinguished them from the broken ones beforehand, so
    they get rows too. A row documenting a shape as sound is worth as much as one that
    catches a defect.
  - **It also pins the digest across HOSTS.** Every entry point in `PathCanon`
    derives its conventions from the layout rather than from the running binary, so
    the corpus yields one digest on Windows, Linux and macOS. A change that broke
    that would make a Windows server and a POSIX one disagree about a value they both
    hold — the same defect with no version skew anywhere near it.
- **The reader could not tell a foreign value from a damaged one.**
  `CanonicalStoredValue` returned `std::optional`, and its `nullopt` meant both "these
  bytes are not a stored value" and "this IS one, of a generation I do not
  implement". The two servers apply *opposite* policies to that single state — the
  daemon refuses, a node's cache tier stores the bytes **verbatim**, each defensible
  for genuinely opaque bytes — so a launcher at generation N storing into a node at
  N+1 put the producing checkout's absolute paths into the shared cache under a key
  every machine computes. That is #229/#319 reached by nothing worse than a rolling
  upgrade, and it is `.agent/rules/metrics-and-observability.md`'s four-states rule:
  an `optional` cannot carry it.
  - `CanonicalizationOutcome` names three, and what protects a caller is **switching
    on it with no `default:`**, never the empty `bytes` that `ForeignGeneration`
    carries. Empty bytes stop a server storing *nothing*; they do not stop one
    storing the ORIGINAL, and a node's verbatim fallback was never the canonical
    bytes but the STORE's own payload — so `outcome == Canonicalized ? canonical.bytes
    : payload` compiles, reads naturally, and reinstates the whole defect. No type
    can refuse that. The exhaustive switch is what makes the third state impossible
    to leave out.
  - `DecodeCompileValue` answers `UnsupportedFeature` for an unknown generation and
    `MalformedFrame` for everything else, and the classifier reads that code back
    rather than re-testing the version byte: two spellings of one rule are two places
    for it to drift. It is `.agent/rules/storage.md`'s `UnsupportedFormatVersion`
    against `Corrupt`, one layer up and on the wire instead of on disk — and for the
    same reason, since the obvious remedy for a cache reported corrupt is to wipe it.
  - **Another generation is proved, never inferred from the leading byte alone** —
    and the first cut of this got it backwards, which the OTHER server's tests caught
    rather than any reasoning here. Almost no opaque blob begins with `0x01`, so
    "leading byte is not ours" called every opaque value foreign and refused it,
    overturning the node cache tier's documented policy of storing an opaque value
    verbatim — a policy this layer does not own. `DecodeAfterGeneration` therefore
    runs on the rest of the frame too, and only a layout that holds together is
    reported as a generation. The shape that matters (#547 — framing kept,
    canonicalization moved) is then caught exactly. **The residual is not the harmless
    direction, and saying so was this bullet's own first mistake**: a generation that
    moves the FRAMING as well reads as junk, comes back `NotACompileValue`, and a
    node's tier handles THAT by storing verbatim — so it re-enters through the door
    this closes, for the class of bump most likely to cause it, since
    `CompileValueVersion` names the framing too and nothing couples it to an
    `objkey-v*` bump. Bounded rather than closed: the daemon refuses
    `NotACompileValue`, so the forward cannot reach the shared cache, and a tier is
    loopback-only. Bounds are not a fix, so it is tracked. The general form is the one worth carrying: **absence
    of the expected value is not evidence of a particular alternative**, and a
    classifier built on it will be confidently wrong about everything that is merely
    unfamiliar.
  - **No tag moved for this.** Nothing observable about generation 1 changed, so the
    population of values stored verbatim by this route is empty **by construction**.
    The reason is the FRAMING, not the generation byte: this route needs a bump that
    moves the layout, and no bump has. Saying instead that "`CompileValueVersion` has
    never moved" was a weaker claim wearing the same words, and it expired at
    [#547](https://github.com/LASTRADA-Software/fastcached/issues/547) — generation 2
    exists, generation-1 values are in the field, and the `ForeignGeneration` refusal
    above is doing work today while this residual is still empty. Two conditions that
    happened to hold together are not one condition.
- **The launcher says which of the two it met.** `DecodeFailureReason` gives a
  foreign generation its own `--show-stats` row, because a mixed fleet is a rolling
  upgrade that ends by itself while a malformed value is a defect somebody has to
  look at, and an operator does different things about them. Otherwise an upgrade
  window presents as an endlessly cold cache with no diagnostic, which this wire has
  already recorded paying for once.

## Accepted trade-offs

These are argued in place above and are **not** open work — do not "fix" one
without reopening the argument:

- **`ValidateManifest` accepts a manifest that names the TU and no header, and
  nothing but the PRODUCER stops one existing.** `ManifestAssertsNothing` is
  `entries.empty()`, and a TU-only manifest is not empty — so a hollow manifest
  reaching the validator would revalidate on the TU hash alone and serve its
  recorded object into any checkout that computes the key, which is
  [#368](https://github.com/LASTRADA-Software/fastcached/issues/368).
  It cannot currently be built. Three producer-side guards refuse it, and two of the
  three now live at the seam rather than one translation unit away:
  `DepsNotObserved` when no dependency record was observed at all, `NoProjectDeps`
  when paths were reported and every one dropped (`4739f54`) — both in
  `BuildManifest` — and `UnreadablePaths` in `RecordManifest` when a reported path is
  not readable as text (`47ee5e5`).
  The first of those was `RecordManifest`'s own `includes.empty()` early return until
  [#512](https://github.com/LASTRADA-Software/fastcached/issues/512), and moving it is
  what that issue was for. **"No dependencies" and "dependencies not observed" are
  different states that an empty `std::vector<std::string>` renders identically**, and
  `BuildManifest` used to tell them apart by inferring honesty from
  `!includePaths.empty()` — sound only because of a guard nothing in
  `DirectManifest.cpp` could see. So the FOUR states are: deps observed and some
  project ones survived (a manifest); observed, some reported, none survived
  (`NoProjectDeps`); observed and none reported (an honest TU-only manifest); and not
  observed (`DepsNotObserved`). The last two were the collapsed pair. `ManifestInputs`
  now carries a `ReportedDependencies` whose **default constructor is deleted**, so a
  new caller cannot omit the answer — designated initializers value-initialize an
  omitted member in silence, which is how a defaulted field would have made the guard
  decorative.
  The third state has **no producer**, and that is a finding rather than an oversight:
  `RecordManifest` cannot separate "there is nothing to read" from "I could not read
  it", because `IncludeNoteMarker` is the literal English `"Note: including file:"`
  and a localized `cl` prints notes it does not match
  ([#692](https://github.com/LASTRADA-Software/fastcached/issues/692)). It therefore
  answers `NotObserved()`. The type puts the judgement where the knowledge is.
  **So the accepted cost is the asymmetry, not a live hole**: the defence is
  entirely on the produce side, and the validator would accept a hollow manifest
  that arrived any other way — decoded from a store written by an older or foreign
  producer, for instance. Tightening `ManifestAssertsNothing` to refuse a TU-only
  manifest is the defence-in-depth fix, and it is cheap; #512 did not do it and did
  not make it unnecessary — that is a CONSUMER-side question about a manifest
  carrying no provenance, and #512 moved the PRODUCE-side one into the type. The two
  do not substitute for each other. What is NOT acceptable is
  removing any of the three producer guards on the belief that the validator would
  catch it. `DirectManifest_test.cpp`'s two cross-checkout cases characterize the
  validator's current answer and say in-place that they must change if it is
  tightened; the `Observed({})` they build is a lie the fixture tells deliberately
  and says so, since no honest caller spells that state.
- **`VSLANG=1033` on the banner probe collapses the common locale split and opens a
  rarer one, and the rarer one has no detector.** Since #195 the banner IS the
  compiler's identity, and `cl` localizes it, so one MSVC toolset under two Visual
  Studio UI languages was two identities that shared no cache entry and matched
  nothing in the fleet. Asking the probe for English
  ([#200](https://github.com/LASTRADA-Software/fastcached/issues/200)) collapses that
  — which is the commoner split, between whole machines, and the trade is a good one.
  **What it costs is an estate whose machines differ only in WHICH PACKS ARE
  INSTALLED**: German UI throughout, `en-US` present on some. Those machines matched
  each other before and no longer do — the one that can honour `VSLANG` derives the
  English identity and the one that cannot derives the German one. That is #200's own
  symptom, newly introduced for exactly that shape of estate.
  **And there is no detector, nor can there cheaply be one.** The `/showIncludes` twin
  has `Cc::CarriesUnreadableIncludeNotes`, which can see that a request did not take
  because it parses the answer. A banner is one opaque line: nothing distinguishes
  "this is English" from "this is a language shaped like the last one" without a
  second probe spawn per compile to compare against. So this case is silent by
  construction, and that is the half most likely to be rediscovered as a bug and
  "fixed". It is not a bug; it is this row. The argument is on `CompilerBanner` in
  `apps/fastcache-cc/ToolchainProbe.cpp`, which points here.
- MurmurHash3 is not collision-resistant against an adversary. Accepted because
  the key is not a security boundary: anyone who can STORE can already write a
  wrong object under a correct key. Closing it needs a keyed hash *and* a trust
  model for STORE.
- Two machines whose compilers print the same version banner from different
  install prefixes share a key and can replay each other's toolchain paths.
  Hashing those paths would give up cross-machine sharing wholesale;
  `Cc::MissingReplayedDependency` is the backstop instead.
- The 64 MiB floor in `SessionContext` means a cap below it cannot be exercised
  end-to-end without a genuinely large fixture, so the e2e drives the client
  ceiling and `TcpClient_test` pins the socket half.
- A stored stream region mixes encodings when the compiler's is not UTF-8: the
  path spans are decoded, the text around them is the tool's own bytes. Replayed
  on a hit to a console that is not UTF-8, a non-ASCII path therefore renders
  differently than the uncached compile printed it. Accepted, because the
  alternative is the one that cannot work: an entry storing the producer's code
  page is an entry no other machine can canonicalize, which is the whole of what
  this cache is for. `chcp 65001` makes the two identical.
- **On COFF a replayed object's debug records name the producing checkout, and no
  flag closes it.** `-ffile-prefix-map` does not remap CodeView's `S_OBJNAME` or
  clang-cl's embedded `-cc1` line — measured, still 23 bytes apart with the flag —
  and `cl` has no path-map switch at all. The alternative is #489's shape, which
  gives up cross-checkout sharing on **every** `cl` compile, debug info or not,
  and that sharing is what the launcher exists to provide on that platform. So the
  residue is accepted and stated rather than closed. Coverage, which has the same
  defect, is hard-guarded (`cmake/Coverage.cmake` refuses a launcher outright);
  debug info is not, because a wrong source path in a debugger is a nuisance and a
  wrong coverage report is a lie that looks like data.

## A performance figure is a quantity under conditions, and both halves get lost separately

`ProbeToolchainFiles` walks a toolchain's include roots and hashes **every byte** of
every file. Its header recorded the cost as **"about 2 s warm"** — measured
honestly, correctly attributed, with the condition attached. That figure then
appeared twice more:

- `ComputeToolchainStamp`: *"the alternative is the 2-second full walk this exists
  to avoid"*
- `CachedToolchainFingerprint`: *"The full walk costs about 2 seconds over 288 MB"*

**Both dropped the `warm`.** And the design downstream was reasoned from the
citations rather than from the measurement, so a walk that is seconds warm and was
observed exceeding **300 s** cold on a Windows runner was treated as a two-second
operation for as long as anybody remembered it.

Two separate failures, and neither is staleness — the number was never wrong.

**Attaching the conditions is necessary and not sufficient, because the citation
is where they get lost.** The original was exemplary; it did not survive being
quoted twice. So a figure that is going to be *referred to* elsewhere belongs in
**one** place that the other sites point at, rather than being restated — the same
reasoning `check-tsan-scope.cmake` follows for reading a tag expression instead of
copying it. A second copy of a measurement is not a cross-check, it is a second
thing to drift.

**And a figure can be current, correctly measured, correctly attributed, and still
be the wrong quantity.** That is a distinct failure and it deserves naming
separately. Here it was *circular*: the warm cost was quoted as the price of
**missing the cache** — but the cache is the mechanism that makes the warm case
warm. What a miss costs is by definition the cold number. Nobody was quoting a
stale figure; they were quoting the right figure for the wrong question, which no
amount of re-measuring would have caught.

The corrected form is one table at the measurement site, showing the spread rather
than a number, with the conditions as rows:

| condition | per file |
|---|---|
| warm page cache, local SSD | 0.21 ms |
| cold, local SSD, anti-malware active | **5.00 ms** |
| Linux (gcc 14), cold | 0.26 ms |

A spread also states its own uncertainty, which a single number cannot. Anyone
citing it has to pick a row, and picking a row is the moment the question "under
which conditions?" gets asked.

### The same rule governs a claim handed between people

Everything above is written about a figure in a code comment, and the mechanism has
nothing to do with code. It is a claim that was true when someone measured it,
restated once, and arriving at the next reader with its conditions stripped. A
**handoff is a citation**, and it is the citation that loses them.

It is worse than the comment case in one specific way: a reader who doubts a comment
can go and re-measure. A reader who is *handed* a claim usually cannot, because the
measurement was made somewhere they cannot see -- another session, another machine,
a run whose log has gone. What reaches them is a sentence with the same shape whether
it was measured, inferred, remembered or guessed, and nothing in that sentence
distinguishes the four.

Four in one session, all through a handoff, none of them a mistake about the subject:

- a mechanism reported by one session and published by another **as established**,
  when it was a reconstruction that the subject itself contradicted
  ([#517](https://github.com/LASTRADA-Software/fastcached/issues/517));
- a `36/36` correspondence **verified by hand** and relayed as though a guard
  enforced it;
- a **pull request number** that was an issue number, handed over as checkable;
- *"thirty lines up"*, written in a comment that quoted **both line numbers** and
  never subtracted them -- the real distances being 117 and 212
  ([#172](https://github.com/LASTRADA-Software/fastcached/issues/172)).

That last one shows the correction usually strengthens the point rather than denting
it. *Thirty lines* invites "somebody glanced past the helper"; **117 and 212** means a
reader can arrive at either call site having never seen the helper at all, which is a
better account of how a rule fails to travel and a worse one for anybody arguing that
proximity is sufficient.

**The remedy is on the sending end, because only the sending end can do it: say what
was MEASURED and what was INFERRED, separately, every time.** The receiver cannot
recover the distinction at any price, and the sender can state it for free. A claim
worth acting on is worth one clause saying which of the two it is.

## Open work

- **[#188](https://github.com/LASTRADA-Software/fastcached/issues/188)** — the
  target-triple probe costs a driver spawn per translation unit on clang and
  clang-cl, hits included, because its answer is a cache key input. Memoizing it
  under the fingerprint's stamp is unsound: that stamp does not cover the MSVC
  install the answer depends on, so a stale value would be a wrong hit rather than
  a miss.
- **[#200](https://github.com/LASTRADA-Software/fastcached/issues/200)** — `cl`
  localizes its banner, and since [#195](https://github.com/LASTRADA-Software/fastcached/issues/195)
  that banner is the compiler's identity, so two machines holding one toolset under
  different Visual Studio language packs share nothing and match nothing. Correct but
  needlessly conservative. The fix is `VSLANG=1033` on the PROBE alone, which needs
  per-spawn environment on `IProcessRunner` -- setting it process-wide would change the
  language of the diagnostics the operator sees. Not token extraction: no rule over
  "the version-looking word and the last one" survives a locale nobody has read.
- **[#800](https://github.com/LASTRADA-Software/fastcached/issues/800)** — a client
  whose source argument is ABSOLUTE and whose line carries a matching
  `-fdebug-prefix-map` records the mapped spelling locally, while the dispatched object
  records the unmapped one: `RemoteCompileArgs` drops the client's rules by design, so
  the worker maps its scratch to the raw spelling it was sent. #506's disagreement shape
  one attribute over, and strictly better than the `job-N` scratch path #660 replaced.
  The fix is to send what the client's own compile RECORDS rather than what the build
  system wrote -- `MappedCompileDirectory` is already that computation, named and
  documented for the working directory only -- and the reason it was not folded into
  #660 is that its only call site is `main.cpp`, which no test can reach. No wire field
  either way.
- **[#64](https://github.com/LASTRADA-Software/fastcached/issues/64)** — a
  relative include-dir argument still reaches the key verbatim through
  `RelativizeArgs`, so two build trees at different depths key apart on the
  arguments even though their dependency sets now agree.
- **[#583](https://github.com/LASTRADA-Software/fastcached/issues/583)** — a
  RETIRED generation's conformance digest is a dated record and nothing can
  re-derive it: it describes the corpus as that generation met it, and #547 retired
  generation 1 while adding three corpus rows in the same commit. The live row is
  guarded, and a reverted bump is caught structurally (unique ascending versions,
  the live byte naming the last row) — what is missing is any assertion that a bump
  changed what it SAID it changed, which needs a per-generation frozen corpus. Read
  with #548, since the key side's retired rows have the same property and whichever
  lands second inherits the other's shape.
- **[#548](https://github.com/LASTRADA-Software/fastcached/issues/548)** — the
  retired-generation idiom has two homes with different key types:
  `apps/fastcache-cc/KeyDigestTestSupport.hpp` keyed on a schema-tag string, and
  `CompileValue_test.cpp`'s generation table keyed on a version byte. It could not be
  reused as it stands because a library test may not include an app header, so the
  shared home is `src/tests/` — and that constraint is what shapes the fix rather
  than being incidental to it.
