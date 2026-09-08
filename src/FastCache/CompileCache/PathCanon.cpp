// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace FastCache::PathCanon
{
namespace
{

    constexpr std::string_view SrcRootSentinel = "<SRCROOT>";
    constexpr std::string_view BuildTreeSentinel = "<BUILDTREE>";

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

    /// Where a `/showIncludes` note's marker ENDS on one line, or npos when the
    /// line is not a note.
    ///
    /// The one recognition rule for the two readers IN THIS FILE: `SplitLine`, which
    /// finds the path span to rewrite, and `RewriteIncludeNoteMarker`, which re-spells
    /// the prefix in front of it. A line one of them calls a note and the other does
    /// not is a stored region carrying a canonical token under a prefix nobody can
    /// find, or the reverse.
    ///
    /// **The launcher's `IncludeNotePath` is a THIRD reader and does not share this**,
    /// which is stated rather than implied because two hand-maintained copies of this
    /// rule is exactly the mechanism that produced
    /// [#891](https://github.com/LASTRADA-Software/fastcached/issues/891): that one
    /// skipped leading blanks while `SplitLine` demanded column zero, and nothing made
    /// them agree. They agree again now, by hand, which is the same footing. Promoting
    /// this helper to the header and calling it from `IncludeNotePath` is the repair --
    /// legal and free, since the app->library edge and the `_fc_cc_core` link edge both
    /// already exist -- and it is deliberately not folded in here.
    ///
    /// Anchored at the start of the line, and nothing may precede it but blanks.
    /// That is load-bearing on the launcher's side of the same rule: the splitter
    /// there runs over a stream that also carries preprocessed SOURCE, so a rule
    /// matching the marker anywhere in a line would delete an ordinary line that
    /// merely contains the text from the bytes the cache key is hashed over. Here
    /// it is milder and still real — both regions the launcher stores are tagged
    /// `ShowIncludes` and one of them is the diagnostic stream.
    ///
    /// **Leading blanks are skipped because `cl` INDENTS a note by inclusion depth**,
    /// and this grammar used to demand the marker at column zero while
    /// `IncludeNotePath` already skipped them. Nothing made the two agree, so a note
    /// for anything a header pulled in transitively — which is essentially all of
    /// them — had its path found by the launcher's reader and NOT by the
    /// canonicalizer, and the region was stored with the producing checkout's
    /// absolute paths in it. Independent of language, so it reached every MSVC direct
    /// compile with an include tree deeper than one
    /// ([#891](https://github.com/LASTRADA-Software/fastcached/issues/891)).
    ///
    /// The indentation itself is not part of the match and is preserved verbatim by
    /// both callers: Ninja ignores it, and rewriting it would be a change to text no
    /// defect asked for.
    ///
    /// @param body   One line, already stripped of its terminators.
    /// @param marker The prefix a note begins with.
    /// @return The offset just past `marker`, or npos when `body` is not a note.
    [[nodiscard]] std::size_t IncludeNoteMarkerEnd(std::string_view body, std::string_view marker) noexcept
    {
        // An empty marker would otherwise match at the head of every line, which
        // turns "this build does not know its own prefix" into "rewrite everything".
        if (marker.empty())
            return std::string_view::npos;
        auto const indent = body.find_first_not_of(" \t");
        if (indent == std::string_view::npos || !body.substr(indent).starts_with(marker))
            return std::string_view::npos;
        return indent + marker.size();
    }

    /// Split a line into (leading text kept verbatim, path span, trailing text kept
    /// verbatim) for the given grammar. Returns false when the line does not match
    /// the grammar's shape (then the whole line is preserved).
    /// @param line    One line WITHOUT its trailing newline (a trailing '\r' is
    ///                treated as part of the trailing text and preserved).
    /// @param grammar The active grammar.
    /// @param head    [out] Text before the path span.
    /// @param path    [out] The path span.
    /// @param tail    [out] Text after the path span (incl. any '\r').
    /// @return True if a path span was located.
    [[nodiscard]] bool SplitLine(
        std::string_view line, Grammar grammar, std::string_view& head, std::string_view& path, std::string_view& tail)
    {
        // Length of an optional trailing carriage return, kept as part of the
        // line body (so it lands in the trailing text and CRLF survives round-trip).
        std::size_t const crLen = (!line.empty() && line.back() == '\r') ? 1U : 0U;
        std::string_view const body = line.substr(0, line.size() - crLen);

        switch (grammar)
        {
            case Grammar::ShowIncludes: {
                // `IncludeNoteMarker` rather than a literal of this file's own. A
                // stored region carries the canonical marker BY CONTRACT -- the
                // producer normalizes to it -- so the grammar and that contract are
                // one constant, not two that happen to read alike.
                std::size_t start = IncludeNoteMarkerEnd(body, IncludeNoteMarker);
                if (start == std::string_view::npos)
                    return false;
                while (start < body.size() && body[start] == ' ')
                    ++start;
                if (start >= body.size())
                    return false;
                head = line.substr(0, start);
                path = body.substr(start);               // path excludes the CR
                tail = line.substr(line.size() - crLen); // just the CR (or empty)
                return true;
            }
            case Grammar::MsvcDiagnostics: {
                // "<path>(line[,col]): ..." — the path ends at the '(' beginning the
                // location. Require a following "): " to avoid matching a stray '('.
                std::size_t const open = body.find('(');
                std::size_t const close = body.find("): ");
                if (open == std::string_view::npos || close == std::string_view::npos || close < open)
                    return false;
                head = {};
                path = body.substr(0, open);
                tail = line.substr(open); // everything from '(' onward, incl. CR
                return true;
            }
            case Grammar::GccDiagnostics: {
                // Three ANCHORED shapes and nothing else:
                //
                //     <path>:<line>:<col>: ...                the diagnostic itself
                //     In file included from <path>:<line>[,:]  the include chain head
                //                      from <path>:<line>[,:]  its continuations
                //
                // Never a scan for path-shaped spans. A GCC diagnostic embeds the
                // offending SOURCE LINE and a caret, and this repository's own
                // tests carry path literals -- a blanket rewrite would corrupt a
                // snippet that merely quotes one, turning a correct diagnostic into
                // a wrong one. Only these positions hold a path; the rest of the
                // line is somebody's code.
                constexpr std::string_view IncludedFrom = "In file included from ";
                constexpr std::string_view ContinuedFrom = "from ";

                std::size_t begin = 0;
                if (body.starts_with(IncludedFrom))
                {
                    begin = IncludedFrom.size();
                }
                else if (!body.empty() && body.front() == ' ')
                {
                    // A continuation line: spaces, then `from `. Anything else that
                    // begins with a space is source text or a caret and is left alone.
                    std::size_t at = 0;
                    while (at < body.size() && body[at] == ' ')
                        ++at;
                    if (!body.substr(at).starts_with(ContinuedFrom))
                        return false;
                    begin = at + ContinuedFrom.size();
                }

                // The path runs to the `:` that begins `:<digits>` followed by `:`
                // or `,`. Searched left to right from `begin`, so a drive letter's
                // colon cannot end it -- `C:` is not followed by digits-then-
                // separator -- and a path containing a literal `:<digits>:` would
                // have to do so before its real location suffix, which no compiler
                // emits.
                std::size_t at = begin;
                while (true)
                {
                    std::size_t const colon = body.find(':', at);
                    if (colon == std::string_view::npos || colon == begin)
                        return false;
                    std::size_t digits = colon + 1;
                    while (digits < body.size() && body[digits] >= '0' && body[digits] <= '9')
                        ++digits;
                    if (digits > colon + 1 && digits < body.size() && (body[digits] == ':' || body[digits] == ','))
                    {
                        head = line.substr(0, begin);
                        path = body.substr(begin, colon - begin);
                        tail = line.substr(colon); // from the ':' onward, incl. any CR
                        return true;
                    }
                    at = colon + 1;
                }
            }
            case Grammar::GccDepfile:
                // A depfile line carries MANY path spans (a target plus its whole
                // dependency list), so it cannot be expressed as one head/path/tail
                // split. RewriteDepfile handles this grammar instead.
                return false;
        }
        return false;
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

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            char const c = text[i];

            // An escape pair belongs to the token: `\ ` is a literal space inside
            // a path. A backslash-newline is a line continuation and is not.
            if (c == '\\' && i + 1 < text.size() && (text[i + 1] == ' ' || text[i + 1] == '\\' || text[i + 1] == ':'))
            {
                token.push_back(text[i + 1]);
                raw.push_back(c);
                raw.push_back(text[i + 1]);
                ++i;
                continue;
            }

            // A backslash immediately before a newline is a line continuation and
            // ends the token; anywhere else it is a Windows path separator and
            // belongs to the path ("D:\src\a.cpp" is ONE token, not three).
            if (c == '\\')
            {
                bool const continuation =
                    i + 1 < text.size()
                    && (text[i + 1] == '\n' || (text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n'));
                if (continuation)
                {
                    flush();
                    out.push_back(c);
                    continue;
                }
                token.push_back(c);
                raw.push_back(c);
                continue;
            }

            // Any separator ends the current token and is copied verbatim.
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                flush();
                out.push_back(c);
                continue;
            }

            // A ':' separates target from dependencies — unless it is a Windows
            // drive letter, which is part of the path itself ("C:\src\a.cpp").
            //
            // Only the letter rule is shared with the drive tests above; this
            // deliberately does not ask what follows the colon. The question here
            // is where a rule ends, and a drive-relative "C:foo" is still one
            // token — splitting it would hand the transform two fragments, neither
            // of which is a path.
            if (c == ':' && !(token.size() == 1 && IsDriveLetter(token.front())))
            {
                flush();
                out.push_back(c);
                continue;
            }

            token.push_back(c);
            raw.push_back(c);
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

            std::string_view head;
            std::string_view path;
            std::string_view tail;
            if (SplitLine(line, grammar, head, path, tail))
            {
                out.append(head);
                out.append(xform(path));
                out.append(tail);
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

std::string Localize(std::string_view token, Layout const& layout)
{
    return LocalizeOne(token, layout);
}

std::string CanonicalizeRegion(std::string_view text, Grammar grammar, Layout const& layout)
{
    // Folded ONCE for the whole region rather than per span, which is where this
    // matters: the walkers below call `xform` once per path, and the roots are the
    // same for every one of them.
    auto const folded = FoldRoots(layout);
    auto const xform = [&](std::string_view span) {
        return CanonicalizeOne(span, layout, folded);
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

    std::string out;
    out.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();)
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
        out.append(line.substr(0, markerEnd - from.size())); // whatever preceded the marker
        out.append(to);
        out.append(line.substr(markerEnd)); // the path, its blanks and the terminators
    }
    return out;
}

} // namespace FastCache::PathCanon
