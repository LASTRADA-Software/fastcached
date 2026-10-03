// SPDX-License-Identifier: Apache-2.0
#include "DependencyOutput.hpp"
#include "DirectManifest.hpp"
#include "RootBinding.hpp"
#include "RootReconciler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// A resolver driven by a table instead of a filesystem.
///
/// The whole point of the seam: the conditions this class exists for — an 8.3
/// short component, a `subst` drive, a junction — cannot be created on the host
/// running these tests, and two of the three cannot be created on any host that is
/// not Windows. A table states the aliasing directly and every case below is then
/// reproducible everywhere.
class FakeResolver final: public IPathResolver
{
  public:
    /// @param aliases Prefix -> the spelling the "filesystem" reports for it.
    explicit FakeResolver(std::map<std::string, std::string, std::less<>> aliases):
        _aliases { std::move(aliases) }
    {
    }

    std::string Resolve(std::string_view path) override
    {
        ++_calls;
        return Rewrite(path);
    }

    std::string ResolveDirectory(std::string_view path) override
    {
        ++_calls;
        return Rewrite(path);
    }

    std::string ShortForm(std::string_view directory) override
    {
        return std::string { directory };
    }

    [[nodiscard]] std::size_t FilesystemCalls() const noexcept override
    {
        return _calls;
    }

  private:
    // The LONGEST matching prefix, so an alias of a directory and an alias of one of
    // its subdirectories compose as a filesystem composes them.
    [[nodiscard]] std::string Rewrite(std::string_view path) const
    {
        std::string const* from = nullptr;
        std::string const* to = nullptr;
        for (auto const& [alias, target]: _aliases)
        {
            if (path.starts_with(alias) && (from == nullptr || alias.size() > from->size()))
            {
                from = &alias;
                to = &target;
            }
        }
        if (from == nullptr)
            return std::string { path };
        return *to + std::string { path.substr(from->size()) };
    }

    std::map<std::string, std::string, std::less<>> _aliases;
    std::size_t _calls { 0 };
};

/// The measured Windows shape: the build system spells the root short, `cl`
/// resolves an include through the filesystem and reports it long.
[[nodiscard]] FakeResolver ShortNameHost()
{
    return FakeResolver { { { R"(C:\Users\RUNNER~1\p)", R"(C:\Users\runneradmin\p)" } } };
}

} // namespace

TEST_CASE("WithoutTrailingSeparator trims a root but never a bare one")
{
    CHECK(WithoutTrailingSeparator("/x/build/") == "/x/build");
    CHECK(WithoutTrailingSeparator("/x/build//") == "/x/build");
    CHECK(WithoutTrailingSeparator(R"(D:\proj\build\)") == R"(D:\proj\build)");
    CHECK(WithoutTrailingSeparator("/x/build") == "/x/build");

    // A bare root IS its trailing separator; trimming `C:\` to `C:` would also
    // flip the separator style PathCanon derives from it when localizing, and `/`
    // would become empty, which is a prefix of nothing at all.
    CHECK(WithoutTrailingSeparator("/") == "/");
    CHECK(WithoutTrailingSeparator(R"(C:\)") == R"(C:\)");
    CHECK(WithoutTrailingSeparator("C:/") == "C:/");
    CHECK(WithoutTrailingSeparator("").empty());

    // The drive test is narrow, so a three-byte string that merely has a colon in
    // the middle is an ordinary root and its separator comes off.
    CHECK(WithoutTrailingSeparator("a:/") == "a:/"); // a real drive letter
    CHECK(WithoutTrailingSeparator("1:/") == "1:");  // not a drive at all
    CHECK(WithoutTrailingSeparator("ab/") == "ab");
}

TEST_CASE("A path the driver spelled differently is translated into the build's spelling")
{
    // The defect issue #66 records: the root is spelled one way and nothing the
    // driver emits shares that spelling, so every root test fails silently.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    CHECK(reconciler.Path(R"(C:\Users\runneradmin\p\src\inc\h1.h)") == R"(C:\Users\RUNNER~1\p\src\inc\h1.h)");
    CHECK(reconciler.Directory(R"(C:\Users\runneradmin\p\src\inc)") == R"(C:\Users\RUNNER~1\p\src\inc)");
}

TEST_CASE("A path already spelled the build's way is returned untouched and costs no probe")
{
    // The correctness case, not an optimization. Resolution rewrites a symlink
    // ANYWHERE in a path, so round-tripping an already-correct one would rewrite
    // an in-tree alias and split the key between two byte-identical checkouts.
    auto resolver = FakeResolver { { { R"(C:\p\src\inc)", R"(C:\p\src\real-inc)" } } };
    RootReconciler reconciler { R"(C:\p\src)", R"(C:\p\build)", resolver };

    // Measured from AFTER construction: the two roots are resolved once there, and
    // that is the only filesystem work a healthy build should ever pay for.
    auto const afterConstruction = resolver.FilesystemCalls();

    constexpr auto viaInTreeAlias = R"(C:\p\src\inc\h1.h)";
    CHECK(reconciler.Path(viaInTreeAlias) == viaInTreeAlias);
    CHECK(reconciler.Directory(R"(C:\p\src\inc)") == R"(C:\p\src\inc)");
    CHECK(resolver.FilesystemCalls() == afterConstruction);
}

TEST_CASE("A prefix the reconciler maps onto a root is recorded as that root's alias")
{
    // The other direction from #66: the build exports the LONG root and an argument spells
    // it short. What the key maps, the root scan must look for (`RootAliasList`).
    auto resolver = FakeResolver { { { R"(C:\Users\RUNNER~1\p)", R"(C:\Users\runneradmin\p)" } } };
    RootReconciler reconciler { R"(C:\Users\runneradmin\p\src)", R"(C:\Users\runneradmin\p\build)", resolver };
    CHECK(reconciler.Aliases().Entries().empty());

    CHECK(reconciler.Directory(R"(C:\Users\RUNNER~1\p\src\inc)") == R"(C:\Users\runneradmin\p\src\inc)");
    CHECK(reconciler.Path(R"(C:\Users\RUNNER~1\p\build\gen\g.h)") == R"(C:\Users\runneradmin\p\build\gen\g.h)");
    auto const& aliases = reconciler.Aliases();
    CHECK(aliases.Contains(CheckoutPart::SourceRoot, R"(C:\Users\RUNNER~1\p\src)"));
    CHECK(aliases.Contains(CheckoutPart::BuildTree, R"(C:\Users\RUNNER~1\p\build)"));
    CHECK(aliases.Entries().size() == 2);

    // Mapped again, recorded once.
    CHECK(reconciler.Path(R"(C:\Users\RUNNER~1\p\src\inc\other.h)") == R"(C:\Users\runneradmin\p\src\inc\other.h)");
    CHECK(aliases.Entries().size() == 2);
}

