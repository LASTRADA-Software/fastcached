// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>

#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// The inputs that determine a shared cache key. Two machines that produce the
/// same `KeyInputs` share a cache entry — so every field must be free of any
/// machine-specific detail (the args are pre-relativized, the preprocessed text
/// has its line markers normalized by the compiler's own preprocessor).
struct KeyInputs
{
    /// Stable compiler identity: its version banner, and the target it generates
    /// for when the driver can be asked (see `CacheCompilerId`).
    ///
    /// The target half is not decoration. A banner alone identifies the DRIVER, and
    /// a driver's code generation is not a function of the driver alone: clang-cl
    /// derives `-fms-compatibility-version` from whatever MSVC it can find beside
    /// it, and clang's Microsoft C++ ABI gates version-specific code generation on
    /// that. Two machines running one `clang-cl.exe` beside two MSVC installs print
    /// the same banner, preprocess to the same text -- measured byte-identical even
    /// for a TU including `<cstdio>`, because `_MSC_VER` need not survive into the
    /// output -- and would otherwise share an entry while needing different objects.
    std::string compilerId;
    std::string preprocessed;                 ///< Preprocessor output for the TU.
    std::vector<std::string> relativizedArgs; ///< Compile args with checkout-rooted paths relativized.

    /// The dependency paths the compile resolves, in portable form (see
    /// DependencyProbe's KeyDependencySet, which produces this: canonical tokens,
    /// sorted and deduplicated, toolchain paths dropped — a relative path having
    /// been resolved against the compile's working directory first, so that what
    /// it names rather than how it was spelled decides which of the two it is).
    ///
    /// A hit reproduces TWO artefacts — the object and the build system's
    /// dependency record — and without this the key determines only the first.
    /// Suppressed line markers keep every path out of `preprocessed`, which is
    /// what makes a key portable and equally what makes it invariant under a
    /// header MOVE: the token stream is unchanged, so the object is still correct
    /// and served, while the replayed depfile names a file that is gone. The build
    /// system then records that dependency, cannot stat it, rebuilds, hits the same
    /// value, and never converges (issue #53). Naming the paths here makes a move a
    /// different key by construction (issue #56).
    std::vector<std::string> dependencyPaths;
};

/// Compute the shared cache key as a wide hex digest. Isolated here so the key
/// derivation can be tuned against real cross-machine hits without touching the
/// launcher flow.
///
/// A 128-bit MurmurHash3 x64_128 digest (`Core/MurmurHash3.hpp`) over the joined
/// inputs, reached through `KeyDigest`'s grammar: not cryptographic, but
/// collision-negligible for a compile cache and dependency-free. It genuinely is
/// 128 bits — the construction this replaced was four CRC32C runs over one blob
/// differing only by a salt byte, which carried 32 (issue #63).
///
/// The schema version deliberately does not appear here. It is spelled once, in
/// ComputeKey, next to the construction it versions; naming it in prose as well
/// gave two places to update and they had already drifted apart.
/// @param inputs The machine-independent key inputs.
/// @return A 32-hex-char key string.
[[nodiscard]] std::string ComputeKey(KeyInputs const& inputs);

/// Build the compiler identity a cache key is keyed on.
///
/// Two facts joined with a newline, which is a framing rather than a decoration:
/// concatenating them bare would let `("ab", "c")` and `("a", "bc")` digest alike.
/// A banner is one line by construction (`CompilerBanner` takes the first) and a
/// triple is letters, digits, dots, underscores and dashes, so neither half can
/// contain the separator and the pair is recoverable from the result.
///
/// An empty @p targetTriple yields the banner UNCHANGED, not a banner with an
/// empty field appended. That is what keeps every driver with no target to state
/// -- `cl`, `gcc`, an unknown driver -- keyed exactly as it was, so their existing
/// cache entries stay reachable and only the drivers that gained an identity are
/// re-keyed.
///
/// This is deliberately NOT what the toolchain fingerprint is built on. A
/// fingerprint decides which WORKER may serve a client, and folding the target into
/// it would split a developer-prompt launcher from a service-run worker again --
/// the mismatch #145 removed, reintroduced for a fact the dispatch line now states
/// outright. A key decides which OBJECT may be served, and there the same fact is
/// load-bearing. One string served both questions; it cannot.
///
/// The triple is folded VERBATIM, including the version its environment field
/// carries, and that is a deliberate over-specification rather than an oversight.
/// Clang's Microsoft ABI gates on major.minor (19.12, 19.33), so two MSVC builds
/// differing only in patch generate identically and now key apart -- a lost match
/// on every Visual Studio point release. Truncating would recover those, and is not
/// done, because the same field means something else on another platform: on Apple
/// targets it is the DEPLOYMENT TARGET (`arm64-apple-macosx15.0.0`), which does
/// change code generation, so one rule cannot serve both and a per-platform rule
/// would be guessing at which components matter for a triple this code has never
/// seen. Over-specifying costs a recompile; under-specifying serves an object built
/// by a different code generator, and only one of those is recoverable.
///
/// @param banner The compiler's own version line.
/// @param targetTriple The target it generates for, or empty when it has none to
///        state.
/// @return The identity to key on.
[[nodiscard]] std::string CacheCompilerId(std::string_view banner, std::string_view targetTriple);

