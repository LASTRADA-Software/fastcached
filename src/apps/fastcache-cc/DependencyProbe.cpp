// SPDX-License-Identifier: Apache-2.0
#include "DependencyProbe.hpp"
#include "DirectManifest.hpp"

#include <algorithm>
#include <cstddef>
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

    /// The portable form of one raw dependency path, or nothing when this machine
    /// must not hash it. See the header for why each branch is load-bearing.
    /// @param raw              A dependency path as the compiler spelled it.
    /// @param layout           This machine's roots.
    /// @param workingDirectory What a relative path resolves against.
    /// @return The form to hash, or an empty string to drop the path.
    [[nodiscard]] std::string PortableForm(std::string_view raw,
                                           PathCanon::Layout const& layout,
                                           std::string_view workingDirectory)
    {
        if (raw.empty())
            return {};

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
        switch (PathCanon::AnchorForLayout(folded, layout))
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
                    return {};
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
                // rooted root (`c:foo/...` against `c:/src`), so it is dropped as
                // toolchain anyway — while one under a *drive-relative* root
                // canonicalizes to a token that is portable precisely because the
                // consumer substitutes its own root. Returning early here would
                // silently drop that second case, which was kept before issue #65.
                break;
        }

        // One lexical pass, whichever branch produced the text.
        auto const path = LexicalForm(folded);

        // The classifier the manifest and the replay guard already use, so all
        // three agree on what "toolchain" means — including a vcpkg tree nested
        // under the build tree, which canonicalizes but is still the producing
        // machine's spelling of content the compiler identity already covers. No
        // path reaching here is anchored to a working directory any more, whichever
        // branch it came down — which is the case about which the three genuinely do
        // agree (see the header for where they part on the ones that are).
        if (IsToolchainHeader(path, layout))
            return {};

        // Canonicalize returns its input verbatim for a path it did not rewrite,
        // so inequality — not the spelling of a sentinel PathCanon keeps private —
        // is what says a token was produced.
        if (auto canon = PathCanon::Canonicalize(path, layout); canon.has_value() && *canon != path)
            return *std::move(canon);
        return {};
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

std::vector<std::string> KeyDependencySet(std::span<std::string const> rawPaths,
                                          PathCanon::Layout const& layout,
                                          std::string_view workingDirectory)
{
    // Folded and normalized once, not per path: it arrives in whatever form the
    // caller's platform spells a current directory, and the join below is a byte
    // concatenation that assumes both sides already agree on a separator.
    auto const base = LexicalForm(Folded(workingDirectory));

    std::vector<std::string> out;
    out.reserve(rawPaths.size());
    for (auto const& raw: rawPaths)
        if (auto portable = PortableForm(raw, layout, base); !portable.empty())
            out.push_back(std::move(portable));

    // Byte-wise, so the order is a property of the data rather than of the
    // machine's locale — two machines must produce the same key from the same set.
    std::ranges::sort(out);
    auto const duplicates = std::ranges::unique(out);
    out.erase(duplicates.begin(), duplicates.end());
    return out;
}

} // namespace FastCache::Cc