TEST_CASE("A root's own spelling is never recorded as an alias of it")
{
    // #66's shape: the root is exported short and the driver reports it long. The long
    // spelling is the RESOLVED root, which the scan knows already.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };
    CHECK(reconciler.Path(R"(C:\Users\runneradmin\p\src\inc\h1.h)") == R"(C:\Users\RUNNER~1\p\src\inc\h1.h)");
    CHECK(reconciler.Aliases().Entries().empty());
}

TEST_CASE("An alias whose tail crosses a second link is found by walking up its spelling")
{
    // The combined re-review's I1, measured as a WRONG HIT: `jx\L1` a junction to the
    // build tree, `genlink` a junction INSIDE it. The resolved path has three components
    // below the root and the spelling one, so a count of components found no alias -- and
    // the key mapped the path anyway. Walking up the spelling finds `jx\L1`.
    auto resolver = FakeResolver { {
        { R"(C:\run\jx\L1)", R"(C:\run\ck\b1)" },
        { R"(C:\run\jx\L1\genlink)", R"(C:\run\ck\b1\x\y\gen)" },
    } };
    RootReconciler reconciler { R"(C:\run\ck)", R"(C:\run\ck\b1)", resolver };
    CHECK(reconciler.Directory(R"(C:\run\jx\L1\genlink)") == R"(C:\run\ck\b1\x\y\gen)");
    CHECK(reconciler.Aliases().Contains(CheckoutPart::BuildTree, R"(C:\run\jx\L1)"));
    // The build tree lies inside the source root, so `jx\L1` resolves INTO the source root
    // as well, and is that root's alias too: the object names a path under both.
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, R"(C:\run\jx\L1)"));
    CHECK(reconciler.Aliases().Entries().size() == 2);
}

TEST_CASE("An alias running through the build tree into the source root is recorded as both")
{
    // The combined re-review's last finding, measured on cl and clang-cl: `jx\L1` a
    // junction to build directory 1, `srclink` a junction inside it back to the source root.
    // The nearest ancestor naming a root is `jx\L1\srclink` (the source root); stopping there
    // never saw that `jx\L1` above it is build directory 1, and build directory 2 was served
    // a path spelled through build directory 1's link.
    auto resolver = FakeResolver { {
        { R"(C:\run\jx\L1)", R"(C:\run\ck\b1)" },
        { R"(C:\run\jx\L1\srclink)", R"(C:\run\ck)" },
    } };
    RootReconciler reconciler { R"(C:\run\ck)", R"(C:\run\ck\b1)", resolver };
    CHECK(reconciler.Directory(R"(C:\run\jx\L1\srclink\src\gen)") == R"(C:\run\ck\src\gen)");
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, R"(C:\run\jx\L1\srclink)"));
    CHECK(reconciler.Aliases().Contains(CheckoutPart::BuildTree, R"(C:\run\jx\L1)"));
    CHECK(reconciler.Aliases().Entries().size() == 2);
}

TEST_CASE("An alias through a subdirectory of the build tree into the source root is recorded as both")
{
    // The fourth shape of one class, measured on cl and clang-cl: `C:\a\L` a junction to
    // `b1\x`, a SUBDIRECTORY of the build tree, with `srclink` inside it back to the source
    // root. No ancestor resolves TO build directory 1, only INTO it, so a walk that
    // recorded only exact matches bound the object to the source root alone.
    auto resolver = FakeResolver { {
        { R"(C:\a\L)", R"(C:\run\ck\b1\x)" },
        { R"(C:\a\L\srclink)", R"(C:\run\ck)" },
    } };
    RootReconciler reconciler { R"(C:\run\ck)", R"(C:\run\ck\b1)", resolver };
    CHECK(reconciler.Directory(R"(C:\a\L\srclink\src\gen)") == R"(C:\run\ck\src\gen)");
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, R"(C:\a\L\srclink)"));
    CHECK(reconciler.Aliases().Contains(CheckoutPart::BuildTree, R"(C:\a\L)"));
    CHECK(reconciler.Aliases().Entries().size() == 2);
}

TEST_CASE("A link from outside into a directory below a root is that root's alias")
{
    // No ancestor of the spelling is the root, but one resolves INTO it: the highest such
    // ancestor is recorded, one entry for everything below it.
    auto resolver = FakeResolver { { { R"(C:\a\b\c\lnk)", R"(C:\p\src\d\e)" } } };
    RootReconciler reconciler { R"(C:\p\src)", R"(C:\p\build)", resolver };
    CHECK(reconciler.Path(R"(C:\a\b\c\lnk\x.h)") == R"(C:\p\src\d\e\x.h)");
    CHECK(reconciler.Directory(R"(C:\a\b\c\lnk\inc)") == R"(C:\p\src\d\e\inc)");
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, R"(C:\a\b\c\lnk)"));
    CHECK(reconciler.Aliases().Entries().size() == 1);
}

TEST_CASE("A path no ancestor of whose spelling resolves into a root is not mapped at all")
{
    // A FILE that is itself a link into the tree, in a directory outside every root: no
    // ancestor is asked about the leaf, and none of them lies in a root, so there is no
    // alias the scan could look for. Mapping it anyway would put a spelling into the key
    // that the scan cannot see -- the WRONG hit -- so it is keyed as it stands.
    auto resolver = FakeResolver { { { R"(C:\a\f.h)", R"(C:\p\src\f.h)" } } };
    RootReconciler reconciler { R"(C:\p\src)", R"(C:\p\build)", resolver };
    CHECK(reconciler.Path(R"(C:\a\f.h)") == R"(C:\a\f.h)");
    CHECK(reconciler.Aliases().Entries().empty());
}

