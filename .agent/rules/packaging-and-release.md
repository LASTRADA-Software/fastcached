# Packaging and releasing

Rules for `packaging/`, `cmake/Packaging.cmake`, `cmake/Version.cmake` and the
release half of `.github/workflows/build.yml`, plus how a release is actually cut.

Read this before touching an install rule, a maintainer-script template, an
installer pane, the version derivation, or the release job's asset list.

## Packaging

- **The package payload is rooted at `/`, not `/usr`.** `/etc` cannot sit under
  a `/usr` prefix, so `FASTCACHED_INSTALL_BINDIR`/`DOCDIR` spell their own
  `usr/` (and `opt/fastcached/` on macOS). A relative destination for the units
  would put them where systemd never looks — and on macOS an *absolute*
  `install(DESTINATION)` escapes CPack's staging tree and writes to the build
  host's real filesystem.
- **Neither a macOS `.pkg` nor an MSI has a conffile mechanism.** Both overwrite
  their payload on every install, so on both the live `fastcached.yaml` is
  deliberately not payload: only a `.default` template ships, and it is copied to
  the real location exactly once, when nothing is there. macOS does this in the
  Runtime postinstall (`seed-config.sh.inc`), Windows in a custom action running
  the daemon's own `--seed-config` — which takes its destination from
  `SystemConfigPath`, so the seeded file and the startup lookup cannot disagree.
  Only the DEB conffile and the RPM `%config(noreplace)` can ship the live file
  directly. Uninstalling leaves the config behind on every platform.
- **An HTML installer pane must begin with its doctype.** Installer.app decides
  HTML from plain text by sniffing the first bytes of the resource, so the
  `<!-- SPDX-License-Identifier -->` header that opens every other file in the
  tree made the welcome and read-me panes render with every tag visible — the
  `.txt` license pane looking right is what disguised it. `mime-type="text/html"`
  on the Distribution XML element does *not* override the sniff (tried, and the
  panes stayed raw), so the doctype goes first and the licence comment after it.
  The same files need an explicit `<meta charset="utf-8">`: without it the em
  dashes arrive as mojibake, a defect the raw markup was hiding.
- **Third-party `install()` rules must be excluded.** A CPM-fetched zstd brings
  its own, and with the payload rooted at `/` they put `zstd.h` and `libzstd.a`
  into `/include` and `/lib` on the user's machine. Hence `EXCLUDE_FROM_ALL`.
- **macOS binaries must link nothing outside `/usr/lib`.** `CPM_USE_LOCAL_PACKAGES`
  defaults ON and makes CPM prefer Homebrew's shared yaml-cpp, so the package job
  passes `-DCPM_USE_LOCAL_PACKAGES=OFF -DOPENSSL_USE_STATIC_LIBS=ON` and CI
  asserts the result with `otool -L`. `CMAKE_OSX_DEPLOYMENT_TARGET` is pinned to
  13.3 (the floor at which the system libc++ has floating-point `std::to_chars`,
  which `std::format` needs) and must be set *before* `project()`.
## The version

- **The git tag is the only version source, and `version.txt` must never come
  back.** There used to be a committed `version.txt`, and because
  `cmake/Version.cmake` read it *first* it was the real source of truth: a second
  version carrier that each release had to remember to bump in lock-step with the
  tag, and that pinned every build, every wire banner and every package to `0.0.1`
  for as long as it existed. `ctest -R repository-hygiene` now fails if it — or any
  other row in `scripts/check-repository-hygiene.cmake`'s table — is ever *tracked*
  again. The test asks the git **index**, not the filesystem, so it fails at
  `git add` time rather than after a commit, and an untracked local `version.txt`
  stays legitimate (it is in `.gitignore` and is read by nothing). A build that
  cannot reach a tag states its version with `-DFASTCACHED_VERSION=1.2.3`.
- **The resolved version triple must stay a bare numeric `X.Y.Z`, and the fallback
  with it.** `CMakeLists.txt` feeds it to `project(VERSION ...)`, which rejects
  anything else, and CPack carries it into the MSI `ProductVersion` (major/minor
  < 256, patch < 65536), the RPM `Version:` field (where `-` is illegal) and the
  Debian version (where `-` starts the package revision) — so `Version.cmake`
  validates the fields against a table and the *string*, never the triple, carries
  the `-12-gdeadbee`/`-dirty` suffixes. This is why the no-tag fallback is `0.0.0`
  and not `0.0.0-unknown`: the `docker` job takes that path on **every** ref
  regardless of clone depth, because `.dockerignore` excludes `.git/`, so a
  non-numeric fallback would turn every push red. It is also why the release
  trigger matches `v[0-9]+.[0-9]+.[0-9]+` rather than `v*` — a `v0.1.0-rc1` tag
  cannot configure, and failing to *start* costs nothing where fifteen red jobs and
  a burnt notarization slot would.
## CI and the release job

- **Every checkout in `build.yml` that could configure the project passes
  `fetch-depth: 0`, and the release job's asset list must stay the last key of its
  `with:` mapping.** The default depth-1
  checkout fetches no tags, so `git describe` finds nothing and the build silently
  falls back — which in a packaging job means artifacts named after a release
  nobody cut. Full history, not `fetch-tags: true`: fetching a tag into a depth-1
  clone leaves the tagged commit as an unrelated shallow root `describe` cannot
  reach. The two jobs that only *read* the workflow file — `check-release-gate`
  and `release` — are the stated exception and pass no `fetch-depth`, because
  history buys them nothing and a release must not be able to fail on a clone
  parameter that cannot affect it. Separately, `/publish-release` learns what a release should contain by
  parsing that literal asset list, and its extractor stops only at a line that does
  not look like a filename — `draft: true` looks exactly like one, so a key moved
  below the list is collected as a glob that can never match and publication blocks
  forever on a phantom asset. The same reason forbids writing `files:` followed by
  `|` anywhere else in that file, comments included. A step in the release job
  re-reads the list with that same extractor and fails on any entry containing
  whitespace or a colon, so the rule is enforced rather than merely documented.

## Releasing

The version is the git tag, so cutting a release is pushing one:

```sh
git tag -a v0.1.0 -m "fastcached 0.1.0"
git push origin v0.1.0
```

That tag matches the trigger in `.github/workflows/build.yml`, which runs the
**entire** suite against the tagged tree — nothing about a release path is
exercised only at release time — and then the tag-gated `release` job collects the
three packaging jobs' artifacts, asserts that the set is complete and that every
filename carries the tag's version, and creates a **draft** GitHub release with
them attached. Nothing publishes automatically; a human does that with
`/publish-release` once the assets have been verified. `/draft-release` drives the
tagging half.

There is no changelog file: release notes are GitHub's generated commit summary
(`generate_release_notes: true`), so commit subjects are what a reader of the
release page sees.

