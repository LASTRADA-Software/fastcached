// SPDX-License-Identifier: Apache-2.0
#include "DependencyProbe.hpp"
#include "DirectManifest.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

namespace
{
    /// Unify a path's separators without deciding anything else about it.
    ///
    /// std::filesystem recognises `\` as a separator only on a Windows HOST, while
    /// every test here is the LAYOUT's — "derived from the LAYOUT, never from the
    /// host", as PathCanon.hpp puts it. Folding first is what lets one code path
    /// answer for both conventions on either host.
    ///
    /// Emitting `/` costs nothing downstream: `IsToolchainHeader` and
    /// `PathCanon::Canonicalize` both compare through separator-insensitive forms,
    /// and a canonical token's tail is forward-slashed whatever it was built from.
    ///
    /// @param raw A path as the compiler spelled it.
    /// @return The same path with `/` separators.
    [[nodiscard]] std::string Folded(std::string_view raw)
    {
        std::string out { raw };
        std::ranges::replace(out, '\\', '/');
        return out;
    }

    /// Collapse an already-folded path's `.`/`..` segments, host-neutrally.
    ///
    /// The lexical pass is DirectManifest's `NormalizePath`, for the reason it
    /// states: a driver echoes a path as *resolved*, so `.`/`..` segments arrive
    /// verbatim, and skipping the collapse makes `<BUILDTREE>/../inc/a.hpp` and
    /// `<SRCROOT>/inc/a.hpp` two key entries for one header — or, across two
    /// machines whose generators spell an include directory differently, two keys
    /// for identical content. A `..` that survived would also reach Canonicalize
    /// still prefix-matching a root, producing a token naming a file the path does
    /// not name.
    ///
    /// The result is re-folded because the pass emits the HOST's preferred
    /// separator, so a path normalized on Windows comes back backslash-separated —
    /// where `IsAbsoluteForLayout`, which for a POSIX layout asks only about a
    /// leading `/`, would read an absolute toolchain path as relative.
    ///
    /// A leading `//` is carried across the pass by hand. Two leading separators
    /// are a UNC root (`\\server\share`), which a Windows host preserves as a root
    /// name and a POSIX host collapses to a single `/` — measured, not assumed. A
    /// UNC-rooted layout normalized on POSIX would therefore stop prefix-matching
    /// its own root, classify every project header as toolchain, and hand back an
    /// empty dependency set: precisely the host-dependent key this function exists
    /// to rule out. Exactly two, because three or more is not a UNC root on either
    /// platform.
    ///
    /// @param folded A path whose separators are already `/`.
    /// @return The lexically normalized path, `/`-separated.
    [[nodiscard]] std::string LexicalForm(std::string_view folded)
    {
        bool const uncRoot = folded.starts_with("//") && !folded.starts_with("///");
        auto path = Folded(NormalizePath(folded));
        if (uncRoot && !path.starts_with("//"))
            path.insert(path.begin(), '/');
        return path;
    }

    /// Join a working directory and a relative path, without normalizing either.
    ///
    /// Lexical and layout-shaped: `std::filesystem::path::operator/` applies the
    /// RUNNING machine's rules to paths that belong to a cache shared with machines
    /// which do not share them — on a POSIX host a whole `C:\src\proj\build` is one
    /// relative component with no root at all, and on either host the separator it
    /// inserts is the host's preferred one rather than the layout's. Both operands
    /// are folded already, so a separator-joined concatenation is the whole
    /// operation; the caller normalizes the result once.
    ///
    /// @param workingDirectory The directory relative paths resolve against.
    /// @param relative         A folded relative path.
    /// @return The joined path, still folded and not yet normalized.
    [[nodiscard]] std::string JoinedWith(std::string_view workingDirectory, std::string_view relative)
    {
        std::string joined { workingDirectory };
        if (!joined.empty() && joined.back() != '/')
            joined.push_back('/');
        joined += relative;
        return joined;
    }

    /// One reported path's outcome: what became of it, and its token if it was kept.
    struct Classification
    {
        PathDisposition disposition; ///< Why it was kept, or why it was not.
        std::string token;           ///< The form to hash; empty unless `disposition` is Keyed.
    };