TEST_CASE("Every path the reconciler maps is covered by the alias list or a root's own spelling")
{
    // The invariant the list exists for, asked of every shape here rather than of one: a
    // path whose spelling changed was mapped, and a mapped path must have an ancestor the
    // scan knows -- a root spelled as exported or resolved, or an entry on the list.
    auto resolver = FakeResolver { {
        { R"(C:\run\jx\L1)", R"(C:\run\ck\b1)" },
        { R"(C:\run\jx\L1\genlink)", R"(C:\run\ck\b1\x\y\gen)" },
        { R"(C:\run\CK~1)", R"(C:\run\ck)" },
        { R"(S:\)", R"(C:\run\ck\)" },
        { R"(C:\a\lnk)", R"(C:\run\ck\src\deep)" },
        { R"(C:\run\jx\L2)", R"(C:\run\ck\b1)" },
        { R"(C:\run\jx\L2\srclink)", R"(C:\run\ck)" },
        { R"(C:\sub\L)", R"(C:\run\ck\b1\x)" },
        { R"(C:\sub\L\srclink)", R"(C:\run\ck)" },
    } };
    RootReconciler reconciler { R"(C:\run\ck)", R"(C:\run\ck\b1)", resolver };
    constexpr auto Spellings = std::to_array<std::string_view>({
        R"(C:\run\jx\L1\genlink)",
        R"(C:\run\jx\L1\x\y\gen)",
        R"(C:\run\CK~1\src\inc)",
        R"(C:\run\CK~1\b1\gen)",
        R"(S:\src\inc)",
        R"(C:\a\lnk\inc)",
        R"(C:\run\ck\src\inc)",
        R"(C:\run\jx\L2\srclink\src\gen)",
        R"(C:\sub\L\srclink\src\gen)",
    });
    auto const covered = [&reconciler](std::string_view spelling) {
        auto const lower = PathCanon::AsciiLower(spelling);
        auto const under = [&lower](std::string_view prefix) {
            auto const folded = PathCanon::AsciiLower(prefix);
            return lower.starts_with(folded);
        };
        auto const& layout = reconciler.Layout();
        auto const& aliases = reconciler.Aliases().Entries();
        return under(layout.sourceRoot) || under(layout.buildTree)
               || std::ranges::any_of(aliases, [&under](RootAlias const& alias) { return under(alias.spelling); });
    };
    for (auto const spelling: Spellings)
    {
        INFO(spelling);
        auto const mapped = reconciler.Directory(spelling);
        if (mapped != spelling)
            CHECK(covered(spelling));
    }
    // And the shapes that should map did -- every one here, the link into a subdirectory
    // included, which is recorded as the root it resolves into.
    CHECK(reconciler.Directory(R"(C:\run\jx\L1\genlink)") != R"(C:\run\jx\L1\genlink)");
    CHECK(reconciler.Directory(R"(S:\src\inc)") != R"(S:\src\inc)");
    CHECK(reconciler.Directory(R"(C:\a\lnk\inc)") != R"(C:\a\lnk\inc)");
    // Through a SUBDIRECTORY of the build tree into the source root: both roots as well.
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, R"(C:\sub\L\srclink)"));
    CHECK(reconciler.Aliases().Contains(CheckoutPart::BuildTree, R"(C:\sub\L)"));
    // And the alias running through the build tree into the source root is covered as
    // BOTH roots, not merely by some ancestor: the object it produces names both.
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, R"(C:\run\jx\L2\srclink)"));
    CHECK(reconciler.Aliases().Contains(CheckoutPart::BuildTree, R"(C:\run\jx\L2)"));
}

namespace
{

/// How a filesystem reads a `..` segment. Private to this file, never transmitted.
enum class DotDot : std::uint8_t
{
    /// Collapsed from the SPELLING before anything is resolved, as Windows does:
    /// `L\..\x` never opens `L`.
    Lexical,
    /// Taken from where the path has LED so far, as POSIX `realpath` does: `L\..` is the
    /// directory holding `L`'s target.
    Physical,
};

/// A filesystem of LINKS, resolved as a real one resolves: component by component, a
/// prefix that is a link replaced by its target -- a target that is itself a link
/// followed too, a bounded number of hops. Where `FakeResolver` rewrites one prefix, this
/// composes any number, which is what the generative case below needs.
class LinkResolver final: public IPathResolver
{
  public:
    /// @param links Link location -> its target, both spelled with `\`.
    /// @param dotDot How a `..` segment is read.
    explicit LinkResolver(std::map<std::string, std::string> links, DotDot dotDot = DotDot::Lexical):
        _links { std::move(links) },
        _dotDot { dotDot }
    {
    }

    std::string Resolve(std::string_view path) override
    {
        return Real(path);
    }

    std::string ResolveDirectory(std::string_view path) override
    {
        return Real(path);
    }

    std::string ShortForm(std::string_view directory) override
    {
        return std::string { directory };
    }

    [[nodiscard]] std::size_t FilesystemCalls() const noexcept override
    {
        return 0;
    }

    /// @param path A path. @return Where it leads.
    [[nodiscard]] std::string Real(std::string_view path) const
    {
        constexpr int HopLimit = 8;
        auto const spelled = _dotDot == DotDot::Lexical ? PathCanon::LexicallyNormal(path) : std::string { path };
        std::string current;
        for (auto const component: spelled | std::views::split('\\'))
        {
            std::string const name { component.begin(), component.end() };
            if (name.empty() || name == ".")
                continue;
            if (name == "..")
            {
                // Never above the drive: `C:\..` is `C:\`.
                auto const separator = current.find_last_of('\\');
                if (separator != std::string::npos)
                    current.resize(separator);
                continue;
            }
            if (!current.empty())
                current.push_back('\\');
            current.append(name);
            for ([[maybe_unused]] auto const hop: std::views::iota(0, HopLimit))
            {
                auto const link = _links.find(current);
                if (link == _links.end())
                    break;
                current = link->second;
            }
        }
        return current;
    }

