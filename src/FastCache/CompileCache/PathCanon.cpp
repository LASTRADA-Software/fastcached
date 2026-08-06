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

    /// Lower-case an ASCII byte (path comparison is case-insensitive on Windows).
    /// @param c Input byte.
    /// @return The lower-case form for A-Z, else the byte unchanged.
    [[nodiscard]] char AsciiLower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
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

    /// The separator a localized path should use. Taken from the consuming
    /// layout's own root rather than from the host OS: a cache is shared across
    /// machines, so a Windows consumer layout must localize to backslashes even
    /// when this code runs on POSIX (and vice versa). A root with no separator
    /// at all is treated as POSIX.
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
                // Depfile handling is multi-token; v1 treats each whitespace-split
                // token via the caller. Not reached as a single-span line here.
                return false;
        }
        return false;
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

std::expected<std::string, CanonError> Canonicalize(std::string_view absolutePath, Layout const& layout)
{
    return CanonicalizeOne(absolutePath, layout);
}

std::expected<std::string, CanonError> Localize(std::string_view token, Layout const& layout)
{
    return LocalizeOne(token, layout);
}

std::expected<std::string, CanonError> CanonicalizeRegion(std::string_view text, Grammar grammar, Layout const& layout)
{
    return RewriteRegion(text, grammar, [&](std::string_view span) { return CanonicalizeOne(span, layout); });
}

std::expected<std::string, CanonError> LocalizeRegion(std::string_view text, Grammar grammar, Layout const& layout)
{
    return RewriteRegion(text, grammar, [&](std::string_view span) { return LocalizeOne(span, layout); });
}

} // namespace FastCache::PathCanon