    /// Drop a path, naming the reason so the launcher's note can report it.
    /// @param disposition Why this path does not reach the key.
    /// @return The classification.
    [[nodiscard]] Classification Dropped(PathDisposition disposition)
    {
        return { .disposition = disposition, .token = {} };
    }

    /// The canonical token for a path under one of the layout's roots.
    ///
    /// Spelled once because two questions need it and they must not answer
    /// differently: what to hash, and — for a drive-relative path — whether the
    /// layout can place it at all. `Canonicalize` returns its input verbatim for a
    /// path it did not rewrite, so inequality is what says a token was produced —
    /// and it is the only test there is, nothing in PathCanon being able to fail.
    ///
    /// @param path   A normalized, `/`-separated path.
    /// @param layout This machine's roots.
    /// @return The token, or nothing when the path lies under no root.
    [[nodiscard]] std::optional<std::string> RootToken(std::string const& path, PathCanon::Layout const& layout)
    {
        auto canon = PathCanon::Canonicalize(path, layout);
        if (canon != path)
            return canon;
        return std::nullopt;
    }

    /// The portable form of one raw dependency path, or the reason this machine
    /// must not hash it. See the header for why each branch is load-bearing.
    ///
    /// Every `return` names a PathDisposition rather than merely yielding nothing.
    /// The branches were always distinct; only the reporting collapsed them, which
    /// is what left `0 of M` unable to tell a short-name root (issue #66) from an
    /// unanchorable path (issue #65) — see PathDisposition (issue #105).
    ///
    /// @param raw              A dependency path as the compiler spelled it.
    /// @param layout           This machine's roots.
    /// @param workingDirectory What a relative path resolves against.
    /// @return The form to hash, or the reason the path was dropped.
    [[nodiscard]] Classification PortableForm(std::string_view raw,
                                              PathCanon::Layout const& layout,
                                              std::string_view workingDirectory)
    {
        if (raw.empty())
            return Dropped(PathDisposition::Empty);

        auto folded = Folded(raw);

        // Classified before the toolchain test, because that test reports every
        // path outside the roots as toolchain — a relative one included.
        //
        // Asked BEFORE the lexical pass, which is both cheaper (the relative branch
        // normalizes once, after the join, rather than on each side of it) and the
        // more faithful answer. Collapsing `.`/`..` cannot give a path an anchor or
        // take one away — except in one case, where doing it first gets the answer
        // wrong: `C:/../x` normalizes to a bare `x` on a POSIX host, because `C:` is
        // an ordinary filename there for `..` to eat. Windows cannot ascend past a
        // drive root and would keep `C:/x`, so a path this machine must classify as
        // Absolute would have been resolved against the working directory instead.
        // Measured on libc++, and the same shape of host coupling as the UNC root
        // above.
        //
        // Kept rather than merely branched on, because it is also half of what the
        // note reports a later drop AS: see the toolchain branch below.
        auto const anchor = PathCanon::AnchorForLayout(folded, layout);
        switch (anchor)
        {
            case PathCanon::Anchor::WorkingDirectory:
                // RESOLVED, never kept for its spelling: the tests below ask what a
                // path NAMES, and its spelling answers a different question — one
                // that inverts for a vendored toolchain reached through a relative
                // include path (issue #64, and the header for the whole argument).
                folded = JoinedWith(workingDirectory, folded);
                // Still anchored to a working directory after the join: the caller
                // had none to give, or gave a relative one. A path this machine
                // cannot classify is a path it cannot hash portably, so it is
                // dropped rather than guessed at. Tested on the join rather than on
                // `workingDirectory` alone, so one rule covers an empty directory
                // and a relative one alike.
                if (PathCanon::AnchorForLayout(folded, layout) == PathCanon::Anchor::WorkingDirectory)
                    return Dropped(PathDisposition::Unanchored);
                break;
            case PathCanon::Anchor::DriveRelative:
            case PathCanon::Anchor::Absolute:
                // Both fall through to the root tests below, and for a
                // drive-relative path that is a decision rather than an oversight.
                // What must never happen is the branch above: `C:foo` resolves
                // against drive C's current directory — per-process state on the
                // producing machine that no cache entry records — so joining it to
                // the compile's working directory, as that branch does to a
                // genuinely relative path, would produce a path naming a file that
                // was never read. Issue #65 records this as the reason the anchor is
                // three-valued rather than a tightened boolean.
                //
                // Beyond that, root membership is the stronger test and is left to
                // decide: a drive-relative path under no root cannot prefix-match a
                // rooted root (`c:foo/...` against `c:/src`), so it is dropped
                // regardless — while one under a *drive-relative* root canonicalizes
                // to a token that is portable precisely because the consumer
                // substitutes its own root. Returning early here would silently drop
                // that second case, which was kept before issue #65.
                break;
        }

        // One lexical pass, whichever branch produced the text.
        auto const path = LexicalForm(folded);

        bool const driveRelative = anchor == PathCanon::Anchor::DriveRelative;

        // The classifier the manifest and the replay guard already use, so all
        // three agree on what "toolchain" means — including a vcpkg tree nested
        // under the build tree, which canonicalizes but is still the producing
        // machine's spelling of content the compiler identity already covers. No
        // path reaching here is anchored to a working directory any more, whichever
        // branch it came down — which is the case about which the three genuinely do
        // agree (see the header for where they part on the ones that are).
        if (IsToolchainHeader(path, layout))
        {
            // Which of two true things a dropped drive-relative path is REPORTED
            // as, and the root question is what decides. Such a path is anchored to
            // a drive's own current directory — per-process state on the producing
            // machine, portable to nothing — and it may also be content the
            // toolchain stamp covers. Under NO root only the first says what to
            // change, and calling it toolchain there is precisely the collapse
            // issue #105 exists to undo: that is the word a short-name root
            // produces too, so the two faults would keep rendering as one line.
            // Under a drive-relative root the path canonicalizes, so a marker match
            // is ordinary vendored content and toolchain is the true answer —
            // reporting THAT as drive-relative would be the loudest possible
            // reading of this vocabulary on a healthy build, which is the same
            // defect from the other side. `IsToolchainHeader` cannot separate the
            // two on its own (it tests its markers before any root, deliberately),
            // and splitting it is not worth a fourth spelling of the toolchain rule
            // — so the root question is asked here instead, and only for the
            // drive-relative path no ordinary build produces at all. The toolchain
            // drop is 476 of a real translation unit's 635 paths and still costs
            // exactly one root test.
            if (driveRelative && !RootToken(path, layout).has_value())
                return Dropped(PathDisposition::DriveRelative);

            // A root spelled almost right, asked as its own question rather than
            // inferred from two classifiers disagreeing. It used to be the latter:
            // `IsToolchainHeader` matched a root character-wise while `Canonicalize`
            // matched segment-wise, so `/x/build-other/a.h` fell out of the gap
            // between them and was reported here by arriving at the bottom of this
            // function. That gap was a violation of the invariant the three
            // classifiers exist to keep (issue #562) and is closed; the fault it
            // happened to name is not, because it is the one root fault of the three
            // an operator repairs by editing a root rather than by moving a file.
            // Explicit is also cheaper to reason about: it now says what it means
            // instead of meaning whatever two predicates happen to disagree about.
            if (IsNearMissRoot(path, layout))
                return Dropped(PathDisposition::Uncanonical);
            return Dropped(PathDisposition::Toolchain);
        }

        if (auto token = RootToken(path, layout); token.has_value())
            return { .disposition = PathDisposition::Keyed, .token = *std::move(token) };

        // `IsToolchainHeader` called this path rooted and `Canonicalize` then
        // produced nothing, which the shared predicate leaves no way to reach: since
        // issue #562 both ask `PathCanon::IsUnderRoot`, and the only remaining
        // difference is the markers, which cannot make a path MORE rooted. Answered
        // rather than asserted because the alternative to answering is keying a path
        // with no portable form -- the producing machine's absolute spelling in the
        // key -- and that is the one outcome here worth being total about.
        return Dropped(driveRelative ? PathDisposition::DriveRelative : PathDisposition::Toolchain);
    }
} // namespace