  private:
    std::map<std::string, std::string> _links;
    DotDot _dotDot;
};

/// @param path A path. @param root A root. @return Whether @p path is @p root or below it.
[[nodiscard]] bool InRoot(std::string const& path, std::string const& root)
{
    PathCanon::Layout const alone { .sourceRoot = root, .buildTree = {} };
    return PathCanon::Canonicalize(path, alone) != path;
}

/// @param path A path. @param root A root. @return Whether @p path spells @p root itself.
[[nodiscard]] bool SpellsRootItself(std::string const& path, std::string const& root)
{
    PathCanon::Layout const alone { .sourceRoot = root, .buildTree = {} };
    return PathCanon::Canonicalize(path, alone) == PathCanon::Canonicalize(root, alone);
}

/// @param spelling A spelling. @return It and every ancestor, nearest first.
[[nodiscard]] std::vector<std::string> SelfAndAncestors(std::string spelling)
{
    std::vector<std::string> all;
    while (!spelling.empty())
    {
        all.push_back(spelling);
        auto const separator = spelling.find_last_of('\\');
        if (separator == std::string::npos)
            break;
        // `S:\` is a directory; `S:` is not.
        spelling = separator == 2 && spelling[1] == ':' && spelling.size() > 3 ? spelling.substr(0, 3)
                                                                               : spelling.substr(0, separator);
        if (!all.empty() && spelling == all.back())
            break;
    }
    return all;
}

/// The roots every generated composition is asked against: the build tree inside the source
/// root, as the measured repros lay them out.
constexpr std::string_view GeneratedSource = R"(C:\run\ck)";
constexpr std::string_view GeneratedBuild = R"(C:\run\ck\b1)";

/// @return Every place a generated link points: each root, directories inside each, and one
///         outside both.
[[nodiscard]] std::array<std::string, 7> GeneratedTargets()
{
    return {
        R"(C:\run\ck)",      R"(C:\run\ck\src)",    R"(C:\run\ck\src\gen)", R"(C:\run\ck\b1)",
        R"(C:\run\ck\b1\x)", R"(C:\run\ck\b1\x\y)", R"(C:\run\out)",
    };
}

/// A set of links and a spelling through them.
struct Composition
{
    std::map<std::string, std::string> links;
    std::string spelling;
};

/// Every composition of links the generative cases enumerate: flat, nested, into a
/// subdirectory, through, self-loops (a link to its own ancestor), chains of links to links, a
/// `subst` drive -- up to three deep across both roots. Two anchors, seven targets, then per
/// link two placements and seven or eight targets (the chain's first link among them), two
/// tails at each depth.
/// @return The compositions, in enumeration order.
[[nodiscard]] std::vector<Composition> EnumeratedCompositions()
{
    auto const targets = GeneratedTargets();
    auto const anchors = std::to_array<std::string>({ R"(C:\a\L)", "S:" });
    auto const placements = std::to_array<std::string>({ "", R"(m\)" });
    auto const tails = std::to_array<std::string>({ "", "t" });
    // `S:\` is the drive's root and `S:` its current directory, so a spelling joins
    // components without doubling the root's own separator.
    auto const join = [](std::string const& head, std::string const& tail) {
        if (tail.empty())
            return head;
        return head.ends_with('\\') ? head + tail : std::format("{}\\{}", head, tail);
    };

    std::vector<Composition> compositions;
    // Grows a chain by one link, placed inside where the chain so far leads.
    auto grow = [&](auto const& self, Composition const& chain, std::string const& lastTarget, int depth) -> void {
        for (auto const& tail: tails)
            compositions.push_back(Composition { .links = chain.links, .spelling = join(chain.spelling, tail) });
        if (depth == 3)
            return;
        auto const where = LinkResolver { chain.links }.Real(lastTarget);
        // A link may point at anything the table does, or at the chain's first link: a
        // chain of links to links.
        std::vector<std::string> choices(targets.begin(), targets.end());
        if (chain.links.contains(anchors[0]))
            choices.push_back(anchors[0]);
        for (auto const& placement: placements)
        {
            auto const name = std::format("l{}", depth + 1);
            auto const location = std::format("{}\\{}{}", where, placement, name);
            for (auto const& target: choices)
            {
                Composition next { .links = chain.links, .spelling = join(chain.spelling, placement + name) };
                next.links[location] = target;
                self(self, next, target, depth + 1);
            }
        }
    };
    for (auto const& anchor: anchors)
        for (auto const& target: targets)
            grow(grow,
                 Composition { .links = { { anchor, target } },
                               .spelling = anchor == "S:" ? std::string { R"(S:\)" } : anchor },
                 target,
                 1);
    return compositions;
}

/// The shapes found by hand, as the reviewer's repro (`jx2.ps1`) spells them for build
/// directory 1, so each is asked by name as well as somewhere in an enumeration.
/// @return Each shape's name and composition.
[[nodiscard]] std::vector<std::pair<std::string_view, Composition>> SeedCompositions()
{
    std::string const source { GeneratedSource };
    std::string const build { GeneratedBuild };
    return {
        { "flat", { .links = { { R"(C:\run\jx\L1)", build } }, .spelling = R"(C:\run\jx\L1\x\y\gen)" } },
        { "nested",
          { .links = { { R"(C:\run\ck\b1\genlink)", R"(C:\run\ck\b1\x\y\gen)" }, { R"(C:\run\jx\L1)", build } },
            .spelling = R"(C:\run\jx\L1\genlink)" } },
        { "through",
          { .links = { { R"(C:\run\ck\b1\srclink)", source }, { R"(C:\run\jx\L1)", build } },
            .spelling = R"(C:\run\jx\L1\srclink\src\gen)" } },
        { "subthrough",
          { .links = { { R"(C:\run\ck\b1\x\srclink)", source }, { R"(C:\run\jx\L1)", R"(C:\run\ck\b1\x)" } },
            .spelling = R"(C:\run\jx\L1\srclink\src\gen)" } },
        // The build tree reached twice ...
        { "selfbuild",
          { .links = { { R"(C:\run\ck\b1\self)", build }, { R"(C:\run\jx\L1)", build } },
            .spelling = R"(C:\run\jx\L1\self\x\y\gen)" } },
        // ... twice and then through into the source root ...
        { "selfthrough",
          { .links = { { R"(C:\run\ck\b1\self)", build },
                       { R"(C:\run\ck\b1\srclink)", source },
                       { R"(C:\run\jx\L1)", build } },
            .spelling = R"(C:\run\jx\L1\self\srclink\src\gen)" } },
        // ... through, then the source root twice ...
        { "srcself",
          { .links = { { R"(C:\run\ck\srcself)", source },
                       { R"(C:\run\ck\b1\srclink)", source },
                       { R"(C:\run\jx\L1)", build } },
            .spelling = R"(C:\run\jx\L1\srclink\srcself\src\gen)" } },
        // ... and a shared alias of the SOURCE root through a link to the build tree.
        { "revthrough",
          { .links = { { R"(C:\run\jx\S)", source }, { R"(C:\run\ck\bl1)", build } },
            .spelling = R"(C:\run\jx\S\bl1\x\y\gen)" } },
    };
}

/// What asking one composition found.
struct CoverageFinding
{
    /// Some directory the path passes through lies in a root.
    bool reachesRoot = false;
    /// The key mapped the spelling.
    bool mapped = false;
    /// How many roots the path passes through.
    std::size_t roots = 0;
    /// Why the invariant does not hold, when it does not.
    std::optional<std::string> failure;
};

/// The class's rule, asked of one composition and INDEPENDENT of how the reconciler meets it:
/// for each root that a directory the path passes through lies in, the object's text must
/// carry a needle for that root -- an ancestor of the spelling a compiler writes that is on the
/// alias list or spells the root. Which spelling a compiler writes depends on its flags: as
/// given, `.` and `..` kept (`cl` with no debug flag, clang-cl), or collapsed (`cl` under
/// `/Z7`, `/Zi`, `/ZI` or `/FC`), so where a `..` is read lexically (Windows) the normal form
/// is asserted as well. There the path
/// passes through its NORMAL form's ancestors; where a `..` is read physically (POSIX) it
/// passes through every prefix as spelled.
/// @param composition The links and the spelling.
/// @param dotDot How the filesystem reads a `..`.
/// @return What was found.
[[nodiscard]] CoverageFinding CheckCoverage(Composition const& composition, DotDot dotDot)
{
    std::string const source { GeneratedSource };
    std::string const build { GeneratedBuild };
    LinkResolver resolver { composition.links, dotDot };
    RootReconciler reconciler { source, build, resolver };
    auto const result = reconciler.Directory(composition.spelling);
    auto const spelled = SelfAndAncestors(composition.spelling);
    auto const normal = SelfAndAncestors(PathCanon::LexicallyNormal(composition.spelling));
    auto const& route = dotDot == DotDot::Lexical ? normal : spelled;

    CoverageFinding finding;
    std::vector<std::pair<CheckoutPart, std::string>> roots;
    for (auto const& [part, root]:
         { std::pair { CheckoutPart::SourceRoot, source }, std::pair { CheckoutPart::BuildTree, build } })
        if (std::ranges::any_of(route, [&](std::string const& ancestor) { return InRoot(resolver.Real(ancestor), root); }))
            roots.emplace_back(part, root);
    finding.reachesRoot = !roots.empty();
    finding.mapped = result != composition.spelling;
    finding.roots = roots.size();

    std::string linkText;
    for (auto const& [location, target]: composition.links)
        linkText += std::format("{} -> {}; ", location, target);
    auto const fail = [&](std::string_view why) {
        finding.failure = std::format("{} with {}: {}", composition.spelling, linkText, why);
    };
    // Where nothing can refuse -- a lexical `..` leads where its normal form does -- a
    // spelling whose own path lies in a root is mapped: an ancestor is always found.
    auto const lands = resolver.Real(composition.spelling);
    if (dotDot == DotDot::Lexical && (InRoot(lands, source) || InRoot(lands, build)) && !finding.mapped)
        fail("lies in a root and was not mapped");
    if (!finding.mapped)
        return finding;
    auto const needle = [&](std::vector<std::string> const& ancestors, CheckoutPart part, std::string const& root) {
        return std::ranges::any_of(ancestors, [&](std::string const& ancestor) {
            return reconciler.Aliases().Contains(part, ancestor) || SpellsRootItself(ancestor, root);
        });
    };
    for (auto const& [part, root]: roots)
    {
        if (!needle(spelled, part, root))
            fail(std::format("no needle for {} in the spelling as written", CheckoutPartName(part)));
        if (dotDot == DotDot::Lexical && !needle(normal, part, root))
            fail(std::format("no needle for {} in the normal form", CheckoutPartName(part)));
    }
    return finding;
}

/// Figures over a set of compositions, and the first failures for the report.
struct CoverageTally
{
    std::size_t checked = 0;
    std::size_t mapped = 0;
    std::size_t bothRoots = 0;
    /// Reached a root and was not mapped: nothing found, or a mapping refused.
    std::size_t unmapped = 0;
    std::size_t failures = 0;
    std::string firstFailures;

