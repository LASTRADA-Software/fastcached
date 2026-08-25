// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/PathCanon.hpp>

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

    /// True when `rootCmp` (comparison form, no trailing separator) is a
    /// segment-boundary prefix of `pathCmp` (comparison form).
    /// @param pathCmp Comparison form of the candidate path.
    /// @param rootCmp Comparison form of a root.
    /// @return Whether the root prefixes the path on a segment boundary.
    [[nodiscard]] bool IsSegmentPrefix(std::string_view pathCmp, std::string_view rootCmp)
    {
        if (rootCmp.empty() || pathCmp.size() < rootCmp.size())
            return false;
        if (!pathCmp.starts_with(rootCmp))
            return false;
        // Exact match, or the next byte is a separator (segment boundary).
        return pathCmp.size() == rootCmp.size() || pathCmp[rootCmp.size()] == '/';
    }

    /// Rewrite a single native path to a token. Longest matching root wins.
    /// @param absolutePath Native-form path.
    /// @param layout       Producing machine's roots.
    /// @return The token, or the input verbatim when under neither root.
    [[nodiscard]] std::string CanonicalizeOne(std::string_view absolutePath, Layout const& layout)
    {
        std::string const pathCmp = ComparisonForm(absolutePath);
        std::string const srcCmp = ComparisonForm(layout.sourceRoot);
        std::string const buildCmp = ComparisonForm(layout.buildTree);

        bool const srcMatch = IsSegmentPrefix(pathCmp, srcCmp);
        bool const buildMatch = IsSegmentPrefix(pathCmp, buildCmp);

        // Longest root wins so a build tree nested under the source root maps to
        // <BUILDTREE>, not <SRCROOT>.
        if (buildMatch && (!srcMatch || buildCmp.size() >= srcCmp.size()))
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
            std::string_view tail = token.substr(sentinel.size());
            if (!tail.empty() && tail.front() == '/')
                tail.remove_prefix(1);
            return JoinLocalized(layout.*root, tail);
        }
        return std::string { token };
    }

    // --- Region grammar --------------------------------------------------------

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
                constexpr std::string_view prefix = "Note: including file:";
                if (!body.starts_with(prefix))
                    return false;
                std::size_t start = prefix.size();
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

std::string Canonicalize(std::string_view absolutePath, Layout const& layout)
{
    return CanonicalizeOne(absolutePath, layout);
}

std::string Localize(std::string_view token, Layout const& layout)
{
    return LocalizeOne(token, layout);
}

std::string CanonicalizeRegion(std::string_view text, Grammar grammar, Layout const& layout)
{
    auto const xform = [&](std::string_view span) {
        return CanonicalizeOne(span, layout);
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

} // namespace FastCache::PathCanon