ProbeText SplitIncludeNotes(std::string_view text)
{
    ProbeText out;
    out.preprocessed.reserve(text.size());

    std::size_t offset = 0;
    while (offset < text.size())
    {
        auto lineEnd = text.find('\n', offset);
        auto const terminated = lineEnd != std::string_view::npos;
        if (!terminated)
            lineEnd = text.size();

        // The line WITH its terminator, so a non-note is reproduced byte-for-byte.
        auto const whole = text.substr(offset, (terminated ? lineEnd + 1 : lineEnd) - offset);
        auto const line = text.substr(offset, lineEnd - offset);
        offset = terminated ? lineEnd + 1 : text.size();

        // Recognition is IncludeNotePath's, shared with ParseIncludePaths, and it
        // is anchored: this stream also carries preprocessed SOURCE, so a rule
        // that matched the marker mid-line would delete an ordinary line holding
        // that text from the bytes the key is computed over.
        auto const path = IncludeNotePath(line);
        if (!path.has_value())
        {
            out.preprocessed += whole;
            continue;
        }

        if (!path->empty())
            out.notePaths.emplace_back(*path);
    }
    return out;
}

bool IsDriveRelativeUnderNoRoot(std::string_view path, PathCanon::Layout const& layout)
{
    // Asked before anything is canonicalized, so a layout that cannot produce this
    // anchor at all — every POSIX one — pays a single character comparison.
    //
    // A switch without a `default:`, as PortableForm's, IsCheckable's and
    // ClassifyResolved's each are, so a fourth Anchor state is a compile error here
    // rather than a silent `false` — which for this function means silently
    // re-opening the stale-depfile serve it exists to close.
    switch (PathCanon::AnchorForLayout(path, layout))
    {
        case PathCanon::Anchor::WorkingDirectory:
            // Resolves against the compile's working directory, which is also the
            // launcher's, so it is placeable and every other rule covers it.
        case PathCanon::Anchor::Absolute:
            // Names a fixed location: keyed under a root, and dropped as toolchain
            // outside both — the deliberate trade the compiler identity covers.
            return false;
        case PathCanon::Anchor::DriveRelative:
            break;
    }

    // RootToken, not a second spelling of the same Canonicalize-and-compare: this
    // is the SAME question PortableForm asks one line above its own
    // `PathDisposition::DriveRelative` return, and two spellings of it are two
    // places for the rule to drift. What differs is only which input each is handed
    // — a reported dependency path there, a command-line argument here.
    //
    // The path is NOT put through LexicalForm first, unlike PortableForm's. That
    // matters only for an argument carrying `..` segments that would collapse into
    // a root (`-IC:foo\..\src`), where this refuses a compile the root would have
    // placed. Conservative in the safe direction, and unreachable without a
    // drive-relative argument that also needs normalizing.
    return !RootToken(std::string { path }, layout).has_value();
}

