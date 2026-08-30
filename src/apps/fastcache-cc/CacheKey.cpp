// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"
#include "CmdLine.hpp"
#include "DependencyProbe.hpp"
#include "KeyDigest.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{
namespace
{

    /// The characters that introduce a command-line option under `layout`, which
    /// is the one question behind both halves of RelativizeOne: which spellings
    /// of a path-valued flag may match, and which arguments are bare paths.
    ///
    /// The answer comes from the LAYOUT, not from the compiling host. A leading
    /// `/` introduces an option under a Windows layout, but on POSIX it starts an
    /// absolute path; treating it as an option there would leave absolute paths
    /// unrelativized and bake the checkout location into the cache key — the
    /// exact failure that breaks cross-checkout sharing. Matching `/I` there
    /// would split a checkout rooted at `/Infra` into the prefix `/I` plus the
    /// fragment `nfra/...`, which lies under neither root and so comes back
    /// verbatim with the absolute path still embedded in the key.
    ///
    /// Keying off `_WIN32` instead would make a Windows-hosted launcher
    /// mis-handle POSIX paths, and it made the behaviour untestable from the
    /// other platform. `PathCanon::IsWindowsLayout` is the one definition of
    /// "is this a Windows layout", shared with the canonicalizer, so the two
    /// cannot drift apart on a root such as `C:/src/proj`; `IntroducersOf` is
    /// the one definition of what each family's options start with, shared with
    /// the parser.
    ///
    /// @param layout The layout whose path conventions apply.
    /// @return The introducer characters recognised for it.
    [[nodiscard]] std::string_view IntroducersFor(PathCanon::Layout const& layout) noexcept
    {
        return IntroducersOf(PathCanon::IsWindowsLayout(layout) ? DriverFamily::Any : DriverFamily::Gnu);
    }

    /// Reconcile one path's spelling, or leave it alone when no transform was
    /// supplied. Spelled once so the two branches below cannot differ on it.
    /// @param resolve The caller's transform; may be empty.
    /// @param path    The isolated path portion.
    /// @return The reconciled path, or a copy of `path`.
    [[nodiscard]] std::string Reconciled(PathCanon::PathTransform const& resolve, std::string_view path)
    {
        return resolve ? resolve(path) : std::string { path };
    }

    /// One argument split into the part that is a path and the part in front of it.
    struct PathPortion
    {
        std::string_view prefix; ///< The flag and its separator; empty for a bare path.
        std::string_view path;   ///< The path the argument carries.
    };

    /// Isolate the path an argument carries, if it carries one.
    ///
    /// Spelled once because two callers need exactly this split and neither may
    /// re-derive it: `RelativizeOne` rewrites the path portion, and
    /// `UnkeyableArgument` classifies it. A second copy of "where does an `-I`
    /// end and its value begin" is precisely how the object output came to be
    /// relativized in its separated spelling and not in its fused one.
    ///
    /// Two shapes, in order:
    ///
    /// - **A fused path-valued flag**: `<flag><path>`, e.g. `/IC:\src\inc` and
    ///   `/FoC:\src\build\u.obj`. Every role qualifies, because every one of these
    ///   values is a path on the producing machine. These args are used for the
    ///   KEY only and never to run a compiler, and the layout does not say which
    ///   driver produced them, so every family's rows are offered. A row matched
    ///   against another family's flag costs nothing — MSVC's `-MTd` (the static
    ///   multithreaded runtime) against the GNU `-MT` row is the case to have in
    ///   mind: the tail is not a path under either root, so canonicalization is a
    ///   no-op and the argument comes back byte-for-byte. A *bare* occurrence,
    ///   whose value is the next argument, carries no path of its own and is
    ///   reported as such — that value arrives on its own iteration as a bare path.
    /// - **A bare path argument**: a source file or a response path.
    ///
    /// What the LAYOUT decides is which introducers may match, and therefore which
    /// arguments are bare paths at all — see IntroducersFor. A `/`-led spelling
    /// under a POSIX layout must reach the bare-path shape, or a checkout rooted at
    /// `/Infra` is mis-split at `/I`; and `-` introduces an option everywhere, so a
    /// `-`-led argument is never a bare path.
    ///
    /// @param arg    The raw argument.
    /// @param layout The layout whose path conventions apply.
    /// @return The isolated path and its prefix, or nullopt when `arg` carries none.
    [[nodiscard]] std::optional<PathPortion> PathPortionOf(std::string_view arg, PathCanon::Layout const& layout)
    {
        auto const introducers = IntroducersFor(layout);
        if (auto const match = MatchPathValueFlag(arg, introducers, DriverFamily::Any);
            match.has_value() && !match->value.empty())
            return PathPortion { .prefix = match->prefix, .path = match->value };

        if (!arg.empty() && !introducers.contains(arg.front()))
            return PathPortion { .prefix = {}, .path = arg };

        return std::nullopt;
    }