/// Rewrite every argument whose path lies under `sourceRoot` or `buildTree` to
/// a checkout-independent token form (via PathCanon), leaving other arguments
/// untouched. This is what makes the key identical across checkouts at
/// different absolute roots. Handles bare source paths and every flag in
/// `CmdLine`'s PathValueFlags() table — the object output, the include dirs and
/// the depfile options — in both the fused and the separate spelling.
///
/// BOTH spellings, off one table, because the two used to be answered by two:
/// a separated value is an argument of its own and reached the bare-path branch,
/// while a fused one went through a table that listed the include-dir prefixes
/// and nothing else. So `/Fo<abs>` — what every build system driving MSVC
/// writes — kept the producing machine's object path, and two Windows checkouts
/// at different roots could never share an entry, while the separated form used
/// by the unit tests worked and hid it.
///
/// Both roots matter: a checkout commonly nests its build tree *inside* the
/// source root (e.g. `<src>/out/build/...`), and include args frequently point
/// into it (vcpkg headers, generated config). Relativizing only the source root
/// would leave those build-tree paths absolute — and machine-specific — so the
/// key would differ across machines even for identical content. `PathCanon`
/// emits `<BUILDTREE>` for the (longer-matching) build-tree paths and
/// `<SRCROOT>` for the rest.
/// `resolve` reconciles a spelling before the roots are consulted, and it is
/// applied to the path PORTION this function has already isolated rather than to
/// the whole argument — which is the reason it is injected here instead of in the
/// caller: `IncludePrefixes` is the only table that knows where an `-I` flag ends
/// and its path begins, and re-deriving that outside would be a second copy of it.
/// It matters because a build system may spell an include directory in a form the
/// compiler never echoes back (an 8.3 short component, a `subst` drive), and an
/// argument that does not reconcile against the roots leaves the checkout location
/// in the key — issue #66. Left empty, every path is used exactly as written.
/// @param args       The raw compile arguments (excluding the compiler).
/// @param sourceRoot The checkout source root to relativize against.
/// @param buildTree  The build output root to relativize against (may be empty).
/// @param resolve    Optional spelling reconciliation for each isolated path.
/// @return The arguments with source-/build-rooted paths tokenized.
[[nodiscard]] std::vector<std::string> RelativizeArgs(std::span<std::string const> args,
                                                      std::string_view sourceRoot,
                                                      std::string_view buildTree,
                                                      PathCanon::PathTransform const& resolve = {});

/// Reads one response file's bytes, or answers nothing when it cannot. The path is as the
/// command line spelled it; resolving it against the compile's working directory is the
/// reader's business.
using ResponseFileReader = std::function<std::optional<std::string>(std::string_view path)>;

/// The key material a command line's response files (`@file`) carry, or why the compile
/// must not be cached.
///
/// A driver expands `@file` itself, so the arguments inside it never reached the key: the
/// key hashed the NAME. Two builds differing only in what the file says (`-O0` against
/// `-O2`, a define, `/arch:`) shared one entry -- measured: changing `-O0` to `-O2` inside
/// the file was answered with the `-O0` object, a direct-mode HIT, because the manifest key
/// sees the arguments and not the text the file expands to.
///
/// Each file's BYTES are keyed, so no tokenizer of this launcher's can disagree with the
/// driver's about what they say; the three roots are first replaced by their tokens, so two
/// checkouts still share where the file names them in the build's own spelling (any other
/// spelling is a miss, never a wrong hit). Refused, rather than keyed, when a file:
/// - cannot be read -- there is nothing to key;
/// - names another response file -- its bytes would be outside the key again;
/// - carries an output or dependency-file flag -- the launcher reads those off the command
///   line to restore what a hit writes, and one hidden in a file would be written by nobody.
/// @param args   The compile arguments (excluding the compiler).
/// @param layout The roots to tokenize within each file.
/// @param read   Reads a response file.
/// @return One key item per response file, in order; or the refusal's reason.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> ResponseFileKeyItems(std::span<std::string const> args,
                                                                                        PathCanon::Layout const& layout,
                                                                                        ResponseFileReader const& read);