    /// @param finding One composition's finding.
    void Count(CoverageFinding const& finding)
    {
        ++checked;
        if (finding.failure.has_value())
        {
            constexpr std::size_t Shown = 10;
            if (failures++ < Shown)
                firstFailures += *finding.failure + "\n";
        }
        if (!finding.reachesRoot)
            return;
        if (!finding.mapped)
        {
            ++unmapped;
            return;
        }
        ++mapped;
        if (finding.roots == 2)
            ++bothRoots;
    }
};

/// @param spelling A spelling. @return Its components, the drive's own (`C:`) first.
[[nodiscard]] std::vector<std::string> ComponentsOf(std::string_view spelling)
{
    std::vector<std::string> components;
    for (auto const component: spelling | std::views::split('\\'))
        if (!component.empty())
            components.emplace_back(component.begin(), component.end());
    return components;
}

/// @param components A drive and the components below it. @return The spelling they make.
[[nodiscard]] std::string SpellingOf(std::span<std::string const> components)
{
    std::string spelling = components.front() + "\\";
    for (auto const& component: components.subspan(1))
        spelling += spelling.ends_with('\\') ? component : "\\" + component;
    return spelling;
}

/// Every spelling of @p spelling with ONE dot segment put in after one of its components: a
/// `.`, a `zz\..` through a plain directory, and a `..` that re-enters the next component
/// (`L\..\L`) -- which a physical `..` reads as the directory holding `L`'s TARGET.
/// @param spelling A spelling without dot segments.
/// @return The dotted spellings.
[[nodiscard]] std::vector<std::string> DottedSpellings(std::string_view spelling)
{
    auto const components = ComponentsOf(spelling);
    std::vector<std::string> dotted;
    for (auto const at: std::views::iota(std::size_t { 1 }, components.size() + 1))
    {
        auto const insert = [&](std::initializer_list<std::string> segments) {
            std::vector<std::string> next(components.begin(), components.begin() + static_cast<std::ptrdiff_t>(at));
            next.insert(next.end(), segments);
            next.insert(next.end(), components.begin() + static_cast<std::ptrdiff_t>(at), components.end());
            dotted.push_back(SpellingOf(next));
        };
        insert({ "." });
        insert({ "zz", ".." });
        if (at < components.size())
            insert({ components[at], ".." });
    }
    return dotted;
}

} // namespace

TEST_CASE("For every root any ancestor resolves into, an ancestor is recorded -- over every enumerated link composition")
{
    // The class, not a fifth shape: four shapes of one defect were found by hand (a second
    // link below the alias, an alias through the build tree into the source root, through
    // a SUBDIRECTORY of it, an 8.3 spelling). This enumerates compositions of links and
    // asserts the class's rule of every spelling the key maps: for each root that ANY
    // ancestor resolves into, some ancestor is recorded for that root, or spells it.
    auto compositions = EnumeratedCompositions();
    auto const generated = compositions.size();
    auto const seeds = SeedCompositions();
    for (auto const& [shape, composition]: seeds)
    {
        INFO("seed " << shape);
        // Every seed resolves into a root, so every one must be mapped -- a seed the key
        // left alone would pass the invariant below vacuously.
        LinkResolver resolver { composition.links };
        RootReconciler reconciler { std::string { GeneratedSource }, std::string { GeneratedBuild }, resolver };
        CHECK(reconciler.Directory(composition.spelling) != composition.spelling);
        compositions.push_back(composition);
    }

    CoverageTally tally;
    for (auto const& composition: compositions)
        tally.Count(CheckCoverage(composition, DotDot::Lexical));
    WARN(std::format("link compositions checked: {} ({} enumerated, {} seeds), mapped by the key: {}, naming both "
                     "roots: {}",
                     tally.checked,
                     generated,
                     seeds.size(),
                     tally.mapped,
                     tally.bothRoots));
    INFO(tally.firstFailures);
    CHECK(tally.failures == 0);
    // Pinned EXACTLY, not as floors: a change that maps fewer spellings -- or more -- is a
    // change to what the key shares, and is read here rather than passed over.
    CHECK(tally.checked == 6784);
    CHECK(generated == 6776);
    CHECK(tally.mapped == 5816);
    CHECK(tally.bothRoots == 4712);
}