    /// Relativize one argument against both roots: if it carries a path that lies
    /// under the source root or the build tree, replace that path with its
    /// canonical token; otherwise return the argument unchanged. PathCanon prefers
    /// the longer-matching root, so a build tree nested under the source root
    /// tokenizes to `<BUILDTREE>`.
    ///
    /// An argument that does not tokenize comes back exactly as written, never in
    /// its reconciled spelling: reconciliation exists to make the ROOT TEST
    /// succeed, and rewriting an argument it did not help would change the key for
    /// a path the cache has no opinion about.
    /// @param arg     The raw argument.
    /// @param layout  The source-root / build-tree layout to relativize against.
    /// @param resolve Optional spelling reconciliation for the isolated path.
    /// @return The (possibly) relativized argument.
    [[nodiscard]] std::string RelativizeOne(std::string_view arg,
                                            PathCanon::Layout const& layout,
                                            PathCanon::PathTransform const& resolve)
    {
        auto const portion = PathPortionOf(arg, layout);
        if (!portion.has_value())
            return std::string { arg };

        auto const path = Reconciled(resolve, portion->path);
        auto const canon = PathCanon::Canonicalize(path, layout);
        if (canon != path)
            return std::string { portion->prefix } + canon;
        return std::string { arg };
    }

} // namespace

std::vector<std::string> RelativizeArgs(std::span<std::string const> args,
                                        std::string_view sourceRoot,
                                        std::string_view buildTree,
                                        PathCanon::PathTransform const& resolve)
{
    PathCanon::Layout const layout { .sourceRoot = std::string { sourceRoot }, .buildTree = std::string { buildTree } };
    std::vector<std::string> out;
    out.reserve(args.size());
    for (auto const& arg: args)
        out.push_back(RelativizeOne(arg, layout, resolve));
    return out;
}

std::optional<std::string> UnkeyableArgument(std::span<std::string const> args, PathCanon::Layout const& layout)
{
    for (auto const& arg: args)
    {
        auto const portion = PathPortionOf(arg, layout);
        // Reported as the WHOLE argument rather than as the isolated path: it is
        // what an operator has to find on their own command line to act on the
        // refusal, and `/IC:foo` says which flag carries it while `C:foo` does not.
        if (portion.has_value() && IsDriveRelativeUnderNoRoot(portion->path, layout))
            return std::string { arg };
    }
    return std::nullopt;
}

std::string CacheCompilerId(std::string_view banner, std::string_view targetTriple)
{
    if (targetTriple.empty())
        return std::string { banner };
    return std::format("{}\n{}", banner, targetTriple);
}