/// The preprocessed text the key hashes: the probe's output with every line marker's PATH
/// removed and its line number and flags kept.
///
/// The probe keeps its markers (`-E`, `/E`) because they carry the line structure: without
/// them a comment inserted above a function left the text byte-identical while
/// `std::source_location`, `__builtin_LINE` and every `-g` line table moved, so the edited
/// file was served the object built before the edit. The marker's path is what made it
/// unportable -- it is the checkout's absolute path -- and nothing in the object depends on
/// it that a key could see: `__FILE__` is already expanded in the text, and a file name the
/// compiler records after preprocessing (`source_location::file_name`, debug records) is
/// root binding's and the prefix maps' to answer.
///
/// Both spellings are recognised: GNU's `# <line> "<path>" <flags>` and MSVC's
/// `#line <line> "<path>"`. A line that is not a marker of either shape -- a `#pragma`, an
/// assembler comment -- is kept verbatim, as is every other byte.
/// @param text The probe's stdout.
/// @return The text with each marker reduced to its line number and flags.
[[nodiscard]] std::string KeyedPreprocessedText(std::string_view text);

/// `RelativizeArgs` over a whole layout, its dependency root included: a path under
/// `layout.dependencyRoot` becomes a `<DEPROOT>` token, so an include directory into a
/// per-user dependency cache stops keeping the user's home in every key.
/// @param args    The raw compile arguments (excluding the compiler).
/// @param layout  The roots to relativize against.
/// @param resolve Optional spelling reconciliation for each isolated path.
/// @return The arguments with rooted paths tokenized.
[[nodiscard]] std::vector<std::string> RelativizeArgs(std::span<std::string const> args,
                                                      PathCanon::Layout const& layout,
                                                      PathCanon::PathTransform const& resolve = {});

/// The first argument carrying a path this launcher can neither key nor guard, or
/// nothing when the whole command line is safe to cache (issue #104).
///
/// The rule is `IsDriveRelativeUnderNoRoot`'s and is spelled once, there. What
/// this adds is *where* it is asked: on the command line, before the launcher has
/// done anything at all.
///
/// That placement is the substance rather than an optimization. The authoritative
/// answer comes from the paths the compiler actually reported, but that is only
/// known after the preprocess — and direct mode runs *before* it, validating a
/// manifest whose entries came through the same filter and therefore dropped the
/// very path in question. A manifest recorded by an older launcher would keep
/// direct-hitting a stale object forever. Asking here steps out before the
/// manifest is consulted at all, which retires those entries by making them
/// unreachable rather than by bumping a schema tag and invalidating every entry on
/// every platform for a clang-cl-only exposure.
///
/// What it cannot promise is completeness, which is why it is not the only ask:
/// isolating an argument's path depends on `PathValueFlags()` recognising the
/// flag, and `CouldNameAFile` records in as many words that such a list is what
/// cannot be kept complete (`-isystem<path>`, `--sysroot=`, `-B`, a partial
/// `@response-file`). `KeyDependencySet` asks the same question of what the
/// compiler reported, where there is no list to be incomplete.
///
/// Every `PathValueFlags()` role is examined, not only `IncludeDir`. Filtering by
/// role would be a second list to keep complete, and the only cost of examining
/// one that could not have seeded a dependency path is a lost cache on a layout
/// nothing common generates.
///
/// Which introducers may match, and therefore which arguments are bare paths, is
/// the LAYOUT's answer and not the host's — see RelativizeArgs, whose walk this
/// shares.
///
/// @param args   The raw compile arguments (excluding the compiler).
/// @param layout This machine's roots, and the source of path conventions.
/// @return The offending argument, verbatim, or nullopt when there is none.
[[nodiscard]] std::optional<std::string> UnkeyableArgument(std::span<std::string const> args,
                                                           PathCanon::Layout const& layout);

} // namespace FastCache::Cc