std::string DescribeDropped(DependencySet const& set)
{
    std::string out;
    for (auto const& row: DispositionTable)
    {
        // `Keyed` is what the note's own `N of M` prefix already reports, and a
        // reason that counted nothing is a reason this compile did not have — a
        // healthy build should say what happened, not enumerate what did not.
        if (row.disposition == PathDisposition::Keyed)
            continue;
        auto const count = set.Count(row.disposition);
        if (count == 0)
            continue;
        if (!out.empty())
            out += ", ";
        out += std::format("{} {}", count, row.label);
    }
    return out;
}

DependencySet KeyDependencySet(std::span<std::string const> rawPaths,
                               PathCanon::Layout const& layout,
                               std::string_view workingDirectory)
{
    // Folded and normalized once, not per path: it arrives in whatever form the
    // caller's platform spells a current directory, and the join below is a byte
    // concatenation that assumes both sides already agree on a separator.
    auto const base = LexicalForm(Folded(workingDirectory));

    DependencySet out;
    out.keyed.reserve(rawPaths.size());
    for (auto const& raw: rawPaths)
    {
        auto classified = PortableForm(raw, layout, base);
        // Tallied per reported OCCURRENCE, before the deduplication below, so the
        // whole tally sums to what the probe reported and the note's reasons
        // account for every path it counted.
        ++out.tally[static_cast<std::size_t>(classified.disposition)];
        if (classified.disposition == PathDisposition::Keyed)
            out.keyed.push_back(std::move(classified.token));
    }

    // Byte-wise, so the order is a property of the data rather than of the
    // machine's locale — two machines must produce the same key from the same set.
    std::ranges::sort(out.keyed);
    auto const duplicates = std::ranges::unique(out.keyed);
    out.keyed.erase(duplicates.begin(), duplicates.end());
    return out;
}

} // namespace FastCache::Cc