TEST_CASE("Every root a dotted spelling passes through has a needle in the spelling each compiler writes")
{
    // A compiler writes an `-I` spelling VERBATIM, `.` and `..` kept -- clang-cl, and `cl`
    // with no debug flag -- or collapsed (`cl` under `/Z7`, `/Zi`, `/ZI` or `/FC`), measured.
    // Recording only the normal form -- which the walk had always asked about -- left
    // `jx\zz\..\L1` without a needle a verbatim object contains, measured as a WRONG
    // HIT: judged portable, build directory 2 served build directory 1's spelling. The case
    // above shares that premise (it asks about normal forms only), so it could not see it.
    //
    // Every enumerated composition of at most TWO links, and every seed, respelled with one
    // dot segment at every position, read both ways a filesystem reads `..`. The coverage is
    // asserted against the spelling AS WRITTEN, and on the lexical reading against the normal
    // form as well. Two links, because the third multiplies the run by fifteen for shapes
    // (twice through, through and back) the seeds already name; measured, every composition
    // took 157 s on a Debug build.
    constexpr std::size_t MostLinks = 2;
    std::vector<Composition> bases;
    std::ranges::copy_if(EnumeratedCompositions(), std::back_inserter(bases), [](Composition const& composition) {
        return composition.links.size() <= MostLinks;
    });
    for (auto const& [shape, composition]: SeedCompositions())
        bases.push_back(composition);
    std::vector<Composition> dotted;
    for (auto const& base: bases)
        for (auto const& spelling: DottedSpellings(base.spelling))
            dotted.push_back(Composition { .links = base.links, .spelling = spelling });
    // And a `..` from the anchor link into a SIBLING link, `C:\a\L\..\y`: read physically it
    // ascends from `L`'s TARGET, so the path passes through `L` and leads elsewhere than its
    // normal form `C:\a\y`. The shape where a mapping accepted from the normal form alone
    // leaves the root `L` reaches without a needle -- which only a sibling can produce.
    constexpr std::string_view Anchor = R"(C:\a\L)";
    for (auto const& base: bases)
    {
        if (!base.spelling.starts_with(Anchor))
            continue;
        for (auto const& target: GeneratedTargets())
        {
            Composition sibling { .links = base.links,
                                  .spelling = std::format(R"({}\..\y{})", Anchor, base.spelling.substr(Anchor.size())) };
            sibling.links[R"(C:\a\y)"] = target;
            dotted.push_back(std::move(sibling));
        }
    }
    std::array<CoverageTally, 2> tallies {};
    for (auto const& composition: dotted)
    {
        tallies[0].Count(CheckCoverage(composition, DotDot::Lexical));
        tallies[1].Count(CheckCoverage(composition, DotDot::Physical));
    }
    constexpr auto Readings = std::to_array<std::string_view>({ "lexical", "physical" });
    for (auto const index: std::views::iota(std::size_t { 0 }, tallies.size()))
    {
        auto const dotDot = Readings[index];
        auto const& tally = tallies[index];
        INFO(dotDot << " `..`");
        WARN(std::format("{} `..`: dotted spellings checked: {}, mapped by the key: {}, naming both roots: {}, "
                         "reaching a root unmapped: {}",
                         dotDot,
                         tally.checked,
                         tally.mapped,
                         tally.bothRoots,
                         tally.unmapped));
        INFO(tally.firstFailures);
        CHECK(tally.failures == 0);
    }
    // Pinned EXACTLY. The readings part where a `..` follows a link: read physically it
    // ascends from the link's TARGET, so the path leads elsewhere than its normal form, and
    // the key maps fewer of those spellings. No sibling spelling is mapped on the physical
    // reading: the physical `mapped` is the dotted spellings' alone.
    CHECK(tallies[0].checked == 6718);
    CHECK(tallies[0].mapped == 5782);
    CHECK(tallies[0].bothRoots == 3802);
    CHECK(tallies[0].unmapped == 552);
    CHECK(tallies[1].checked == 6718);
    CHECK(tallies[1].mapped == 3794);
    CHECK(tallies[1].bothRoots == 2658);
    CHECK(tallies[1].unmapped == 2492);
}

TEST_CASE("An alias spelled with a dot segment is recorded as spelled and in its normal form")
{
    // rev-filemacro's final check, measured on clang-cl as a WRONG HIT: `jx\L1` a junction to
    // build directory 1 and `-I<run>\jx\zz\..\L1\x\y\gen`. clang-cl's object carries the
    // spelling verbatim, so a needle recorded only as `jx\L1` was never found in it, and build
    // directory 2 printed build directory 1's path; `cl /FC` collapses it, which is why the
    // normal form is recorded too.
    std::string const source { GeneratedSource };
    std::string const build { GeneratedBuild };
    struct Shape
    {
        std::string_view name;
        std::map<std::string, std::string> links;
        std::string spelling;
        std::string mapped;
        /// Each part, and the alias it must carry in both spellings.
        std::vector<std::tuple<CheckoutPart, std::string, std::string>> aliases;
    };
    auto const shapes = std::to_array<Shape>({
        { .name = "dotdot",
          .links = { { R"(C:\run\jx\L1)", build } },
          .spelling = R"(C:\run\jx\zz\..\L1\x\y\gen)",
          .mapped = R"(C:\run\ck\b1\x\y\gen)",
          .aliases = { { CheckoutPart::BuildTree, R"(C:\run\jx\zz\..\L1)", R"(C:\run\jx\L1)" },
                       { CheckoutPart::SourceRoot, R"(C:\run\jx\zz\..\L1)", R"(C:\run\jx\L1)" } } },
        { .name = "dotseg",
          .links = { { R"(C:\run\jx\L1)", build } },
          .spelling = R"(C:\run\jx\.\L1\x\y\gen)",
          .mapped = R"(C:\run\ck\b1\x\y\gen)",
          .aliases = { { CheckoutPart::BuildTree, R"(C:\run\jx\.\L1)", R"(C:\run\jx\L1)" },
                       { CheckoutPart::SourceRoot, R"(C:\run\jx\.\L1)", R"(C:\run\jx\L1)" } } },
        { .name = "dotdotsub",
          .links = { { R"(C:\run\jx\L1)", R"(C:\run\ck\b1\x)" }, { R"(C:\run\ck\b1\x\srclink)", source } },
          .spelling = R"(C:\run\jx\zz\..\L1\srclink\src\gen)",
          .mapped = R"(C:\run\ck\src\gen)",
          .aliases = { { CheckoutPart::BuildTree, R"(C:\run\jx\zz\..\L1)", R"(C:\run\jx\L1)" },
                       { CheckoutPart::SourceRoot, R"(C:\run\jx\zz\..\L1\srclink)", R"(C:\run\jx\L1\srclink)" } } },
    });
    for (auto const& shape: shapes)
    {
        INFO(shape.name);
        LinkResolver resolver { shape.links };
        RootReconciler reconciler { source, build, resolver };
        CHECK(reconciler.Directory(shape.spelling) == shape.mapped);
        for (auto const& [part, spelled, normal]: shape.aliases)
        {
            INFO(CheckoutPartName(part));
            CHECK(reconciler.Aliases().Contains(part, spelled));
            CHECK(reconciler.Aliases().Contains(part, normal));
        }
        CHECK(reconciler.Aliases().Entries().size() == 2 * shape.aliases.size());
    }
}