std::string ComputeKey(KeyInputs const& inputs)
{
    // The schema tag comes first, and bumping it is what makes a format change
    // safe. Nothing else in this key describes how the stored value is framed or
    // canonicalized, so without it a change to either would leave old entries
    // matching new keys and being served under rules they were not written by --
    // a silent mis-serve, which presents as a mysterious hit-rate collapse rather
    // than as a miss. Bumping re-keys the cache instead, so stale entries simply
    // miss and are rewritten. ComputeManifestKey's tag moves in lock-step; see
    // the reason recorded there.
    //
    // v2 added the dependency path set, which is what makes a moved header a
    // different key rather than a hit whose depfile has to be re-checked (see
    // KeyInputs::dependencyPaths).
    //
    // v3 is the digest itself (issue #63). Until it, this was four CRC32C runs
    // over one blob distinguished only by a leading salt byte. CRC is affine over
    // GF(2): with `A` the per-byte state-update operator and `S_i` the state
    // after salt `i`, quarter_i XOR quarter_j is `A^len(blob) * (S_i XOR S_j)` --
    // a function of the blob's LENGTH and of nothing else about it. So matching
    // one quarter forced all four, and a key that was 128 bits wide carried 32
    // bits of strength. Equal length is not an exotic condition, it is the
    // ordinary shape of a source edit (`return 1;` -> `return 2;`), and the
    // consequence was not a miss but an unrelated translation unit's object file
    // served under a zero exit code. Measured before the fix: one distinct XOR
    // value across 2000 random equal-length blobs, and a full 32-hex-char
    // collision after 86,125 of them -- the birthday bound for the 32 bits it
    // really had, against ~10^5 entries a shared team cache reaches.
    //
    // v3 deliberately did NOT move when RelativizeArgs learned to tokenize a
    // FUSED object-output path, even though that changed the key of every
    // Windows build. That reasoning is kept, because it is the case for NOT
    // bumping and it stands on its own: the tag versions this CONSTRUCTION and
    // the rules the stored value is written under, and neither moved -- the
    // golden vector did not move for that change either, nor did the value's
    // framing or canonicalization. What changed was one input, for the builds
    // whose command line carried a machine-specific string it should never have
    // carried. Old entries stayed correct in their own terms; they simply
    // stopped being addressed, missed, and were rewritten. A bump could not be
    // reached from there -- an old key can only become a new key by a build
    // literally passing the text `<BUILDTREE>`, and a `/Fo` path that was
    // already relative canonicalizes to itself and does not move at all --
    // while it WOULD have invalidated every POSIX entry, where nothing changed.
    // Direct mode needed no bump for the same reason and not by luck:
    // ComputeManifestKey takes the relativized args too, so a manifest key moves
    // exactly where an object key does, in lock-step, for exactly the builds
    // affected.
    //
    // v4 is issue #64. A dependency path is now classified by what it RESOLVES to
    // rather than by whether it was spelled absolutely, so a relative one tokenizes
    // or drops where it used to be hashed verbatim.
    //
    // On its own that would not have required a bump, by exactly the argument
    // above: it re-keys only the translation units that reported such a path, and
    // their keys differ by construction, so a pre-fix entry becomes unreachable
    // rather than servable under rules it was not written by. The bump is taken
    // anyway, and the reason is worth stating precisely because it is NOT the
    // lock-step rule this comment used to cite. `manifest-v4` moved separately,
    // ahead of this, when BuildManifest learned to resolve a relative dependency
    // instead of dropping it -- and that direction of the coupling costs nothing,
    // as ComputeManifestKey records: an unreachable manifest is re-recorded on the
    // next compile. So nothing forced `objkey` here.
    //
    // What decides it is that the two tags disagreeing is a state nobody should
    // have to reason about later. A cache holding v4 manifests that point at v3
    // objects is correct today and is one careless edit from not being, and the
    // cost of removing the question is a single cold rebuild. The fused-output
    // re-key above, which needed no bump of its own, rides along in the same
    // invalidation event rather than ever costing a second.
    //
    // v5 is issue #111, and NOTHING here forced it. The manifest half is what the
    // issue is about — a manifest recorded before #104's refusal keeps direct-
    // hitting a drive-relative compile, and only a re-key retires it; see
    // ComputeManifestKey. This key's construction, the value's framing and its
    // canonicalization are all unmoved, and the golden vector below moved for the
    // tag and for nothing else.
    //
    // It is taken because the paragraph above is not a remark about v4, it is the
    // standing rule, and it decides this case the same way: two tags sitting
    // numerically apart is a state nobody should have to reason about later. A
    // cache holding v5 manifests that point at v4 objects is correct today under
    // the one-way lock-step and is one careless edit from not being, and the cost
    // of removing the question is again a single cold rebuild — one that the
    // manifest bump has already committed every user to paying, so the object half
    // rides in that invalidation event rather than ever costing a second.
    //
    // Issue #111 recommends the opposite — bump `manifest` alone, on the ground
    // that re-keying objects invalidates every entry on every platform for a
    // clang-cl-only exposure — and asks for that recommendation to be followed or
    // explicitly overruled here rather than silently ignored. It is overruled. The
    // asymmetry it cites is real but it is an argument about THIS invalidation
    // event, weighed once; the tags-apart hazard is carried by every reader of
    // this file afterwards, and unlike a cold rebuild it does not expire.
    // v6 retires every value stored before #319. A compile node served this wire
    // without canonicalizing a stored value's text regions, so those values carry
    // the producing checkout's absolute paths -- and a consumer replaying them
    // hands its build system dependencies naming a tree it will never edit, which
    // no later build can invalidate. The bytes are wrong rather than merely old,
    // and nothing in a value says which kind it is, so the tag is what makes them
    // unreachable. Bumped rather than sniffed: a reader that tried to tell a
    // poisoned region from a sound one would have to call a path with no
    // `<SRCROOT>` sentinel suspect, and 92 of the 93 paths a trivial translation
    // unit reports are toolchain headers that correctly have none.
    KeyDigest digest { "objkey-v6" };
    digest.Field(inputs.compilerId);
    digest.Field(inputs.preprocessed);
    for (auto const& arg: inputs.relativizedArgs)
        digest.Item(arg);
    for (auto const& path: inputs.dependencyPaths)
        digest.Path(path);
    return digest.ToHex();
}

} // namespace FastCache::Cc
