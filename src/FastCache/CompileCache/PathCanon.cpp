// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::PathCanon
{
namespace
{

    constexpr std::string_view SrcRootSentinel = "<SRCROOT>";
    constexpr std::string_view BuildTreeSentinel = "<BUILDTREE>";

    /// Either separator, because a compiler emits both and often in one path.
    [[nodiscard]] constexpr bool IsPathSeparator(char c) noexcept
    {
        return c == '/' || c == '\\';
    }

    /// True when `text` carries `..` as a whole path SEGMENT.
    ///
    /// The distinction is the point: `D:\src\a..b\x.hpp` contains `..` and has no `..` segment, so
    /// a `contains("..")` test would send it down the slow path and hand back a re-separated
    /// spelling for no reason. Both ends must land on a boundary.
    [[nodiscard]] bool HasDotDotSegment(std::string_view text)
    {
        auto at = text.find("..");
        while (at != std::string_view::npos)
        {
            auto const after = at + 2;
            bool const opensSegment = at == 0 || IsPathSeparator(text[at - 1]);
            bool const closesSegment = after == text.size() || IsPathSeparator(text[after]);
            if (opensSegment && closesSegment)
                return true;
            at = text.find("..", at + 1);
        }
        return false;
    }

    /// The lexical normal form of a `/`-only path: empty and `.` segments dropped, and each `..`
    /// taking the segment before it. A `..` with nothing before it is dropped on an absolute path
    /// (nothing ascends past a root) and kept on a relative one, where the caller then sees it
    /// survive. A trailing separator survives, as `lexically_normal` keeps one.
    /// @param tail A path whose separators are all `/`, its anchor already split off.
    /// @return The collapsed form.
    [[nodiscard]] std::string LexicallyCollapsed(std::string_view tail)
    {
        bool const absolute = tail.starts_with('/');
        bool const trailing = tail.size() > 1 && tail.ends_with('/');
        std::vector<std::string_view> kept;
        for (auto const segment: std::views::split(tail, '/'))
        {
            std::string_view const name { segment.begin(), segment.end() };
            if (name.empty() || name == ".")
                continue;
            if (name != "..")
                kept.push_back(name);
            else if (!kept.empty() && kept.back() != "..")
                kept.pop_back();
            else if (!absolute)
                kept.push_back(name);
        }

        std::string out { absolute ? "/" : "" };
        for (auto const& name: kept)
        {
            if (!out.empty() && out.back() != '/')
                out.push_back('/');
            out.append(name);
        }
        if (out.empty())
            return ".";
        if (trailing && out.back() != '/')
            out.push_back('/');
        return out;
    }

    /// Build the comparison form of a path: separators normalized to '/' and,
    /// on Windows, lower-cased. Used only for prefix matching, never emitted.
    /// @param path Native-form path.
    /// @return The comparison-form string.
    [[nodiscard]] std::string ComparisonForm(std::string_view path)
    {
        std::string out;
        out.reserve(path.size());
        for (char const c: path)
            out.push_back(AsciiLower(c == '\\' ? '/' : c));
        return out;
    }

    /// The POSIX tail of `original` after stripping `rootLen` leading bytes:
    /// separators normalized to '/', a leading separator dropped. Preserves the
    /// original bytes' case (only the comparison used lower-case).
    /// @param original Native-form path.
    /// @param rootLen  Number of leading bytes belonging to the matched root.
    /// @return The normalized relative tail.
    [[nodiscard]] std::string PosixTail(std::string_view original, std::size_t rootLen)
    {
        std::string_view tail = original.substr(rootLen);
        while (!tail.empty() && (tail.front() == '\\' || tail.front() == '/'))
            tail.remove_prefix(1);
        std::string out;
        out.reserve(tail.size());
        for (char const c: tail)
            out.push_back(c == '\\' ? '/' : c);
        return out;
    }

    /// True when `rootCmp` (comparison form) is a segment-boundary prefix of
    /// `pathCmp` (comparison form).
    ///
    /// A trailing separator on the root is handled here rather than assumed away.
    /// This summary used to promise one absent, which was a precondition no caller
    /// enforced: the daemon and the node take these roots straight off a STORE frame
    /// and `RootReconciler` deliberately exempts a bare root from trimming, so `/`
    /// and `C:\` arrive with one and always did.
    /// @param pathCmp Comparison form of the candidate path.
    /// @param rootCmp Comparison form of a root.
    /// @return Whether the root prefixes the path on a segment boundary.
    [[nodiscard]] bool IsSegmentPrefix(std::string_view pathCmp, std::string_view rootCmp)
    {
        if (rootCmp.empty() || pathCmp.size() < rootCmp.size())
            return false;
        if (!pathCmp.starts_with(rootCmp))
            return false;
        // A root that ENDS in a separator is its own segment boundary, so demanding
        // another one after it asks for a byte that cannot be there. `/` and `C:\`
        // are the whole of that class -- a bare root IS its trailing separator -- and
        // before this they matched nothing at all: every path under them was judged
        // to lie outside both roots, so the stored value kept the producing machine's
        // absolute paths, silently, which is #229/#319 for any build rooted at the
        // filesystem root (issue #547). An UNTRIMMED root (`/x/build/`) lands here
        // too and now behaves; `RootReconciler` still trims those, so this is a
        // second line rather than a substitute for the first.
        if (rootCmp.back() == '/')
            return true;

        // Exact match, or the next byte is a separator (segment boundary).
        //
        // **One separator on purpose, and not an oversight.** Both operands are
        // COMPARISON forms, and `ComparisonForm` folds `\` to `/`, so a backslash
        // cannot reach this line -- testing for one would be dead code that reads
        // like thoroughness. Anything unifying separator handling across the
        // predicates in this file must not reach here; #562 records the same caution
        // from the other direction, and #575 left this line alone deliberately while
        // hoisting the folded roots out of the per-span path.
        return pathCmp.size() == rootCmp.size() || pathCmp[rootCmp.size()] == '/';
    }

    /// The relation between two comparison forms, from one pass of the rule above.
    ///
    /// `NearMiss` is defined as the complement of `Under` over the same bytes rather
    /// than as a second opinion about them, so the two can never disagree about the
    /// boundary. An empty root is `Outside` of everything: it prefixes every path
    /// character-wise and is `Under` nothing, so without this it would make every
    /// path in a layout naming no build tree a near miss of that root.
    ///
    /// @param pathCmp Comparison form of the candidate path.
    /// @param rootCmp Comparison form of a root.
    /// @return Where the path stands relative to the root.
    [[nodiscard]] RootRelation RelateFolded(std::string_view pathCmp, std::string_view rootCmp)
    {
        if (rootCmp.empty() || !pathCmp.starts_with(rootCmp))
            return RootRelation::Outside;
        return IsSegmentPrefix(pathCmp, rootCmp) ? RootRelation::Under : RootRelation::NearMiss;
    }

    /// How deep a root reaches, for deciding which of two matching roots is the
    /// longer. A trailing separator adds a byte and no depth, so it must not decide
    /// the question: `/x/proj` and `/x/proj/` name one directory, and comparing raw
    /// sizes made the second beat the first on that byte alone -- so an in-source
    /// layout spelling its two roots differently sent every path to `<BUILDTREE>`,
    /// and a consumer with a real out-of-source layout then replayed a path that does
    /// not exist. Reachable only from a client that does not trim, which the daemon
    /// and the node are, since they take these roots straight off a STORE frame.
    ///
    /// Two roots of equal depth remain a tie, resolved where the tie-break is: the
    /// build tree wins, which is the rule for a build tree nested at the source root.
    ///
    /// @param rootCmp Comparison form of a root.
    /// @return Its length, less one trailing separator.
    [[nodiscard]] constexpr std::size_t RootDepth(std::string_view rootCmp) noexcept
    {
        return (!rootCmp.empty() && rootCmp.back() == '/') ? rootCmp.size() - 1 : rootCmp.size();
    }

    /// The comparison forms of a layout's two roots.
    ///
    /// A `Layout`'s roots are fixed for the life of an operation while the path
    /// varies per span, so these belong to the CALLER's scope rather than to
    /// `CanonicalizeOne`, which is invoked once per path span. Rebuilt per span they
    /// cost two heap allocations and two transform loops each time to produce the
    /// same two strings: a translation unit with 100 `/showIncludes` lines pays
    /// roughly 200 avoidable allocations for a value that never changes.
    ///
    /// **Threaded down rather than memoized**, which is what keeps this a change of
    /// scope rather than of lifetime: there is no cache to invalidate, no hidden
    /// static, and no clock — the value simply lives where it is invariant. That is
    /// the shape `AGENT.md`'s caching principle asks for when the cheaper answer is
    /// to compute once rather than to remember.
    struct FoldedRoots
    {
        std::string sourceRoot; ///< Comparison form of `Layout::sourceRoot`.
        std::string buildTree;  ///< Comparison form of `Layout::buildTree`.
    };

    /// Fold a layout's roots into their comparison forms, once.
    /// @param layout Producing machine's roots.
    /// @return Both roots' comparison forms.
    [[nodiscard]] FoldedRoots FoldRoots(Layout const& layout)
    {
        return FoldedRoots { .sourceRoot = ComparisonForm(layout.sourceRoot),
                             .buildTree = ComparisonForm(layout.buildTree) };
    }

    /// Rewrite a single native path to a token. Longest matching root wins.
    /// @param absolutePath Native-form path.
    /// @param layout       Producing machine's roots.
    /// @param folded       @p layout's roots already in comparison form.
    /// @return The token, or the input verbatim when under neither root.
    [[nodiscard]] std::string CanonicalizeOne(std::string_view absolutePath, Layout const& layout, FoldedRoots const& folded)
    {
        // Only this one varies per call; the roots arrive already folded.
        std::string const pathCmp = ComparisonForm(absolutePath);
        std::string_view const srcCmp = folded.sourceRoot;
        std::string_view const buildCmp = folded.buildTree;

        bool const srcMatch = IsSegmentPrefix(pathCmp, srcCmp);
        bool const buildMatch = IsSegmentPrefix(pathCmp, buildCmp);

        // Longest root wins so a build tree nested under the source root maps to
        // <BUILDTREE>, not <SRCROOT>. Depth rather than size -- see RootDepth.
        if (buildMatch && (!srcMatch || RootDepth(buildCmp) >= RootDepth(srcCmp)))
            return std::string { BuildTreeSentinel } + '/' + PosixTail(absolutePath, layout.buildTree.size());
        if (srcMatch)
            return std::string { SrcRootSentinel } + '/' + PosixTail(absolutePath, layout.sourceRoot.size());
        return std::string { absolutePath };
    }

    /// True when `path` opens with a drive specifier (`C:`), whatever follows it.
    /// @param path A path or layout root in native form.
    /// @return True when bytes 0 and 1 are a drive letter and a colon.
    [[nodiscard]] constexpr bool HasDriveSpecifier(std::string_view path) noexcept
    {
        return path.size() >= 2 && path[1] == ':' && IsDriveLetter(path.front());
    }

    /// True when a separator follows the drive specifier — the byte that decides
    /// whether `C:...` is rooted at the drive (`C:\x`) or at the drive's own
    /// current directory (`C:x`). Asked separately by both callers because they
    /// treat a specifier with *no* tail at all (a bare `C:`) differently.
    ///
    /// @param path A path or layout root already known to carry a specifier.
    /// @return True when byte 2 exists and is `/` or `\`.
    [[nodiscard]] constexpr bool DriveTailIsSeparator(std::string_view path) noexcept
    {
        return path.size() > 2 && (path[2] == '/' || path[2] == '\\');
    }

    /// True when `root` is a Windows-shaped path root: backslash-separated, or
    /// prefixed with a drive specifier (`C:` / `C:/...`).
    ///
    /// The drive test is deliberately narrow — an ASCII letter, a colon, and
    /// then either end-of-string or a separator. Both halves matter:
    ///
    /// - Without the letter check, any root whose second byte is a colon reads
    ///   as Windows.
    /// - Without the separator check, a relative POSIX root like `a:b/proj`
    ///   still reads as Windows, because `a` is a letter and `:` sits at index 1.
    ///
    /// Either mistake makes a POSIX layout look like Windows, which turns every
    /// leading `/` into an "option" and leaves absolute paths — and so the
    /// checkout location — baked into the cache key.
    ///
    /// A bare `C:` is accepted, unlike in AnchorForLayout: as a layout ROOT that
    /// is the degenerate spelling of the drive root, while as a PATH the same
    /// bytes name the drive's current directory. Same rule, different question.
    ///
    /// @param root A layout root in native form.
    /// @return True when the root uses Windows path conventions.
    [[nodiscard]] constexpr bool IsWindowsRoot(std::string_view root) noexcept
    {
        if (root.contains('\\'))
            return true;
        if (!HasDriveSpecifier(root))
            return false;
        return root.size() == 2 || DriveTailIsSeparator(root);
    }

    /// The separator a localized path should use. Taken from the consuming
    /// layout's own root rather than from the host OS: a cache is shared across
    /// machines, so a Windows consumer layout must localize to backslashes even
    /// when this code runs on POSIX (and vice versa).
    ///
    /// This asks a narrower question than IsWindowsRoot: a `C:/src/proj` root is
    /// Windows, yet it spells its separators with forward slashes and localized
    /// paths must keep doing so. Only the actual separator in use decides here.
    ///
    /// @param root A layout root in native form.
    /// @return '\\' when the root uses backslashes, else '/'.
    [[nodiscard]] char SeparatorOf(std::string_view root) noexcept
    {
        return root.contains('\\') ? '\\' : '/';
    }

    /// Convert a POSIX tail to the separator style of the target layout.
    /// @param tail POSIX-form relative path.
    /// @param sep  The separator to emit, per SeparatorOf.
    /// @return The relative path in the target layout's separator style.
    [[nodiscard]] std::string ToNative(std::string_view tail, char sep)
    {
        std::string out;
        out.reserve(tail.size());
        for (char const c: tail)
            out.push_back(c == '/' ? sep : c);
        return out;
    }

    /// Join a layout root and a POSIX token tail into a localized path, in the
    /// root's own separator style.
    /// @param root Consuming layout root (native form).
    /// @param tail POSIX-form tail, already stripped of its leading separator.
    /// @return The localized path.
    [[nodiscard]] std::string JoinLocalized(std::string_view root, std::string_view tail)
    {
        char const sep = SeparatorOf(root);
        std::string out { root };

        // The same fact as the segment test above, on the way back: a root that ends
        // in a separator already carries the one this join would add. Appending
        // regardless produced `//inc/a.hpp` on POSIX -- implementation-defined rather
        // than merely ugly -- and `C:\\inc\a.hpp` on Windows, which parses as a
        // UNC path naming a host that does not exist (issue #547).
        //
        // Either separator counts, not just the one `SeparatorOf` picked: a root
        // mixing styles (`C:\src/`) reports `\` and ends with `/`, so testing only
        // against the reported one would append a second separator to it.
        //
        // The root is non-empty by `LocalizeOne`'s contract, which answers an empty
        // one by leaving the token standing rather than reaching here; the test below
        // protects `back()` and decides nothing.
        if (!out.empty() && out.back() != '/' && out.back() != '\\')
            out.push_back(sep);

        out += ToNative(tail, sep);
        return out;
    }

    /// Rewrite a single token back to a native path for `layout`.
    /// @param token  A token produced by CanonicalizeOne.
    /// @param layout Consuming machine's roots.
    /// @return The localized native path, or the token verbatim when it carries no
    ///         recognized sentinel.
    [[nodiscard]] std::string LocalizeOne(std::string_view token, Layout const& layout)
    {
        // Sentinel -> the root it localizes against. Adding a sentinel is a new row.
        struct SentinelRoot
        {
            std::string_view sentinel;
            std::string Layout::* root;
        };
        constexpr std::array<SentinelRoot, 2> Roots { {
            { .sentinel = SrcRootSentinel, .root = &Layout::sourceRoot },
            { .sentinel = BuildTreeSentinel, .root = &Layout::buildTree },
        } };

        for (auto const& [sentinel, root]: Roots)
        {
            if (!token.starts_with(sentinel))
                continue;

            // An EMPTY consuming root localizes to NOTHING, and the token is left
            // standing to say so. Joining against one produces a relative path, which
            // is the worst of the three answers available: it resolves against the
            // process's working directory and can name a different real file, silently,
            // where a surviving token is refused by `Cc::MissingReplayedDependency`
            // before anything is written. (Prefixing a separator instead, which is what
            // this did before #547, invents an absolute path out of no root at all.)
            //
            // Not reachable from the launcher -- `RunCached` returns on an empty
            // `srcRoot`/`buildTree` before a layout exists -- but this function takes a
            // `Layout` from whoever hands it one, and the producing side already answers
            // "under no root" for the same case rather than inventing a token.
            if ((layout.*root).empty())
                return std::string { token };

            std::string_view tail = token.substr(sentinel.size());
            if (!tail.empty() && tail.front() == '/')
                tail.remove_prefix(1);
            return JoinLocalized(layout.*root, tail);
        }
        return std::string { token };
    }

    // --- Region grammar --------------------------------------------------------

    /// One line split around its path span: text kept verbatim, the span, text kept
    /// verbatim. The span never includes a trailing '\r'; the tail does, so CRLF
    /// survives a round trip.
    struct LineSplit
    {
        std::string_view head; ///< Text before the path span.
        std::string_view path; ///< The path span.
        std::string_view tail; ///< Text after the path span, incl. any '\r'.
    };

    /// One SHAPE a line can have that locates a path span in it.
    ///
    /// A grammar is a list of these rather than one, because a stream can carry more
    /// than one line language: the MSVC family's console streams carry `/showIncludes`
    /// notes AND diagnostics, on either stream depending on the flag (#825), and a
    /// region carries exactly one grammar tag. So a grammar that could hold only one
    /// shape left the other language verbatim, with the producer's absolute paths in it.
    /// @param line One line without its newline, a trailing '\r' included.
    /// @param body The same line with that '\r' removed.
    /// @return The split, or nothing when the line is not this shape.
    using LineRule = std::optional<LineSplit> (*)(std::string_view line, std::string_view body);

    /// `<marker><blanks><path>`, the marker at column zero.
    ///
    /// `IncludeNoteMarker` rather than a literal of this file's own. A stored region
    /// carries the canonical marker BY CONTRACT -- the producer normalizes to it -- so
    /// the grammar and that contract are one constant, not two that happen to read alike.
    [[nodiscard]] std::optional<LineSplit> SplitIncludeNote(std::string_view line, std::string_view body)
    {
        std::size_t start = IncludeNoteMarkerEnd(body, IncludeNoteMarker);
        if (start == std::string_view::npos)
            return std::nullopt;
        while (start < body.size() && body[start] == ' ')
            ++start;
        if (start >= body.size())
            return std::nullopt;
        return LineSplit { .head = line.substr(0, start), .path = body.substr(start), .tail = line.substr(body.size()) };
    }

    /// The offset just past a run of ASCII digits starting at `from`.
    [[nodiscard]] std::size_t DigitsEnd(std::string_view text, std::size_t from) noexcept
    {
        while (from < text.size() && text[from] >= '0' && text[from] <= '9')
            ++from;
        return from;
    }

    /// What may follow an MSVC-family location's digits. `): ` is what the drivers write;
    /// `) : ` is the `#pragma message(__FILE__ "(" STR(__LINE__) ") : warning: ...")` idiom
    /// MSVC's own documentation gives, written so IDEs and problem matchers read it as a
    /// diagnostic head -- which is why a foreign path in it sends a developer to the wrong
    /// tree. Equally anchored and equally numeric, so it admits no source echo the tighter
    /// spelling rejects.
    constexpr std::array<std::string_view, 2> MsvcLocationEnds { "): ", ") : " };

    /// Whether the `(` at `open` begins an MSVC-family location: `(<digits>)` or
    /// `(<digits>,<digits>)` followed by one of `MsvcLocationEnds`, and nothing looser.
    [[nodiscard]] bool OpensMsvcLocation(std::string_view body, std::size_t open) noexcept
    {
        std::size_t end = DigitsEnd(body, open + 1);
        if (end == open + 1)
            return false;
        if (end < body.size() && body[end] == ',')
        {
            std::size_t const columnEnd = DigitsEnd(body, end + 1);
            if (columnEnd == end + 1)
                return false;
            end = columnEnd;
        }
        auto const rest = body.substr(end);
        return std::ranges::any_of(MsvcLocationEnds, [rest](std::string_view ending) { return rest.starts_with(ending); });
    }

    /// Whether the `:` at `colon` ends a GCC-family path: `:<digits>` followed by `:` or `,`.
    [[nodiscard]] bool EndsGccPath(std::string_view body, std::size_t colon) noexcept
    {
        std::size_t const digits = DigitsEnd(body, colon + 1);
        return digits > colon + 1 && digits < body.size() && (body[digits] == ':' || body[digits] == ',');
    }

    /// `<path>(<digits>[,<digits>]): ...`, the path at column zero -- the head `cl` and
    /// `clang-cl` both write, a warning, an error and a `note:` continuation alike.
    ///
    /// **Anchored, and tight, because the stream also carries SOURCE.** Under
    /// `/diagnostics:caret` a driver echoes the offending source line and a caret after
    /// the head, and this repository's code quotes paths. So the location must be exactly
    /// `(digits)` or `(digits,digits)` followed by `): ` or `) : `, and a parenthesis that
    /// does not open a location (`Program Files (x86)`, a call in the echoed code) is
    /// stepped over rather than taken as the end of the path.
    ///
    /// **What protects an indented echo is the ROOT match, not the column-zero check.**
    /// A span is rewritten only when it BEGINS with a root (`CanonicalizeOne`), and an
    /// indented line's span begins with its blanks, so it is left alone either way --
    /// removing the check turns nothing red (measured by the Job 2 review). The check is
    /// kept as the statement of what a head IS, and is defensive only. The one shape
    /// still rewritten is a source line whose own text begins, at column zero, with an
    /// in-root path followed by a location, which only a raw string or a comment
    /// continuation can produce.
    [[nodiscard]] std::optional<LineSplit> SplitMsvcDiagnostic(std::string_view line, std::string_view body)
    {
        if (body.empty() || body.front() == ' ' || body.front() == '\t')
            return std::nullopt;
        // From 1, so the path is never empty.
        std::size_t open = body.find('(', 1);
        while (open != std::string_view::npos && !OpensMsvcLocation(body, open))
            open = body.find('(', open + 1);
        if (open == std::string_view::npos)
            return std::nullopt;
        return LineSplit { .head = {}, .path = body.substr(0, open), .tail = line.substr(open) };
    }

    /// The GCC-family location suffix: the path starting at `begin` runs to the `:` that
    /// begins `:<digits>` followed by `:` or `,`.
    ///
    /// Searched left to right from `begin`, so a drive letter's colon cannot end it -- `C:`
    /// is not followed by digits-then-separator -- and a path containing a literal
    /// `:<digits>:` would have to do so before its real location suffix, which no compiler
    /// emits.
    [[nodiscard]] std::optional<LineSplit> SplitGccPathAt(std::string_view line, std::string_view body, std::size_t begin)
    {
        std::size_t colon = body.find(':', begin);
        while (colon != std::string_view::npos && colon != begin && !EndsGccPath(body, colon))
            colon = body.find(':', colon + 1);
        if (colon == std::string_view::npos || colon == begin)
            return std::nullopt;
        return LineSplit { .head = line.substr(0, begin),
                           .path = body.substr(begin, colon - begin),
                           .tail = line.substr(colon) };
    }

    /// The include-chain head `In file included from `, spelled once for both GCC rules.
    constexpr std::string_view IncludedFrom = "In file included from ";

    /// The GCC-family include chain: two ANCHORED shapes --
    ///
    ///     In file included from <path>:<line>[,:]  the chain's head
    ///                      from <path>:<line>[,:]  its continuations
    ///
    /// Its own rule because it is its own line language: `clang-cl` writes it on the MSVC
    /// family's stream too, in front of a header's diagnostic -- measured on the `clang-cl`
    /// VS 18 installs, `In file included from <root>\src\a.cpp:1:` above a `(3,29): warning`
    /// -- so `MsvcStream` needs this half and no other part of the GCC grammar.
    [[nodiscard]] std::optional<LineSplit> SplitGccIncludeChain(std::string_view line, std::string_view body)
    {
        constexpr std::string_view ContinuedFrom = "from ";
        if (body.starts_with(IncludedFrom))
            return SplitGccPathAt(line, body, IncludedFrom.size());
        if (body.empty() || body.front() != ' ')
            return std::nullopt;
        // A continuation line: spaces, then `from `. Anything else that begins with a space
        // is source text or a caret and is left alone.
        std::size_t at = 0;
        while (at < body.size() && body[at] == ' ')
            ++at;
        if (!body.substr(at).starts_with(ContinuedFrom))
            return std::nullopt;
        return SplitGccPathAt(line, body, at + ContinuedFrom.size());
    }

    /// The GCC-family diagnostic head, `<path>:<line>:<col>: ...`, at column zero.
    ///
    /// Never a scan for path-shaped spans. A GCC diagnostic embeds the offending SOURCE
    /// LINE and a caret, and this repository's own tests carry path literals -- a blanket
    /// rewrite would corrupt a snippet that merely quotes one, turning a correct diagnostic
    /// into a wrong one. Only a column-zero head and the include chain hold a path; the
    /// rest of the line is somebody's code. An include-chain line or an indented one is
    /// the other rule's to answer, or nobody's.
    [[nodiscard]] std::optional<LineSplit> SplitGccDiagnosticHead(std::string_view line, std::string_view body)
    {
        if (body.starts_with(IncludedFrom) || (!body.empty() && body.front() == ' '))
            return std::nullopt;
        return SplitGccPathAt(line, body, 0);
    }

    constexpr std::array<LineRule, 1> IncludeNoteRules { &SplitIncludeNote };
    constexpr std::array<LineRule, 1> MsvcDiagnosticRules { &SplitMsvcDiagnostic };
    /// Every line language the MSVC family writes on one console stream: `/showIncludes`
    /// notes, its own diagnostic heads, and `clang-cl`'s GCC-style include chain -- and its
    /// GCC-style HEAD, `<path>:<line>:<col>: `, which `clang-cl -fdiagnostics-format=clang`
    /// (or `/clang:` spelling it) writes instead of `(line,col): `. The note rule FIRST: a
    /// note line has no location, so it cannot be a diagnostic, but the order states which
    /// language a line is asked about before the others. The MSVC head before the GCC one,
    /// so a line both could read is read as the driver's own shape.
    ///
    /// Not covered, and stated so: `In module 'm' imported from <path>:1:` keeps the
    /// producer's path -- a clang modules build, which this launcher refuses to cache anyway.
    constexpr std::array<LineRule, 4> MsvcStreamRules {
        &SplitIncludeNote, &SplitMsvcDiagnostic, &SplitGccIncludeChain, &SplitGccDiagnosticHead
    };
    constexpr std::array<LineRule, 2> GccDiagnosticRules { &SplitGccIncludeChain, &SplitGccDiagnosticHead };

    /// The line shapes a grammar recognizes, tried in order.
    ///
    /// An exhaustive `switch` rather than an indexed table so a new enumerator is a
    /// `-Wswitch` error HERE, at the one place that says what it means.
    /// @param grammar The region's grammar.
    /// @return Its rules; none for the depfile grammar, which `RewriteDepfile` walks.
    [[nodiscard]] std::span<LineRule const> RulesOf(Grammar grammar) noexcept
    {
        switch (grammar)
        {
            case Grammar::ShowIncludes:
                return IncludeNoteRules;
            case Grammar::MsvcDiagnostics:
                return MsvcDiagnosticRules;
            case Grammar::MsvcStream:
                return MsvcStreamRules;
            case Grammar::GccDiagnostics:
                return GccDiagnosticRules;
            case Grammar::GccDepfile:
                // A depfile line carries MANY path spans (a target plus its whole
                // dependency list), so it cannot be expressed as one head/path/tail
                // split. RewriteDepfile handles this grammar instead.
                return {};
        }
        return {};
    }

    /// Split a line around its path span under the given grammar: the first of the
    /// grammar's rules the line matches decides it.
    /// @param line    One line WITHOUT its trailing newline (a trailing '\r' is
    ///                treated as part of the trailing text and preserved).
    /// @param grammar The active grammar.
    /// @return The split, or nothing when the line matches none of the grammar's shapes
    ///         (then the whole line is preserved).
    [[nodiscard]] std::optional<LineSplit> SplitLine(std::string_view line, Grammar grammar)
    {
        std::size_t const crLen = (!line.empty() && line.back() == '\r') ? 1U : 0U;
        std::string_view const body = line.substr(0, line.size() - crLen);
        for (auto const rule: RulesOf(grammar))
            if (auto split = rule(line, body))
                return split;
        return std::nullopt;
    }

    /// Rewrite every path token in a GNU-style Makefile depfile.
    ///
    /// A depfile line is `target: dep dep ...`, so unlike the single-span
    /// grammars it carries many paths per line. Both sides are rewritten: the
    /// target is the object path (under the build tree) and the dependencies are
    /// the source and its headers (under the source root), and a consumer needs
    /// all of them pointing into ITS checkout, not the producer's.
    ///
    /// Everything that is not a path token — whitespace, the `:` separator,
    /// backslash-newline continuations, and `\ ` escapes inside a path — is
    /// copied through byte-for-byte, so the file the build system reads keeps the
    /// exact syntax the compiler emitted.
    ///
    /// @param text  The depfile bytes.
    /// @param xform Path transform applied to each token (Canonicalize/Localize).
    /// @return The rewritten depfile.
    template <class Xform>
    [[nodiscard]] std::string RewriteDepfile(std::string_view text, Xform const& xform)
    {
        std::string out;
        out.reserve(text.size());

        std::string token; // the path token being accumulated, unescaped
        std::string raw;   // the same token exactly as written, escapes intact

        // Emit the pending token, transformed, re-applying the original escaping
        // when the transform left the token unchanged (so an untouched path keeps
        // its bytes) and escaping spaces afresh when it rewrote it.
        auto const flush = [&out, &token, &raw, &xform]() {
            if (token.empty())
                return;
            auto const rewritten = xform(std::string_view { token });
            if (rewritten == token)
            {
                out.append(raw); // unchanged — preserve the exact original spelling
            }
            else
            {
                // A rewritten path may contain spaces that make must not split on.
                for (char const c: rewritten)
                {
                    if (c == ' ')
                        out.push_back('\\');
                    out.push_back(c);
                }
            }
            token.clear();
            raw.clear();
        };

        // A `while` with ONE step at its foot, each arm an `else` of the one before rather than
        // a `continue`: an escape pair consumes its second character with a `++i` of its own,
        // so a counting `for` head would advertise a step this walk does not take -- and in a
        // range-for that `++i` would advance a COPY and read the escaped character as data,
        // which on this path is a wrong cache key rather than a crash.
        auto i = std::size_t { 0 };
        while (i < text.size())
        {
            char const c = text[i];

            // An escape pair belongs to the token: `\ ` is a literal space inside
            // a path. A backslash-newline is a line continuation and is not.
            if (c == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '\\' || text[i + 1] == ':'))
            {
                token.push_back(text[i + 1]);
                raw.push_back(c);
                raw.push_back(text[i + 1]);
                ++i; // the escaped character, consumed
            }
            // A backslash immediately before a newline is a line continuation and
            // ends the token; anywhere else it is a Windows path separator and
            // belongs to the path ("D:\src\a.cpp" is ONE token, not three).
            else if (c == '\\')
            {
                bool const continuation =
                    i + 1 < text.size()
                    && (text[i + 1] == '\n' || (text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n'));
                if (continuation)
                {
                    flush();
                    out.push_back(c);
                }
                else
                {
                    token.push_back(c);
                    raw.push_back(c);
                }
            }
            // Any separator ends the current token and is copied verbatim.
            else if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                flush();
                out.push_back(c);
            }
            // A ':' separates target from dependencies — unless it is a Windows
            // drive letter, which is part of the path itself ("C:\src\a.cpp").
            //
            // Only the letter rule is shared with the drive tests above; this
            // deliberately does not ask what follows the colon. The question here
            // is where a rule ends, and a drive-relative "C:foo" is still one
            // token — splitting it would hand the transform two fragments, neither
            // of which is a path.
            else if (c == ':' && !(token.size() == 1 && IsDriveLetter(token.front())))
            {
                flush();
                out.push_back(c);
            }
            else
            {
                token.push_back(c);
                raw.push_back(c);
            }
            ++i;
        }
        flush();
        return out;
    }

    /// Apply `xform` to each grammar-identified path span across every line of
    /// `text`, preserving newlines and non-matching lines byte-for-byte.
    /// @param text    The region bytes.
    /// @param grammar The active grammar.
    /// @param xform   Path-span transform (Canonicalize or Localize on one span).
    ///                Invoked once per matched span, so it is taken by const
    ///                reference rather than forwarded.
    /// @return The rewritten region.
    template <class Xform>
    [[nodiscard]] std::string RewriteRegion(std::string_view text, Grammar grammar, Xform const& xform)
    {
        std::string out;
        out.reserve(text.size());

        std::size_t pos = 0;
        while (pos <= text.size())
        {
            std::size_t const nl = text.find('\n', pos);
            bool const hasNl = nl != std::string_view::npos;
            std::string_view const line = text.substr(pos, hasNl ? nl - pos : std::string_view::npos);

            if (auto const split = SplitLine(line, grammar))
            {
                out.append(split->head);
                out.append(xform(split->path));
                out.append(split->tail);
            }
            else
            {
                out.append(line);
            }

            if (hasNl)
            {
                out.push_back('\n');
                pos = nl + 1;
                // A trailing newline ends the text; do not emit a phantom empty line.
                if (pos == text.size())
                    break;
            }
            else
            {
                break;
            }
        }
        return out;
    }

} // namespace

bool IsWindowsLayout(Layout const& layout) noexcept
{
    return IsWindowsRoot(layout.sourceRoot) || IsWindowsRoot(layout.buildTree);
}

Anchor AnchorForLayout(std::string_view path, Layout const& layout) noexcept
{
    if (path.empty())
        return Anchor::WorkingDirectory;
    if (!IsWindowsLayout(layout))
        return path.front() == '/' ? Anchor::Absolute : Anchor::WorkingDirectory;

    // Past the specifier, the separator is the whole distinction: `C:\x` names a
    // location, `C:x` names an offset from wherever drive C happens to be
    // pointing. A bare `C:` has no tail and is the latter — it *is* "the current
    // directory of drive C". Before issue #65 this test stopped at the colon, so
    // all three shapes were reported as absolute.
    if (HasDriveSpecifier(path))
        return DriveTailIsSeparator(path) ? Anchor::Absolute : Anchor::DriveRelative;

    // A leading separator is root-relative on Windows, and a UNC share (`\\host`)
    // begins with one too; both name a fixed location rather than a cwd-relative
    // one, so neither may be resolved against the working directory.
    return (path.front() == '\\' || path.front() == '/') ? Anchor::Absolute : Anchor::WorkingDirectory;
}

RootRelation RelateToLayout(std::string_view path, Layout const& layout)
{
    // The comparison forms are built here rather than asked of the caller, so that
    // the boundary byte `IsSegmentPrefix` looks for is the one `ComparisonForm`
    // guarantees. That pairing is the whole reason this entry point exists: the
    // launcher's classifier folds separators to BACKSLASH (its toolchain markers
    // are spelled that way), so handing its comparison form to `IsSegmentPrefix`
    // would ask for a `/` that cannot be there and answer "not under root" for
    // every path under every root.
    std::string const pathCmp = ComparisonForm(path);
    auto const folded = FoldRoots(layout);
    auto const source = RelateFolded(pathCmp, folded.sourceRoot);
    auto const build = RelateFolded(pathCmp, folded.buildTree);
    // Strongest answer wins, and `Under` outranking `NearMiss` is the whole content
    // of this function: with a build tree spelled as the source root's sibling, a
    // path inside it is a near miss of one root and correctly under the other.
    return std::max(source, build);
}

std::string Canonicalize(std::string_view absolutePath, Layout const& layout)
{
    return CanonicalizeOne(absolutePath, layout, FoldRoots(layout));
}

std::optional<std::string> CanonicalToken(std::string const& path, Layout const& layout)
{
    auto canonical = Canonicalize(path, layout);
    // Inequality is the signal, per `Canonicalize`'s own contract. Compared against
    // the INPUT rather than tested for a sentinel prefix: a layout may legitimately
    // have a root whose token spelling appears in an unrewritten path.
    if (canonical == path)
        return std::nullopt;
    return canonical;
}

std::string Localize(std::string_view token, Layout const& layout)
{
    return LocalizeOne(token, layout);
}

std::string CollapseRelativeSegments(std::string_view path)
{
    // The overwhelmingly common case, and the reason this is cheap enough to run over every note
    // of every compile: nothing to collapse means nothing to re-spell either.
    if (!HasDotDotSegment(path))
        return std::string { path };
    return LexicallyNormal(path);
}

std::string LexicallyNormal(std::string_view path)
{
    bool const wasNative = path.contains('\\');
    std::string folded { path };
    std::ranges::replace(folded, '\\', '/');

    // The anchor is held aside so the lexical pass never sees it: a drive root cannot be ascended
    // past, and neither can a UNC share -- `\\host\share` is ONE root, so `\\host\share\..\x.h`
    // is `\\host\share\x.h`, which is where Windows resolves it. Held as the host alone, the
    // share was an ordinary segment and `..` climbed to `\\host\x.h`, a different file.
    std::string_view tail { folded };
    std::string_view anchor;
    if (tail.starts_with("//") && !tail.starts_with("///"))
    {
        // `//host/share`, and the tail starts at the separator after it -- or is empty, for a
        // bare share, which has nothing to collapse.
        auto const hostEnd = tail.find('/', 2);
        auto const shareEnd = hostEnd == std::string_view::npos ? std::string_view::npos : tail.find('/', hostEnd + 1);
        if (shareEnd == std::string_view::npos)
            return std::string { path };
        anchor = tail.substr(0, shareEnd);
        tail.remove_prefix(shareEnd);
    }
    else if (tail.size() >= 2 && IsDriveLetter(tail[0]) && tail[1] == ':')
    {
        anchor = tail.substr(0, 2);
        tail.remove_prefix(2);
    }

    auto const collapsed = LexicallyCollapsed(tail);

    // A genuinely relative `../../x.hpp` has nothing lexical left to resolve. Returning the input
    // rather than the rewrite keeps the byte-exactness promise for a caller who gained nothing.
    if (HasDotDotSegment(collapsed))
        return std::string { path };

    auto rejoined = std::string { anchor } + collapsed;
    if (wasNative)
        std::ranges::replace(rejoined, '/', '\\');
    return rejoined;
}

std::string CanonicalizeRegion(std::string_view text, Grammar grammar, Layout const& layout)
{
    // Folded ONCE for the whole region rather than per span, which is where this
    // matters: the walkers below call `xform` once per path, and the roots are the
    // same for every one of them.
    auto const folded = FoldRoots(layout);
    // `..` is collapsed BEFORE a span is matched against the roots (#1593). A driver reports
    // `D:\proj\build\..\inc\a.hpp` for a header reached through a relative include, and matched
    // as it stands that is `<BUILDTREE>/../inc/a.hpp`: a token naming a file the path does not
    // name, which localizes into a consumer whose build tree sits elsewhere as a path to nothing.
    //
    // And a span whose collapsed form lies under NO root is stored as that collapsed spelling,
    // never matched as it was written: `<SRCROOT>/../third/x.h` is the same token naming a file
    // the path does not name, since it resolves against the CONSUMER's root depth. An absolute
    // path outside every root names this machine's file whichever way it is spelled, and
    // collapsed it at least names the file the compiler read.
    auto const xform = [&](std::string_view span) {
        return CanonicalizeOne(CollapseRelativeSegments(span), layout, folded);
    };
    // The depfile grammar is multi-token per line, so it needs its own walker.
    if (grammar == Grammar::GccDepfile)
        return RewriteDepfile(text, xform);
    return RewriteRegion(text, grammar, xform);
}

std::string RewritePaths(std::string_view text, Grammar grammar, PathTransform const& xform)
{
    // An absent transform is the identity, not a crash. std::function throws
    // std::bad_function_call when empty, and an empty PathTransform is an
    // idiomatic value here -- it is what RelativizeArgs defaults its own
    // parameter to -- so a caller that forwards one through would take down a
    // launcher whose entire contract is that a cache problem never breaks a build.
    if (!xform)
        return std::string { text };

    // The same two walkers the canonicalizers use, instantiated on the erased
    // transform. They stay templated so neither of those pays for the erasure.
    if (grammar == Grammar::GccDepfile)
        return RewriteDepfile(text, xform);
    return RewriteRegion(text, grammar, xform);
}

std::string LocalizeRegion(std::string_view text, Grammar grammar, Layout const& layout)
{
    auto const xform = [&](std::string_view span) {
        return LocalizeOne(span, layout);
    };
    if (grammar == Grammar::GccDepfile)
        return RewriteDepfile(text, xform);
    return RewriteRegion(text, grammar, xform);
}

std::string RewriteIncludeNoteMarker(std::string_view text, std::string_view from, std::string_view to)
{
    // Equal markers is the common case -- an English toolchain storing and an
    // English toolchain replaying -- and it must be byte-exact rather than merely
    // equivalent, so it returns the input instead of rebuilding it line by line.
    if (from.empty() || from == to)
        return std::string { text };

    // A `find` walk, one line per pass: the only advance is `offset = past` at the top of the
    // body, so every `continue` below has already taken it.
    std::string out;
    out.reserve(text.size());
    auto offset = std::size_t { 0 };
    while (offset < text.size())
    {
        auto const newline = text.find('\n', offset);
        auto const past = newline == std::string_view::npos ? text.size() : newline + 1;
        auto const line = text.substr(offset, past - offset);
        offset = past;

        // Matched against the body, emitted around the whole line: the terminators
        // are part of what survives byte-for-byte, and a region legitimately ends
        // without one.
        auto body = line;
        if (!body.empty() && body.back() == '\n')
            body.remove_suffix(1);
        if (!body.empty() && body.back() == '\r')
            body.remove_suffix(1);

        auto const markerEnd = IncludeNoteMarkerEnd(body, from);
        if (markerEnd == std::string_view::npos)
        {
            out.append(line);
            continue;
        }
        // The marker is anchored at column zero, so there is nothing in front of it
        // to copy: the line IS its marker followed by the rest. The indentation `cl`
        // uses for inclusion depth sits after the marker and rides along in the tail.
        out.append(to);
        out.append(line.substr(markerEnd)); // the path, its blanks and the terminators
    }
    return out;
}

} // namespace FastCache::PathCanon