TEST_CASE("A spelling whose dot segment leads elsewhere than its normal form is not mapped")
{
    // `C:\a\L` a link to `b1\x`, a subdirectory of the build tree, and `C:\a\y` one to the
    // source root. Where a `..` is read from where the path has LED (POSIX), `C:\a\L\..\y` is
    // `b1\y` -- the key maps it into the build tree -- while its normal form `C:\a\y` is the
    // source root, and a walk of normal forms finds only that. Recorded, the object would
    // be bound to the source root alone under a build-tree key, and another build directory's
    // `L` served this one's spelling. The walk asked about a directory the path does not pass
    // through, so the mapping is refused.
    std::map<std::string, std::string> const links {
        { R"(C:\a\L)", R"(C:\run\ck\b1\x)" },
        { R"(C:\a\y)", R"(C:\run\ck\src)" },
    };
    constexpr auto Spelling = R"(C:\a\L\..\y)";
    {
        LinkResolver resolver { links, DotDot::Physical };
        RootReconciler reconciler { std::string { GeneratedSource }, std::string { GeneratedBuild }, resolver };
        CHECK(resolver.Real(Spelling) == R"(C:\run\ck\b1\y)");
        CHECK(reconciler.Directory(Spelling) == Spelling);
    }
    // The control: where `..` is read from the spelling (Windows), both forms lead to the
    // source root, and the path is mapped and recorded in both.
    LinkResolver resolver { links, DotDot::Lexical };
    RootReconciler reconciler { std::string { GeneratedSource }, std::string { GeneratedBuild }, resolver };
    CHECK(reconciler.Directory(Spelling) == R"(C:\run\ck\src)");
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, Spelling));
    CHECK(reconciler.Aliases().Contains(CheckoutPart::SourceRoot, R"(C:\a\y)"));
}

TEST_CASE("A path under neither root keeps its exact bytes")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    // A toolchain header is covered by the compiler identity, and rewriting its
    // spelling would put this machine's install prefix into a stored value.
    constexpr auto sdk = R"(C:\Program Files (x86)\Windows Kits\10\um\windows.h)";
    CHECK(reconciler.Path(sdk) == sdk);
}

TEST_CASE("A trailing separator on a root does not double in the translated path")
{
    // Untrimmed, the as-given test fails (IsSegmentPrefix wants a separator AFTER
    // the root), the resolved round trip then matches, and JoinLocalized adds a
    // second separator -- a depfile rule target the build never asked for.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src\)", R"(C:\Users\RUNNER~1\p\build\)", resolver };

    auto const translated = reconciler.Path(R"(C:\Users\runneradmin\p\build\a.o)");
    CHECK(translated == R"(C:\Users\RUNNER~1\p\build\a.o)");
    CHECK_FALSE(translated.contains(R"(\\)"));
}

TEST_CASE("Region reconciles a depfile's dependencies and preserves the named output")
{
    // The rule target is the `-o` path the BUILD SYSTEM named; respelling it hands
    // back an output it never asked for, which Ninja rejects outright. Named by
    // value, because -MP's phony rules put a compiler-reported HEADER in target
    // position and that one must be reconciled like any other.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    constexpr std::string_view depFile = "C:\\Users\\runneradmin\\p\\build\\a.o: C:\\Users\\runneradmin\\p\\src\\a.cpp\\\n"
                                         "  C:\\Users\\runneradmin\\p\\src\\inc\\h1.h\n"
                                         "\n"
                                         "C:\\Users\\runneradmin\\p\\src\\inc\\h1.h:\n";

    std::vector<std::string> const targets { R"(C:\Users\runneradmin\p\build\a.o)" };
    auto const out = reconciler.Region(depFile, PathCanon::Grammar::GccDepfile, targets);
    CHECK(out
          == "C:\\Users\\runneradmin\\p\\build\\a.o: C:\\Users\\RUNNER~1\\p\\src\\a.cpp\\\n"
             "  C:\\Users\\RUNNER~1\\p\\src\\inc\\h1.h\n"
             "\n"
             "C:\\Users\\RUNNER~1\\p\\src\\inc\\h1.h:\n");
}

TEST_CASE("Region reconciles a showIncludes stream and leaves everything else alone")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    constexpr std::string_view region = "char const* s = \"Note: including file: x\";\n"
                                        "Note: including file: C:\\Users\\runneradmin\\p\\src\\inc\\h1.h\r\n";
    CHECK(reconciler.Region(region, PathCanon::Grammar::ShowIncludes)
          == "char const* s = \"Note: including file: x\";\n"
             "Note: including file: C:\\Users\\RUNNER~1\\p\\src\\inc\\h1.h\r\n");
}

TEST_CASE("IsInTree answers the question the key asks, relative paths included")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    CHECK(reconciler.IsInTree(R"(C:\Users\runneradmin\p\src\a.cpp)")); // only after reconciling
    CHECK(reconciler.IsInTree(R"(C:\Users\RUNNER~1\p\src\a.cpp)"));
    CHECK_FALSE(reconciler.IsInTree(R"(D:\elsewhere\a.cpp)"));

    // A relative path resolves against the compile's working directory, so it is
    // already machine-independent -- which is how KeyDependencySet and ReplayGuard
    // both treat one. A third answer here would make the launcher contradict
    // itself about one path, and would put the roots-mismatch note on every compile of
    // a build that passes relative sources.
    CHECK(reconciler.IsInTree("src/a.cpp"));
    CHECK_FALSE(reconciler.IsInTree(""));
}

TEST_CASE("DependencyList reconciles a whole dependency list in place")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    std::vector<std::string> paths { R"(C:\Users\runneradmin\p\src\a.cpp)",
                                     R"(C:\Windows Kits\10\um\windows.h)",
                                     "inc/rel.hpp" };
    reconciler.DependencyList(paths);
    CHECK(paths[0] == R"(C:\Users\RUNNER~1\p\src\a.cpp)");
    CHECK(paths[1] == R"(C:\Windows Kits\10\um\windows.h)"); // outside both roots
    CHECK(paths[2] == R"(inc\rel.hpp)");                     // relative, spelled the layout's way
}

TEST_CASE("A dependency list is collapsed after it is reconciled")
{
    // Reconciled first, so the collapse works on the spelling every other path in this build
    // uses: the aliased prefix is rewritten AND the `..` taken out, in one call.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };
    std::vector<std::string> paths { R"(C:\Users\runneradmin\p\build\..\src\inc\a.hpp)", R"(..\..\inc\rel.hpp)" };
    reconciler.DependencyList(paths);
    CHECK(paths[0] == R"(C:\Users\RUNNER~1\p\src\inc\a.hpp)");
    CHECK(paths[1] == R"(..\..\inc\rel.hpp)"); // nothing lexical resolves it, so verbatim
}

