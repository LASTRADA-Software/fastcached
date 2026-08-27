<!-- The one wording of the MSVC/clang-cl sccache hazard. Every page that states
     it includes it from here with `--8<-- "sccache-backend-caveat.md"`; README.md
     and `fastcached --help` restate it, because neither can include anything.

     `ctest -R sccache-backend-caveat` fails on any file that names
     SCCACHE_MEMCACHED or SCCACHE_REDIS without a caveat within 40 lines that
     carries all three of: the exposed compilers, the mechanism, and the remedy.
     Reword freely -- keep those three, and keep the scope: overstating this
     would send a CI fleet that is not exposed away from a cache that is safe
     for it. -->

!!! warning "On MSVC and clang-cl, one sccache cache must not be shared between checkouts"

    sccache preprocesses MSVC and clang-cl with `/EP`, which emits no line
    markers, so the text it hashes to *find* a cache hit carries **no paths at
    all** — while the `/showIncludes` stream it replays *on* that hit carries the
    **absolute** paths of the checkout that stored it. Two checkouts sharing one
    cache therefore record dependencies pointing into each other, after which
    editing a header in the checkout you are building rebuilds nothing: the build
    stays green and the objects are stale.

    Measured on this project — a second worktree recorded **1097** dependency
    edges into the first and **none** into itself — and reproducible in two
    compiles: with sccache 0.14.0 and MSVC 14.51, a second directory took a cache
    hit from the first, and its `/showIncludes` named the **first** directory's
    header.

    **What is exposed is narrower than "sharing a cache".** It is an *incremental*
    build across checkouts at **different absolute paths**. A clean build has no
    dependency graph to corrupt, and checkouts that all sit at the same path
    replay paths that are correct — a CI fleet is normally both, and this is not a
    reason to take the cache away from one. **GCC and Clang are not exposed at
    all**: their preprocessed output carries the paths, so two checkouts never
    share an entry to begin with — the same two compiles under g++ 14 were 0 hits
    and 2 misses.

    A `fastcached`-backed sccache is *definitionally* one cache shared by every
    checkout and every machine pointed at it, so the developer machines behind it
    are exposed even where CI is not. On MSVC and clang-cl, use
    [`fastcache-cc`](https://lastrada-software.github.io/fastcached/tools/fastcache-cc/)
    instead: it rewrites a hit's paths into the consuming checkout before replaying
    them, and refuses a hit whose replayed dependency is not there — which is why
    it does not have this failure mode, and why its entries are portable across
    checkout paths on purpose.