TEST_CASE("One header reached by two include chains is one note and one depfile entry")
{
    // #1592 pinned the opposite as accepted: RenderShowIncludes dedups byte-exactly and ran BEFORE
    // any collapse, so two spellings of one header rendered two identical notes. The dependency
    // list is now collapsed where it enters (#1593), so the renderers' own dedup sees one header.
    //
    // The two spellings are `cl`'s own, measured (VS 18, `/c` and `/EP` alike): the direct include
    // is `<root>\a/x.h`, the one through `b/y.h` is `<root>\b\../a/x.h` -- they differ in the
    // separators as well as in the `..`, so a collapse alone would leave two.
    FakeResolver resolver { {} };
    RootReconciler reconciler { R"(D:\s)", R"(D:\s\build)", resolver };
    std::vector<std::string> deps { R"(D:\s\a/x.h)", R"(D:\s\b/y.h)", R"(D:\s\b\../a/x.h)", R"(D:\s\build\..\inc\y.h)" };
    reconciler.DependencyList(deps);

    CHECK(ParseIncludePaths(RenderShowIncludes(deps, IncludeNoteMarker))
          == std::vector<std::string> { R"(D:\s\a\x.h)", R"(D:\s\b\y.h)", R"(D:\s\inc\y.h)" });

    auto const depfile = RenderDepFile("D:/s/build/u.obj", deps);
    INFO(depfile);
    std::size_t mentions = 0;
    std::size_t at = depfile.find("x.h");
    while (at != std::string::npos)
    {
        ++mentions;
        at = depfile.find("x.h", at + 1);
    }
    CHECK(mentions == 1);
    CHECK_FALSE(depfile.contains(".."));
}

TEST_CASE("A POSIX dependency list keeps its backslashes, which are filename bytes there")
{
    FakeResolver resolver { {} };
    RootReconciler reconciler { "/s", "/s/build", resolver };
    std::vector<std::string> deps { "/s/b/../a/x.h", R"(/s/odd\name.h)" };
    reconciler.DependencyList(deps);
    CHECK(deps == std::vector<std::string> { "/s/a/x.h", R"(/s/odd\name.h)" });
}

TEST_CASE("An empty root is a prefix of nothing, not of everything")
{
    // RunCached refuses the cache outright without both roots, so this state does
    // not reach the launcher's flow -- but an empty string is a prefix of every
    // string, and a reconciler that took that literally would rewrite every path
    // it saw. PathCanon's IsSegmentPrefix is what makes it not, and this pins that
    // the reconciler inherits the answer rather than reimplementing it.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { "", "", resolver };
    constexpr auto path = R"(C:\Users\runneradmin\p\src\a.cpp)";
    CHECK(reconciler.Path(path) == path);
    CHECK(reconciler.Region("Note: including file: C:\\x\\a.h\r\n", PathCanon::Grammar::ShowIncludes)
          == "Note: including file: C:\\x\\a.h\r\n");
}

TEST_CASE("Where bytes are bytes, a legacy path is reconciled exactly as any other")
{
    // POSIX, and a pre-1903 Windows: nothing is transcoded, so a filename spelled
    // in a legacy encoding names a file perfectly well. Refusing it here would
    // break builds that work today, which is why `pathsAreUtf8` is a property of
    // the HOST rather than a rule the launcher applies everywhere.
    auto resolver = FakeResolver { {} };
    RootReconciler reconciler { "/x/src", "/x/build", resolver, FastCache::NarrowTextPolicy {} };

    CHECK(reconciler.Path("/x/src/gr\xFC"
                          "n/a.h")
          == "/x/src/gr\xFC"
             "n/a.h");
    CHECK(reconciler.UnreadablePaths() == 0);
}

TEST_CASE("Where a path must be UTF-8, a tool's own code page is what reads it")
{
    // The split this closes: the roots come from `argv`, which on a host declaring
    // the UTF-8 code page is UTF-8, while `cl` writes `/showIncludes` in the console
    // output code page. Untranslated, the two stop prefix-matching, so a project
    // header keys as toolchain content -- and a header moved inside it then replays
    // a stored object under a zero exit code.
    auto resolver = FakeResolver { {} };
    RootReconciler reconciler {
        "/x/src", "/x/build", resolver, FastCache::NarrowTextPolicy { .pathsAreUtf8 = true, .toolCodePage = 1252U }
    };

    // Already UTF-8: through untouched, on every platform, and the case a modern
    // console actually produces.
    CHECK(reconciler.Path("/x/src/gr\xC3\xBC"
                          "n/a.h")
          == "/x/src/gr\xC3\xBC"
             "n/a.h");
    CHECK(reconciler.UnreadablePaths() == 0);

#if defined(_WIN32)
    // CP-1252 in, UTF-8 out, so it prefix-matches the root that came from argv.
    // Windows-only because there is no transcoder anywhere else -- and nowhere else
    // is a code page ever named, so nothing else can reach this.
    CHECK(reconciler.Path("/x/src/gr\xFC"
                          "n/a.h")
          == "/x/src/gr\xC3\xBC"
             "n/a.h");
    CHECK(reconciler.UnreadablePaths() == 0);
#endif
}

TEST_CASE("A path this host cannot read as text is returned verbatim and counted")
{
    // Verbatim and UNTRANSLATED, because translating means resolving and resolving
    // means `std::filesystem::path` -- which on a host that reads narrow bytes as
    // UTF-8 throws on these rather than mis-naming a file. The count is what
    // main.cpp declines the compile on; nothing here decides that.
    auto resolver = FakeResolver { {} };
    RootReconciler reconciler { "/x/src", "/x/build", resolver, FastCache::NarrowTextPolicy { .pathsAreUtf8 = true } };

    CHECK(reconciler.Path("/x/src/gr\xFC"
                          "n/a.h")
          == "/x/src/gr\xFC"
             "n/a.h");
    CHECK(reconciler.UnreadablePaths() == 1);

    // Counted per occurrence, and reached through `DependencyList` and `Region` too: `Path` is
    // the funnel all three share, which is what lets one count cover the include
    // notes, the depfile entries and the stored regions alike.
    std::vector<std::string> paths { "/x/src/gr\xFC"
                                     "n/a.h",
                                     "/x/src/plain.h" };
    reconciler.DependencyList(paths);
    CHECK(reconciler.UnreadablePaths() == 2);
    CHECK(paths[0]
          == "/x/src/gr\xFC"
             "n/a.h");
    CHECK(paths[1] == "/x/src/plain.h");
}
