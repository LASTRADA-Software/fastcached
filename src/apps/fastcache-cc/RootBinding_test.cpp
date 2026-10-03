// SPDX-License-Identifier: Apache-2.0
#include "FileBytes.hpp"
#include "IProcessRunner.hpp"
#include "RootBinding.hpp"
#include "RootReconciler.hpp"
#include "StubCoffTestSupport.hpp"
#include "StubElfMachOTestSupport.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using namespace FastCache::Cc::Test;

namespace
{

/// The two checkouts every synthetic case below stands in for.
constexpr std::string_view RootA = R"(D:\work\checkout-a)";
constexpr std::string_view RootB = R"(D:\work\checkout-b)";

/// @param root A source root. @return The layout of a checkout at @p root.
[[nodiscard]] PathCanon::Layout LayoutAt(std::string_view root)
{
    return PathCanon::Layout { .sourceRoot = std::string { root }, .buildTree = std::string { root } + R"(\build)" };
}

/// A checkout whose roots and working directory are spelled one way: the build tree is
/// the working directory, as a CMake build's is.
/// @param source The source root. @param build The build tree. @param cwd The working directory.
[[nodiscard]] CheckoutRoots CheckoutWith(std::string_view source, std::string_view build, std::string_view cwd)
{
    auto const layout = PathCanon::Layout { .sourceRoot = std::string { source }, .buildTree = std::string { build } };
    return CheckoutRoots { .exported = layout,
                           .resolved = layout,
                           .workingDirectory = std::string { cwd },
                           .resolvedWorkingDirectory = std::string { cwd },
                           .compilerWorkingDirectory = std::string { cwd } };
}

/// @param root A source root. @return A checkout at @p root, compiling in its build tree.
[[nodiscard]] CheckoutRoots CheckoutAt(std::string_view root)
{
    auto const layout = LayoutAt(root);
    return CheckoutWith(layout.sourceRoot, layout.buildTree, layout.buildTree);
}

/// @param root A source root. @return The spellings a launcher at @p root scans for.
[[nodiscard]] std::vector<std::string> RootsOf(std::string_view root)
{
    return RootSpellings(CheckoutAt(root));
}

/// A resolver that answers every path as spelled, counting nothing -- the checkout under
/// test has no aliases, so what `BindCheckout` itself does is what is measured.
class IdentityResolver final: public IPathResolver
{
  public:
    std::string Resolve(std::string_view path) override
    {
        return std::string { path };
    }

    std::string ResolveDirectory(std::string_view path) override
    {
        return std::string { path };
    }

    std::string ShortForm(std::string_view directory) override
    {
        return std::string { directory };
    }

    [[nodiscard]] std::size_t FilesystemCalls() const noexcept override
    {
        return 0;
    }
};

/// A resolver driven by two tables: which prefixes are aliases of which directories, and
/// which directories have an 8.3 short form. Longest prefix first, so an alias of a
/// directory and an alias of its parent compose as a filesystem would.
class AliasResolver final: public IPathResolver
{
  public:
    /// @param aliases Alias prefix -> the directory it names.
    /// @param shortForms Directory -> its short form.
    AliasResolver(std::map<std::string, std::string> aliases, std::map<std::string, std::string> shortForms):
        _aliases { std::move(aliases) },
        _shortForms { std::move(shortForms) }
    {
    }

    std::string Resolve(std::string_view path) override
    {
        return Rewrite(path);
    }

    std::string ResolveDirectory(std::string_view path) override
    {
        return Rewrite(path);
    }

    std::string ShortForm(std::string_view directory) override
    {
        auto const found = _shortForms.find(std::string { directory });
        return found == _shortForms.end() ? std::string { directory } : found->second;
    }

    [[nodiscard]] std::size_t FilesystemCalls() const noexcept override
    {
        return 0;
    }

  private:
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

    std::map<std::string, std::string> _aliases;
    std::map<std::string, std::string> _shortForms;
};

/// A checkout bound the way `main` binds one: through a reconciler, whose alias list the
/// checkout then reads -- so the reconciler lives exactly as long as the checkout.
class BoundCheckout
{
  public:
    /// @param exported The roots as exported. @param cwd The working directory.
    /// @param compilerCwd The working directory as a compiler spells it.
    /// @param resolver The filesystem.
    BoundCheckout(PathCanon::Layout const& exported,
                  std::string_view cwd,
                  std::string_view compilerCwd,
                  IPathResolver& resolver):
        reconciler { exported.sourceRoot, exported.buildTree, resolver },
        checkout { BindCheckout(reconciler, cwd, compilerCwd) }
    {
    }

    BoundCheckout(BoundCheckout const&) = delete;
    BoundCheckout& operator=(BoundCheckout const&) = delete;
    BoundCheckout(BoundCheckout&&) = delete;
    BoundCheckout& operator=(BoundCheckout&&) = delete;
    ~BoundCheckout() = default;

    RootReconciler reconciler; ///< Declared first: the checkout reads its list.
    CheckoutRoots checkout;    ///< The bound checkout.
};

/// @param text Narrow text. @return Its bytes.
[[nodiscard]] std::string Narrow(std::string_view text)
{
    return std::string { text };
}

/// @param text ASCII text. @return It as UTF-16LE, one unit per character.
[[nodiscard]] std::string Utf16(std::string_view text)
{
    std::string out;
    for (auto const c: text)
    {
        out.push_back(c);
        out.push_back('\0');
    }
    return out;
}

/// @param text ASCII text. @return It as UTF-32LE, one unit per character.
[[nodiscard]] std::string Utf32(std::string_view text)
{
    std::string out;
    for (auto const c: text)
    {
        out.push_back(c);
        out.append(3, '\0');
    }
    return out;
}

/// Scan a `cl`-shaped COFF object whose `.rdata` holds @p rdata and whose path record
/// holds @p debug.
[[nodiscard]] RootBindingScan ScanCoff(std::string rdata, std::string debug, std::string_view root = RootA)
{
    auto const image = BuildCoff({ { .name = ".drectve", .data = "-defaultlib:libcpmt" },
                                   { .name = ".debug$S", .data = std::move(debug) },
                                   { .name = ".rdata", .data = std::move(rdata) },
                                   { .name = ".text$mn", .data = "CODE" } },
                                 1000);
    return ScanForRoots(image, RootsOf(root));
}

/// The fetch a test drives `ResolveFetchedObject` with: a map standing in for the cache.
[[nodiscard]] ValueFetch FetchFrom(std::map<std::string, std::vector<std::byte>> const& cache)
{
    return [&cache](std::string const& key) -> std::optional<std::vector<std::byte>> {
        auto const found = cache.find(key);
        if (found == cache.end())
            return std::nullopt;
        return found->second;
    };
}

/// An encoded object value carrying @p object, shaped the way the launcher stores one:
/// two stream regions, stdout and stderr.
[[nodiscard]] std::vector<std::byte> ObjectValue(std::string_view object)
{
    CompileValue value;
    std::ranges::transform(object, std::back_inserter(value.objectBlob), [](char c) { return static_cast<std::byte>(c); });
    value.textRegions.push_back({ .grammar = PathCanon::Grammar::ShowIncludes, .bytes = {} });
    value.textRegions.push_back({ .grammar = PathCanon::Grammar::ShowIncludes, .bytes = {} });
    return EncodeCompileValue(value);
}

/// @param value A decoded value. @return Its object blob as text.
[[nodiscard]] std::string BlobText(CompileValue const& value)
{
    return std::string { reinterpret_cast<char const*>(value.objectBlob.data()), value.objectBlob.size() };
}

} // namespace

TEST_CASE("An object naming the source root in program data is root-bound", "[launcher][rootbinding]")
{
    auto const scan = ScanCoff(R"(D:\work\checkout-a\src\tu.cpp)", "C:\\elsewhere\\tu.obj");
    CHECK(scan.binding == RootBinding::Bound);
    CHECK(scan.evidence.contains(".rdata"));
    CHECK(scan.evidence.contains("narrow"));
}

TEST_CASE("An object naming the root only in its debug records is portable", "[launcher][rootbinding]")
{
    // #203's accepted cost: `.debug$S` names the producing checkout on every compile with
    // debug info. Counting it would end cross-checkout sharing for every debug build.
    auto const scan = ScanCoff("ordinary data", R"(D:\work\checkout-a\build\tu.obj)");
    CHECK(scan.binding == RootBinding::Portable);
    CHECK(scan.evidence.empty());
}

TEST_CASE("Another checkout's root does not bind an object to this one", "[launcher][rootbinding]")
{
    // The object names checkout-a; the launcher scanning it lives in checkout-b.
    CHECK(ScanCoff(R"(D:\work\checkout-a\src\tu.cpp)", {}, RootB).binding == RootBinding::Portable);
}

TEST_CASE("Every spelling a compiler writes of a root is found", "[launcher][rootbinding]")
{
    // A spelling this misses is a root-bound object served into another checkout: wrong
    // and looking right. Each row is one a compiler or a build system produces.
    for (auto const& spelling: { std::string { R"(D:\work\checkout-a\src\tu.cpp)" },
                                 std::string { "D:/work/checkout-a/src/tu.cpp" },
                                 std::string { R"(d:\work\checkout-a\src\tu.cpp)" }, // `/FC` lower-cases
                                 std::string { R"(D:\WORK\CHECKOUT-A\SRC\TU.CPP)" },
                                 std::string { R"(D:\\work\\checkout-a\\src\\tu.cpp)" }, // a stringized literal
                                 std::string { R"(D:/work\checkout-a/src\tu.cpp)" } })   // mixed separators
    {
        INFO("spelling: " << spelling);
        CHECK(ScanCoff(Narrow(spelling), {}).binding == RootBinding::Bound);
    }
}

TEST_CASE("A wide string naming the root is found at every alignment", "[launcher][rootbinding]")
{
    // `L"" __FILE__` and MSVC's `assert` are UTF-16LE; a Linux `wchar_t` is 32 bits. The
    // odd-offset rows are the alignments a string inside a larger structure can land on.
    auto const path = std::string { R"(D:\work\checkout-a\src\tu.cpp)" };
    for (auto const& [name, bytes]: { std::pair { "UTF-16LE", Utf16(path) },
                                      std::pair { "UTF-16LE, odd offset", "x" + Utf16(path) },
                                      std::pair { "UTF-32LE", Utf32(path) },
                                      std::pair { "UTF-32LE, offset 3", "xyz" + Utf32(path) } })
    {
        INFO(name);
        auto const scan = ScanCoff(bytes, {});
        CHECK(scan.binding == RootBinding::Bound);
    }
}

TEST_CASE("A non-ASCII root is found in any encoding the compiler chose", "[launcher][rootbinding]")
{
    // `cl` without `/utf-8` writes `__FILE__` in the EXECUTION character set, so the
    // root's `ü` arrives as one code-page byte, two UTF-8 bytes or one UTF-16 unit.
    constexpr std::string_view Root = "D:\\work\\m\xC3\xBCller"; // UTF-8, as argv delivers it
    auto const roots = RootsOf(Root);
    for (auto const& [name, data]:
         { std::pair { "UTF-8", std::string { "D:\\work\\m\xC3\xBCller\\tu.cpp" } },
           std::pair { "code page 1252", std::string { "D:\\work\\m\xFCller\\tu.cpp" } },
           std::pair { "UTF-16LE", Utf16("D:\\work\\m") + std::string { "\xFC\x00", 2 } + Utf16("ller") } })
    {
        INFO(name);
        auto const image = BuildCoff({ { .name = ".rdata", .data = data } }, 1000);
        CHECK(ScanForRoots(image, roots).binding == RootBinding::Bound);
    }
}

TEST_CASE("A code-page substitution for a root character is matched", "[launcher][rootbinding]")
{
    // `cl` without `/utf-8` writes `?` for each root character its code page cannot hold --
    // measured: a checkout under `k检-aaa`, ACP 1252, gave `k?-aaa` in `.rdata`. The scan
    // took it for portable. A `?` in the OBJECT now matches any one unit of the root.
    constexpr std::string_view Root = "D:\\work\\k\xE6\xA3\x80-aaa"; // UTF-8, as argv delivers it
    auto const substituted = BuildCoff({ { .name = ".rdata", .data = R"(D:\work\k?-aaa\src\probe.cpp)" } }, 1000);
    CHECK(ScanForRoots(substituted, RootsOf(Root)).binding == RootBinding::Bound);

    // Any single character, ASCII included -- broad, as the scan must be.
    auto const ascii = RootsOf(R"(D:\work\kx-aaa)");
    CHECK(ScanForRoots(substituted, ascii).binding == RootBinding::Bound);

    // The control: another character where the root has one is still another checkout.
    auto const other = BuildCoff({ { .name = ".rdata", .data = R"(D:\work\kQ-aaa\src\probe.cpp)" } }, 1000);
    CHECK(ScanForRoots(other, ascii).binding == RootBinding::Portable);
}

TEST_CASE("The resolved spelling of a root is scanned as well as the exported one", "[launcher][rootbinding]")
{
    // A compiler writes whichever spelling it was handed. A build exporting a `subst`
    // drive while the compile line names the real path is the ordinary shape.
    auto const exported = LayoutAt(R"(S:\proj)");
    auto const resolved = LayoutAt(R"(D:\real\proj)");
    auto const checkout = CheckoutRoots { .exported = exported,
                                          .resolved = resolved,
                                          .workingDirectory = exported.buildTree,
                                          .resolvedWorkingDirectory = resolved.buildTree,
                                          .compilerWorkingDirectory = exported.buildTree };
    auto const roots = RootSpellings(checkout);
    CHECK(roots.size() == 4);

    auto const image = BuildCoff({ { .name = ".rdata", .data = R"(D:\real\proj\src\tu.cpp)" } }, 1000);
    CHECK(ScanForRoots(image, roots).binding == RootBinding::Bound);
    // And without the resolved spelling the same object would pass for portable.
    CHECK(ScanForRoots(image, RootsOf(R"(S:\proj)")).binding == RootBinding::Portable);
}

TEST_CASE("RootSpellings drops empty roots and duplicates", "[launcher][rootbinding]")
{
    CHECK(RootSpellings(CheckoutWith("/w/src", "", "/w/src")) == std::vector<std::string> { "/w/src" });
    // A spelling still relative names no checkout -- and `.` would match nearly any object,
    // which is what a relative export did before it was made absolute.
    CHECK(RootSpellings(CheckoutWith(".", "build", ".")).empty());
    CHECK(RootSpellings(CheckoutWith(R"(D:\w)", "build", R"(D:\w)")) == std::vector<std::string> { R"(D:\w)" });
}

TEST_CASE("A relative root is made absolute against the working directory", "[launcher][rootbinding]")
{
    struct Row
    {
        std::string_view root;
        std::string_view cwd;
        std::string_view absolute;
    };
    constexpr auto Rows = std::to_array<Row>({
        { .root = ".", .cwd = R"(D:\work\checkout-a)", .absolute = R"(D:\work\checkout-a)" },
        { .root = "build", .cwd = R"(D:\work\checkout-a)", .absolute = R"(D:\work\checkout-a\build)" },
        { .root = R"(..\src)", .cwd = R"(D:\work\checkout-a\build)", .absolute = R"(D:\work\checkout-a\src)" },
        { .root = R"(E:\elsewhere)", .cwd = R"(D:\work\checkout-a)", .absolute = R"(E:\elsewhere)" },
        { .root = "E:drive-relative", .cwd = R"(D:\work\checkout-a)", .absolute = "E:drive-relative" },
        { .root = "./src", .cwd = "/home/ci/checkout-a/", .absolute = "/home/ci/checkout-a/src" },
        { .root = "", .cwd = "/home/ci/checkout-a", .absolute = "" },
    });
    for (auto const& row: Rows)
    {
        INFO(row.root << " against " << row.cwd);
        CHECK(AbsoluteAgainst(row.root, row.cwd) == row.absolute);
    }
}

TEST_CASE("Two checkouts exporting the same RELATIVE roots get different bound keys", "[launcher][rootbinding]")
{
    // Measured before this: `FASTCACHE_SOURCE_DIR=.` and `FASTCACHE_BINARY_DIR=build` folded
    // to ONE bound key in every checkout, so a marker led the second checkout to the first
    // one's copy -- `cl /FC` printed the first one's path under `(root-bound: served from ...)`.
    IdentityResolver resolver;
    auto const relative = PathCanon::Layout { .sourceRoot = ".", .buildTree = "build" };
    BoundCheckout const boundA { relative, R"(D:\work\checkout-a)", R"(D:\work\checkout-a)", resolver };
    auto const& a = boundA.checkout;
    BoundCheckout const boundB { relative, R"(D:\work\checkout-b)", R"(D:\work\checkout-b)", resolver };
    auto const& b = boundB.checkout;
    constexpr std::string_view Portable = "0123456789abcdef0123456789abcdef";
    for (auto const part: Enumerators<CheckoutPart>())
    {
        INFO(CheckoutPartName(part));
        auto const only = CheckoutParts {}.With(part);
        CHECK(ComputeRootBoundKey(Portable, a, only) != ComputeRootBoundKey(Portable, b, only));
    }

    // And what is scanned for is the absolute spelling, never the `.` as typed, which
    // would match nearly any object.
    auto const roots = RootSpellings(a);
    CHECK(std::ranges::find(roots, ".") == roots.end());
    CHECK(std::ranges::find(roots, R"(D:\work\checkout-a)") != roots.end());
    CHECK(std::ranges::find(roots, R"(D:\work\checkout-a\build)") != roots.end());
}

TEST_CASE("The working directory is scanned where no root covers it", "[launcher][rootbinding]")
{
    // Measured before this: roots `<checkout>\lib` and `<checkout>\build`, a relative
    // `cl /FC` compile from `<checkout>` -- `cl` absolutized `src\probe.cpp` against the
    // working directory, the object named `<checkout>\src\probe.cpp`, and it passed for
    // portable.
    IdentityResolver resolver;
    auto const narrow =
        PathCanon::Layout { .sourceRoot = R"(D:\work\checkout-a\lib)", .buildTree = R"(D:\work\checkout-a\build)" };
    BoundCheckout const boundCheckout { narrow, R"(D:\work\checkout-a)", R"(D:\work\checkout-a)", resolver };
    auto const& checkout = boundCheckout.checkout;
    auto const image = BuildCoff({ { .name = ".rdata", .data = R"(D:\work\checkout-a\src\probe.cpp)" } }, 1000);
    auto const scan = ScanCheckout(image, checkout);
    CHECK(scan.binding == RootBinding::Bound);
    // It names the working directory and nothing else, and says so.
    CHECK(scan.parts == CheckoutParts {}.With(CheckoutPart::WorkingDirectory));
    CHECK(scan.evidence.contains("bound to its working-directory"));

    // And the working directory is what the bound key then names.
    BoundCheckout const boundElsewhere { narrow, R"(D:\work\checkout-a\build)", R"(D:\work\checkout-a\build)", resolver };
    auto const& elsewhere = boundElsewhere.checkout;
    CHECK(ComputeRootBoundKey("0123456789abcdef0123456789abcdef", checkout, scan.parts)
          != ComputeRootBoundKey("0123456789abcdef0123456789abcdef", elsewhere, scan.parts));
}

TEST_CASE("The working directory is scanned as `$PWD` spells it, and keyed as it resolves", "[launcher][rootbinding]")
{
    // A build entered through a symlink: `getcwd(3)` answers the physical directory, and a
    // POSIX driver absolutizing `src/probe.cpp` against its directory writes the `$PWD`
    // spelling -- the one `CompilerWorkingDirectory` predicts, and the rulebook's "each end
    // predicts that directory from `$PWD`, not `getcwd(3)`".
    IdentityResolver resolver;
    auto const narrow = PathCanon::Layout { .sourceRoot = "/data/checkout-a/lib", .buildTree = "/data/checkout-a/build" };
    BoundCheckout const boundViaLink { narrow, "/data/checkout-a", "/home/dev/link-a", resolver };
    auto const& viaLink = boundViaLink.checkout;
    auto const image = BuildCoff({ { .name = ".rdata", .data = "/home/dev/link-a/src/probe.cpp" } }, 1000);
    auto const scan = ScanCheckout(image, viaLink);
    CHECK(scan.binding == RootBinding::Bound);
    CHECK(scan.parts == CheckoutParts {}.With(CheckoutPart::WorkingDirectory));

    // The control: the same checkout knowing only `getcwd`'s answer takes it for portable.
    BoundCheckout const boundPhysicalOnly { narrow, "/data/checkout-a", "/data/checkout-a", resolver };
    auto const& physicalOnly = boundPhysicalOnly.checkout;
    CHECK(ScanCheckout(image, physicalOnly).binding == RootBinding::Portable);

    // And the key is not the scan: it folds the RESOLVED directory, which both spellings
    // name, so a checkout entered through the link and one entered directly share a key.
    constexpr std::string_view Portable = "0123456789abcdef0123456789abcdef";
    CHECK(ComputeRootBoundKey(Portable, viaLink, scan.parts) == ComputeRootBoundKey(Portable, physicalOnly, scan.parts));
}

TEST_CASE("The scan looks for every alias the reconciler maps onto a root, from its one list", "[launcher][rootbinding]")
{
    // The part-keying review's I1, measured as a WRONG HIT: `-I` spelled wholly in 8.3 short
    // names, which the key resolved to the build tree while the scan knew only the long
    // spellings. Driven from the reconciler's own list rather than from a list of spellings
    // written here, so the property is "the scan reads what the key mapped", whatever that is.
    constexpr std::string_view Checkout = R"(C:\work\checkoutwithalongname)";
    constexpr std::string_view Build = R"(C:\work\checkoutwithalongname\buildverylongname1)";
    AliasResolver resolver {
        {
            { R"(C:\work\CHECKO~1)", std::string { Checkout } },
            { R"(C:\work\CHECKO~1\BUILDV~1)", std::string { Build } },
            { R"(S:\)", std::string { Checkout } + R"(\)" }, // `subst S: <checkout>`
        },
        {},
    };
    BoundCheckout bound { PathCanon::Layout { .sourceRoot = std::string { Checkout }, .buildTree = std::string { Build } },
                          Build,
                          Build,
                          resolver };

    // What the key does with the aliases: each maps onto a root, in the build's spelling.
    // The walk up a spelling stops at the NEAREST root, so the source root's alias is
    // recorded by a path under the source root and outside the build tree.
    CHECK(bound.reconciler.Directory(R"(C:\work\CHECKO~1\BUILDV~1\gen)") == std::string { Build } + R"(\gen)");
    CHECK(bound.reconciler.Directory(R"(C:\work\CHECKO~1\src\inc)") == std::string { Checkout } + R"(\src\inc)");
    CHECK(bound.reconciler.Directory(R"(S:\src\inc)") == std::string { Checkout } + R"(\src\inc)");

    // Each prefix it mapped is on the list, as the part it spells.
    auto const& aliases = bound.reconciler.Aliases();
    CHECK(aliases.Contains(CheckoutPart::BuildTree, R"(C:\work\CHECKO~1\BUILDV~1)"));
    CHECK(aliases.Contains(CheckoutPart::SourceRoot, R"(C:\work\CHECKO~1)"));
    CHECK(aliases.Contains(CheckoutPart::SourceRoot, R"(S:\)"));

    // And the scan finds every one of them, bound to the part it spells.
    REQUIRE_FALSE(aliases.Entries().empty());
    for (auto const& alias: aliases.Entries())
    {
        INFO(alias.spelling << " as " << CheckoutPartName(alias.part));
        // A drive's own root (`S:\`) already ends in its separator.
        auto named = alias.spelling;
        if (!named.ends_with('\\'))
            named.push_back('\\');
        named += R"(gen\g.h)";
        auto const scan = ScanCheckout(BuildCoff({ { .name = ".rdata", .data = named } }, 1000), bound.checkout);
        CHECK(scan.binding == RootBinding::Bound);
        CHECK(scan.parts.Contains(alias.part));
    }

    // The control: the same object against a checkout that reads no list is portable --
    // which is the wrong hit the list closes.
    auto unlisted = bound.checkout;
    unlisted.aliases = nullptr;
    auto const shortObject = BuildCoff({ { .name = ".rdata", .data = R"(C:\work\CHECKO~1\BUILDV~1\gen\g.h)" } }, 1000);
    CHECK(ScanCheckout(shortObject, unlisted).binding == RootBinding::Portable);
}

TEST_CASE("Every part's 8.3 short form is on the list before the compile maps anything", "[launcher][rootbinding]")
{
    // Seeded by `BindCheckout` through the resolver's `ShortForm` seam, so an object
    // naming a part's short form is bound even where no argument spelled it short.
    constexpr std::string_view Checkout = R"(C:\work\checkoutwithalongname)";
    constexpr std::string_view Build = R"(C:\work\checkoutwithalongname\buildverylongname1)";
    constexpr std::string_view Cwd = R"(C:\work\workingdirectory)";
    AliasResolver resolver {
        {},
        {
            { std::string { Checkout }, R"(C:\work\CHECKO~1)" },
            { std::string { Build }, R"(C:\work\CHECKO~1\BUILDV~1)" },
            { std::string { Cwd }, R"(C:\work\WORKIN~1)" },
        },
    };
    BoundCheckout bound {
        PathCanon::Layout { .sourceRoot = std::string { Checkout }, .buildTree = std::string { Build } }, Cwd, Cwd, resolver
    };
    struct Row
    {
        std::string_view named;
        CheckoutPart part;
    };
    constexpr auto Rows = std::to_array<Row>({
        { .named = R"(C:\work\CHECKO~1\src\u.cpp)", .part = CheckoutPart::SourceRoot },
        { .named = R"(C:\work\CHECKO~1\BUILDV~1\gen\g.h)", .part = CheckoutPart::BuildTree },
        { .named = R"(C:\work\WORKIN~1\src\probe.cpp)", .part = CheckoutPart::WorkingDirectory },
    });
    for (auto const& row: Rows)
    {
        INFO(row.named);
        auto const scan =
            ScanCheckout(BuildCoff({ { .name = ".rdata", .data = std::string { row.named } } }, 1000), bound.checkout);
        CHECK(scan.binding == RootBinding::Bound);
        CHECK(scan.parts.Contains(row.part));
    }
}

TEST_CASE("The scan reports exactly the parts of the checkout an object names", "[launcher][rootbinding]")
{
    // Three directories none of which contains another, so each spelling is one part --
    // except where the working directory IS the build tree, which is one spelling of both.
    auto const checkout = CheckoutWith(R"(D:\work\checkout-a)", R"(D:\out\a)", R"(D:\run\a)");
    auto const partsOf = [&checkout](std::string_view data) {
        return ScanCheckout(BuildCoff({ { .name = ".rdata", .data = std::string { data } } }, 1000), checkout).parts;
    };
    auto const none = CheckoutParts {};
    CHECK(partsOf(R"(D:\work\checkout-a\src\tu.cpp)") == none.With(CheckoutPart::SourceRoot));
    CHECK(partsOf(R"(D:\out\a\gen\config.h)") == none.With(CheckoutPart::BuildTree));
    CHECK(partsOf(R"(D:\run\a\probe.cpp)") == none.With(CheckoutPart::WorkingDirectory));
    // Two parts in two sections are both found; the scan does not stop at the first.
    auto const both = ScanCheckout(BuildCoff({ { .name = ".rdata", .data = R"(D:\work\checkout-a\src\tu.cpp)" },
                                               { .name = ".data", .data = R"(D:\out\a\gen\config.h)" } },
                                             1000),
                                   checkout);
    CHECK(both.parts == none.With(CheckoutPart::SourceRoot).With(CheckoutPart::BuildTree));
    CHECK(partsOf("CODE") == none);

    auto const cmake = CheckoutWith(R"(D:\work\checkout-a)", R"(D:\out\a)", R"(D:\out\a)");
    CHECK(ScanCheckout(BuildCoff({ { .name = ".rdata", .data = R"(D:\out\a\gen\config.h)" } }, 1000), cmake).parts
          == none.With(CheckoutPart::BuildTree).With(CheckoutPart::WorkingDirectory));

    // What cannot be read is bound to everything: nobody can say which part it names.
    std::vector<std::byte> const bitcode { std::byte { 'B' }, std::byte { 'C' }, std::byte { 0xC0 }, std::byte { 0xDE } };
    CHECK(ScanCheckout(bitcode, checkout).parts == CheckoutParts::All());
}

TEST_CASE("An ELF object naming the root in rodata is bound and in DWARF is not", "[launcher][rootbinding]")
{
    auto const roots = RootsOf("/home/ci/checkout-a");
    auto const bound = BuildElf({ { .name = ".rodata.str4.4", .data = Utf32("/home/ci/checkout-a/src/tu.cpp") } });
    CHECK(ScanForRoots(bound, roots).binding == RootBinding::Bound);

    auto const portable = BuildElf({ { .name = ".text", .data = "CODE" },
                                     { .name = ".debug_line_str", .data = "/home/ci/checkout-a/src", .flags = 0 },
                                     { .name = ".rela.debug_info", .data = "/home/ci/checkout-a", .flags = 0 } });
    CHECK(ScanForRoots(portable, roots).binding == RootBinding::Portable);
}

TEST_CASE("A COFF DWARF section spelled as a long name is still a debug record", "[launcher][rootbinding]")
{
    // `clang-cl -gdwarf` and MinGW write `.debug_info`, which is `/4` in the name field;
    // an unresolved name would be scanned as program data and bind every debug build.
    auto const image = BuildCoff(
        { { .name = ".text", .data = "CODE" }, { .name = ".debug_line_str", .data = R"(D:\work\checkout-a\src)" } }, 1000);
    CHECK(ScanForRoots(image, RootsOf(RootA)).binding == RootBinding::Portable);
}

TEST_CASE("A Mach-O object naming the root in cstring is bound and in DWARF is not", "[launcher][rootbinding]")
{
    auto const roots = RootsOf("/Users/ci/checkout-a");
    auto const bound = BuildMachO64({ { .name = "__cstring", .data = "/Users/ci/checkout-a/tu.cpp", .segment = "__TEXT" } });
    CHECK(ScanForRoots(bound, roots).binding == RootBinding::Bound);

    auto const portable = BuildMachO64({ { .name = "__text", .data = "CODE", .segment = "__TEXT" },
                                         { .name = "__debug_str", .data = "/Users/ci/checkout-a", .segment = "__DWARF" } });
    CHECK(ScanForRoots(portable, roots).binding == RootBinding::Portable);
}

TEST_CASE("An object whose strings cannot be read as bytes is bound", "[launcher][rootbinding]")
{
    // Guessing "portable" about bytes nobody could read is the wrong-but-looks-right side.
    auto const roots = RootsOf("/home/ci/checkout-a");
    SECTION("GCC LTO intermediate code")
    {
        auto const image = BuildElf({ { .name = ".gnu.lto_.decls.0", .data = "compressed", .flags = 0 } });
        auto const scan = ScanForRoots(image, roots);
        CHECK(scan.binding == RootBinding::Bound);
        CHECK(scan.evidence.contains(".gnu.lto_"));
    }
    SECTION("a compressed program section")
    {
        auto const image = BuildElf({ { .name = ".rodata", .data = "zstream", .flags = 0x2 | 0x800 } });
        CHECK(ScanForRoots(image, roots).binding == RootBinding::Bound);
    }
    SECTION("clang's coverage map, in every format's spelling")
    {
        // A header, then a zlib stream holding the compilation directory and the source:
        // the root appears nowhere in plain bytes (measured on clang 22, `-fcoverage-mapping`).
        std::string covmap { "\x00\x00\x00\x00\x01\x00\x00\x00\x10\x00\x00\x00\x05\x00\x00\x00\x00\x00\x00", 19 };
        covmap.append("\x78\x9C\x4B\x4C\x4A\x06\x00", 7);
        auto const elf = ScanForRoots(BuildElf({ { .name = "__llvm_covmap", .data = covmap, .flags = 0 } }), roots);
        CHECK(elf.binding == RootBinding::Bound);
        CHECK(elf.evidence.contains("__llvm_covmap"));
        CHECK(ScanForRoots(BuildElf({ { .name = "__llvm_covfun", .data = "fun", .flags = 0 } }), roots).binding
              == RootBinding::Bound);
        CHECK(ScanForRoots(BuildCoff({ { .name = ".lcovmap$M", .data = "map" } }, 1000), roots).binding
              == RootBinding::Bound);
        CHECK(ScanForRoots(BuildCoff({ { .name = ".lcovfun$M", .data = "fun" } }, 1000), roots).binding
              == RootBinding::Bound);
        CHECK(ScanForRoots(BuildMachO64({ { .name = "__llvm_covmap", .data = "map", .segment = "__LLVM_COV" } }), roots)
                  .binding
              == RootBinding::Bound);
    }
    SECTION("any section opening with a compressor's header")
    {
        auto const zlib =
            BuildElf({ { .name = ".rodata.blob", .data = std::string { "\x78\xDA\x01\x02", 4 }, .flags = 0 } });
        CHECK(ScanForRoots(zlib, roots).binding == RootBinding::Bound);
        auto const zstd =
            BuildElf({ { .name = ".rodata.blob", .data = std::string { "\x28\xB5\x2F\xFD\x00", 5 }, .flags = 0 } });
        CHECK(ScanForRoots(zstd, roots).binding == RootBinding::Bound);
        // The control that keeps the rule from binding everything: `48 89` passes zlib's
        // header checksum, and it is how most x86-64 functions begin.
        auto const code = BuildElf({ { .name = ".text", .data = std::string { "\x48\x89\x5C\x24\x08", 5 }, .flags = 0 } });
        CHECK(ScanForRoots(code, roots).binding == RootBinding::Portable);

        // Nor may it fire on a section of fixed-size records, whose first entry is whatever
        // constant the source wrote -- each of these was measured tripping it. The records
        // are still SCANNED: a pool naming a root still binds.
        auto const zlibText = std::string { "\x78\x9C\x4B\x4C\x4A\x06\x00\x00", 8 };
        CHECK(ScanForRoots(BuildElf({ { .name = ".rodata.cst8", .data = zlibText, .flags = 0x12 } }), roots).binding
              == RootBinding::Portable);
        CHECK(ScanForRoots(BuildCoff({ { .name = ".pdata", .data = std::string { "\x78\x01\x00\x00", 4 } } }, 1000), roots)
                  .binding
              == RootBinding::Portable);
        CHECK(ScanForRoots(
                  BuildMachO64(
                      { { .name = "__cstring", .data = std::string { "\x28\xB5\x2F\xFD\x00", 5 }, .segment = "__TEXT" } }),
                  roots)
                  .binding
              == RootBinding::Portable);
        CHECK(ScanForRoots(BuildElf({ { .name = ".rodata.str1.1", .data = "/home/ci/checkout-a/tu.cpp", .flags = 0x32 } }),
                           roots)
                  .binding
              == RootBinding::Bound);
        // A merged pool is named for its entry size, and only that is exempt: under
        // `-fdata-sections` a variable `str_blob` or `cst_table` gets `.rodata.str_blob` or
        // `.rodata.cst_table`, which share the prefix and are not pools. Measured by the
        // part-keying review taking the exemption; each row here goes both ways.
        struct PoolRow
        {
            std::string_view name;
            RootBinding sniffed;
        };
        constexpr auto PoolRows = std::to_array<PoolRow>({
            { .name = ".rodata.str1.1", .sniffed = RootBinding::Portable },
            { .name = ".rodata.str4.16", .sniffed = RootBinding::Portable },
            { .name = ".rodata.cst16", .sniffed = RootBinding::Portable },
            { .name = ".rodata.str_blob", .sniffed = RootBinding::Bound },
            { .name = ".rodata.cst_table", .sniffed = RootBinding::Bound },
            { .name = ".rodata.str", .sniffed = RootBinding::Bound },
            { .name = ".rodata.blob_x", .sniffed = RootBinding::Bound },
        });
        for (auto const& row: PoolRows)
        {
            INFO(row.name);
            auto const image = BuildElf({ { .name = std::string { row.name }, .data = zlibText, .flags = 0x2 } });
            CHECK(ScanForRoots(image, roots).binding == row.sniffed);
        }

        // And the format's own COMPRESSED flag still makes such a pool opaque.
        CHECK(
            ScanForRoots(BuildElf({ { .name = ".rodata.str1.1", .data = "zstream", .flags = 0x2 | 0x800 } }), roots).binding
            == RootBinding::Bound);
    }
    SECTION("LLVM bitcode")
    {
        std::vector<std::byte> const image { std::byte { 'B' }, std::byte { 'C' }, std::byte { 0xC0 }, std::byte { 0xDE } };
        CHECK(ScanForRoots(image, roots).binding == RootBinding::Bound);
    }
    SECTION("an MSVC anonymous object")
    {
        auto const image = BuildCoff(ClSections("CODE"), 1000, StubLayout::BigObj, /*version=*/1);
        CHECK(ScanForRoots(image, roots).binding == RootBinding::Bound);
    }
}

TEST_CASE("A format the walk cannot lay out is scanned whole", "[launcher][rootbinding]")
{
    auto const roots = RootsOf("/home/ci/checkout-a");
    auto const bytesOf = [](std::string_view text) {
        std::vector<std::byte> image;
        std::ranges::transform(text, std::back_inserter(image), [](char c) { return static_cast<std::byte>(c); });
        return image;
    };
    CHECK(ScanForRoots(bytesOf("an object of some format naming /home/ci/checkout-a/tu.cpp"), roots).binding
          == RootBinding::Bound);
    CHECK(ScanForRoots(bytesOf("an object of some format naming nothing"), roots).binding == RootBinding::Portable);
}

TEST_CASE("A root-bound key folds the named parts and nothing else changes it", "[launcher][rootbinding]")
{
    constexpr std::string_view Portable = "0123456789abcdef0123456789abcdef";
    auto const all = CheckoutParts::All();
    auto const a = ComputeRootBoundKey(Portable, CheckoutAt(RootA), all);
    auto const b = ComputeRootBoundKey(Portable, CheckoutAt(RootB), all);
    CHECK(a != b);
    CHECK(a != Portable);
    CHECK(a == ComputeRootBoundKey(Portable, CheckoutAt(RootA), all));
    CHECK(a != ComputeRootBoundKey("fedcba9876543210fedcba9876543210", CheckoutAt(RootA), all));
    // The build tree is folded on its own, not only the source root.
    auto const buildA = LayoutAt(RootA).buildTree;
    CHECK(a != ComputeRootBoundKey(Portable, CheckoutWith(RootA, R"(D:\out)", buildA), all));
    // It is the RESOLVED spelling that is folded: two exports of one place key alike, and
    // one export of two places keys apart.
    auto aliased = CheckoutAt(RootA);
    aliased.exported = LayoutAt(R"(S:\alias)");
    CHECK(a == ComputeRootBoundKey(Portable, aliased, all));

    // Only the NAMED parts: a part the object does not name may differ freely...
    auto const sourceOnly = CheckoutParts {}.With(CheckoutPart::SourceRoot);
    CHECK(ComputeRootBoundKey(Portable, CheckoutWith(RootA, R"(D:\out\b1)", R"(D:\out\b1)"), sourceOnly)
          == ComputeRootBoundKey(Portable, CheckoutWith(RootA, R"(D:\out\b2)", R"(D:\out\b2)"), sourceOnly));
    // ... and a part's NAME is folded with its value, so one directory serving as two parts
    // keys apart per part.
    auto const sameDirectory = CheckoutWith(R"(D:\one)", R"(D:\one)", R"(D:\one)");
    CHECK(ComputeRootBoundKey(Portable, sameDirectory, sourceOnly)
          != ComputeRootBoundKey(Portable, sameDirectory, CheckoutParts {}.With(CheckoutPart::BuildTree)));

    // A pin, for the reason `ComputeKey`'s is one: moving this construction moves every
    // bound key, and nothing else announces it. Reproduced from the MurmurHash3 x64_128
    // reference and `KeyDigest`'s grammar by a separate implementation -- the same one
    // that reproduces `ComputeKey`'s pinned vector and the `rootbound-v1` pins before
    // these -- rather than pasted from a run.
    auto const pinned = CheckoutWith("/src", "/build", "/build");
    CHECK(ComputeRootBoundKey(Portable, pinned, sourceOnly) == "d0b6ebf12452a1f2bbbbcf05ac0625bd");
    CHECK(ComputeRootBoundKey(Portable, pinned, all) == "6db382f9fcb4721f7feb2aa44d864a69");
}

TEST_CASE("The marker carries its parts and is recognised, and nothing else is", "[launcher][rootbinding]")
{
    // Every non-empty set of parts survives the round trip, by name.
    auto const allParts = CheckoutParts::All();
    std::vector<CheckoutParts> subsets { CheckoutParts {} };
    for (auto const part: Enumerators<CheckoutPart>())
    {
        auto const count = subsets.size();
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            subsets.push_back(subsets[index].With(part));
    }
    for (auto const& parts: subsets | std::views::drop(1))
    {
        INFO(parts.Names());
        auto const marker = DecodeCompileValue(EncodeRootBoundMarker(parts));
        REQUIRE(marker.has_value());
        auto const reading = ReadRootBoundMarker(Testing::Unwrap(marker));
        CHECK(reading.kind == MarkerKind::Marker);
        CHECK(reading.parts == parts);
    }

    // The same blob with the two stream regions every stored object carries is an
    // object, whatever its bytes spell.
    auto withRegions = Testing::Unwrap(DecodeCompileValue(EncodeRootBoundMarker(allParts)));
    withRegions.textRegions.push_back({ .grammar = PathCanon::Grammar::ShowIncludes, .bytes = {} });
    CHECK(ReadRootBoundMarker(withRegions).kind == MarkerKind::Object);

    auto const object = DecodeCompileValue(ObjectValue("OBJECT"));
    REQUIRE(object.has_value());
    CHECK(ReadRootBoundMarker(Testing::Unwrap(object)).kind == MarkerKind::Object);
}

TEST_CASE("A marker this build cannot read is refused, never followed or served", "[launcher][rootbinding]")
{
    // `v1` bound every object to the whole checkout and named no parts; a part name or a
    // version this build does not know is the same answer. Refused as undecodable, which
    // compiles and stores over it -- never guessed at, and never served as an object.
    std::string const portable = "0123456789abcdef0123456789abcdef";
    auto const checkout = CheckoutAt(RootA);
    for (auto const blob: { std::string_view { "fastcache-cc root-bound marker v1" },
                            std::string_view { "fastcache-cc root-bound marker v2 source-root,elsewhere" },
                            std::string_view { "fastcache-cc root-bound marker v2 " },
                            std::string_view { "fastcache-cc root-bound marker v9 source-root" } })
    {
        INFO(blob);
        CompileValue value;
        std::ranges::transform(blob, std::back_inserter(value.objectBlob), [](char c) { return static_cast<std::byte>(c); });
        CHECK(ReadRootBoundMarker(value).kind == MarkerKind::UnreadableMarker);
        auto const encoded = EncodeCompileValue(value);
        std::map<std::string, std::vector<std::byte>> const cache { { portable, encoded } };
        auto const fetched = ResolveFetchedObject(encoded, portable, checkout, FetchFrom(cache));
        CHECK(std::holds_alternative<ProtocolError>(fetched.answer));
        CHECK_FALSE(fetched.followedMarker);
    }
}

TEST_CASE("Every server stores the marker as it was sent", "[launcher][rootbinding]")
{
    // The marker carries no text region, so canonicalization has nothing to rewrite and
    // hands back the very bytes -- on the daemon and on a node's tier alike, since both
    // run `CanonicalStoredValue`.
    auto const marker = EncodeRootBoundMarker(CheckoutParts::All());
    auto const stored = CanonicalStoredValue(marker, RootA, std::string { RootA } + R"(\build)");
    CHECK(stored.outcome == CanonicalizationOutcome::Canonicalized);
    CHECK(stored.bytes == marker);
}

TEST_CASE("A portable object is stored under its own key and a bound one behind a marker", "[launcher][rootbinding]")
{
    constexpr std::string_view Portable = "0123456789abcdef0123456789abcdef";
    auto const checkout = CheckoutAt(RootA);

    auto const portable = PlanStore(std::string { Portable }, BuildCoff(ClSections("CODE"), 1), checkout, std::nullopt);
    CHECK(portable.scan.binding == RootBinding::Portable);
    CHECK(portable.objectKey == Portable);
    CHECK_FALSE(portable.markerKey.has_value());

    auto const boundImage = BuildCoff({ { .name = ".rdata", .data = R"(D:\work\checkout-a\src\tu.cpp)" } }, 1);
    auto const bound = PlanStore(std::string { Portable }, boundImage, checkout, std::nullopt);
    CHECK(bound.scan.binding == RootBinding::Bound);
    CHECK(bound.scan.parts == CheckoutParts {}.With(CheckoutPart::SourceRoot));
    CHECK(bound.objectKey == ComputeRootBoundKey(Portable, checkout, bound.scan.parts));
    CHECK(bound.markerKey == std::optional<std::string> { Portable });
}

TEST_CASE("A compile that met a marker stores bound whatever its object says", "[launcher][rootbinding]")
{
    // The marker is evidence that an object under this key names a root; storing a portable
    // copy there would overwrite it for every checkout. It stays bound to the MET parts.
    constexpr std::string_view Portable = "0123456789abcdef0123456789abcdef";
    auto const checkout = CheckoutAt(RootA);
    auto const met = CheckoutParts {}.With(CheckoutPart::BuildTree);
    auto const plan = PlanStore(std::string { Portable }, BuildCoff(ClSections("CODE"), 1), checkout, met);
    CHECK(plan.scan.binding == RootBinding::Bound);
    CHECK(plan.scan.evidence.contains("marker"));
    CHECK(plan.scan.parts == met);
    CHECK(plan.objectKey == ComputeRootBoundKey(Portable, checkout, met));
    CHECK(plan.markerKey == std::optional<std::string> { Portable });
}

TEST_CASE("Every followed marker reaches the store, whatever the bound key held", "[launcher][rootbinding]")
{
    // The review's N1: the store honoured a marker only when the bound key was ABSENT. A
    // bound value that is damaged, of another generation, or a marker itself is compiled
    // over too -- and if this compile's scan then missed, a portable store would overwrite
    // the marker for every checkout. So the fetch hands the marker's parts out with every
    // answer it followed one to, and the plan takes them.
    std::string const portable = "0123456789abcdef0123456789abcdef";
    auto const checkout = CheckoutAt(RootA);
    auto const met = CheckoutParts {}.With(CheckoutPart::SourceRoot);
    auto const boundKey = ComputeRootBoundKey(portable, checkout, met);
    std::vector<std::byte> const junk { std::byte { 0xFF }, std::byte { 0x00 } };

    struct Row
    {
        std::string_view held;
        std::optional<std::vector<std::byte>> bound;
    };
    auto const rows = std::to_array<Row>({
        { .held = "nothing", .bound = std::nullopt },
        { .held = "undecodable bytes", .bound = junk },
        { .held = "a marker", .bound = EncodeRootBoundMarker(met) },
    });
    for (auto const& row: rows)
    {
        INFO("the bound key held " << row.held);
        std::map<std::string, std::vector<std::byte>> cache { { portable, EncodeRootBoundMarker(met) } };
        if (row.bound.has_value())
            cache[boundKey] = Testing::Unwrap(row.bound);
        auto const fetched = ResolveFetchedObject(cache.at(portable), portable, checkout, FetchFrom(cache));
        CHECK_FALSE(std::holds_alternative<CompileValue>(fetched.answer));
        CHECK(fetched.followedMarker);
        REQUIRE(fetched.markerParts.has_value());

        // What the launcher then compiles names nothing, and the plan still keeps it bound.
        auto const plan = PlanStore(portable, BuildCoff(ClSections("CODE"), 1), checkout, fetched.markerParts);
        CHECK(plan.scan.binding == RootBinding::Bound);
        CHECK(plan.scan.parts == met);
        CHECK(plan.objectKey == boundKey);
        CHECK(plan.markerKey == std::optional<std::string> { portable });
    }
}

TEST_CASE("A root-bound miss names the parts, never which of them differs, in its trace and its reason alike",
          "[launcher][rootbinding]")
{
    // The combined re-review's M1, measured: an object whose `__builtin_FILE()` sits in
    // the build tree names all three parts, and a second build directory of the SAME
    // checkout was tallied as "another checkout's source tree". A consumer never sees the
    // producer's values, so it cannot know which part differs; both texts say only which
    // parts the object names, and they are one text.
    auto const sameCheckoutSecondBuildDirectory = CheckoutParts::All();
    auto const reason = BoundMissReason(sameCheckoutSecondBuildDirectory);
    CHECK(reason.contains("source-root,build-tree,working-directory"));
    CHECK_FALSE(reason.contains("another checkout"));
    CHECK_FALSE(reason.contains("another build directory"));

    // Every part set: the reason names exactly its parts, and the trace qualifier IS it.
    std::vector<CheckoutParts> subsets { CheckoutParts {} };
    for (auto const part: Enumerators<CheckoutPart>())
    {
        auto const count = subsets.size();
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            subsets.push_back(subsets[index].With(part));
    }
    for (auto const& parts: subsets | std::views::drop(1))
    {
        INFO(parts.Names());
        auto const text = BoundMissReason(parts);
        CHECK(text.contains(std::format("names its {};", parts.Names())));
        CHECK_FALSE(text.contains("another"));
        CHECK(BoundMissQualifier(parts) == std::format(" (root-bound: {})", text));
    }
}

namespace
{

/// Checkout A stores @p object through the real plan, and checkout B fetches the same
/// portable key: what B is served.
/// @param object What A's compile produced.
/// @param a The storing checkout. @param b The fetching checkout.
[[nodiscard]] FetchedObject StoreThenFetch(std::vector<std::byte> const& object,
                                           CheckoutRoots const& a,
                                           CheckoutRoots const& b)
{
    std::string const portable = "0123456789abcdef0123456789abcdef";
    std::map<std::string, std::vector<std::byte>> cache;
    auto const plan = PlanStore(portable, object, a, std::nullopt);
    cache[plan.objectKey] = ObjectValue("A'S OBJECT");
    if (plan.markerKey.has_value())
        cache[Testing::Unwrap(plan.markerKey)] = EncodeRootBoundMarker(plan.scan.parts);
    return ResolveFetchedObject(cache.at(portable), portable, b, FetchFrom(cache));
}

} // namespace

TEST_CASE("An object naming only the source root replays in any build directory of its checkout", "[launcher][rootbinding]")
{
    // Measured by `launcher-replay-e2e`: a warm build of the SAME source in a DIFFERENT
    // build directory missed on every test object -- Catch2's `__FILE__` names the source
    // root, and the key folded the build tree and the working directory as well, which the
    // object never names. The key folds what the object names, and no more.
    auto const inB1 = CheckoutWith(R"(D:\work\checkout-a)", R"(D:\out\b1)", R"(D:\out\b1)");
    auto const inB2 = CheckoutWith(R"(D:\work\checkout-a)", R"(D:\out\b2)", R"(D:\out\b2)");

    auto const namesSource = BuildCoff({ { .name = ".rdata", .data = R"(D:\work\checkout-a\src\tu_test.cpp)" } }, 1);
    auto const replayed = StoreThenFetch(namesSource, inB1, inB2);
    CHECK(replayed.followedMarker);
    CHECK(std::holds_alternative<CompileValue>(replayed.answer));

    // One naming the build directory spells b1, and must never reach b2.
    auto const namesBuild = BuildCoff({ { .name = ".rdata", .data = R"(D:\out\b1\gen\config.h)" } }, 1);
    CHECK(std::holds_alternative<BoundAbsence>(StoreThenFetch(namesBuild, inB1, inB2).answer));

    // Nor one naming the working directory, where that is not the build tree.
    auto const runC1 = CheckoutWith(R"(D:\work\checkout-a)", R"(D:\out\b)", R"(D:\run\c1)");
    auto const runC2 = CheckoutWith(R"(D:\work\checkout-a)", R"(D:\out\b)", R"(D:\run\c2)");
    auto const namesCwd = BuildCoff({ { .name = ".rdata", .data = R"(D:\run\c1\probe.cpp)" } }, 1);
    CHECK(std::holds_alternative<BoundAbsence>(StoreThenFetch(namesCwd, runC1, runC2).answer));
    CHECK(std::holds_alternative<CompileValue>(StoreThenFetch(namesSource, runC1, runC2).answer));

    // And another checkout still misses, whatever the object names.
    auto const otherCheckout = CheckoutWith(R"(D:\work\checkout-b)", R"(D:\out\b1)", R"(D:\out\b1)");
    CHECK(std::holds_alternative<BoundAbsence>(StoreThenFetch(namesSource, inB1, otherCheckout).answer));
}

TEST_CASE("A marker sends another checkout to a key it never stored", "[launcher][rootbinding]")
{
    // The whole mechanism, over a map standing in for the cache: checkout A stores a
    // root-bound object, B fetches the same portable key, and B must MISS where it used
    // to be served A's object.
    std::string const portable = "0123456789abcdef0123456789abcdef";
    auto const layoutA = CheckoutAt(RootA);
    auto const layoutB = CheckoutAt(RootB);
    std::map<std::string, std::vector<std::byte>> cache;

    auto const objectA = BuildCoff({ { .name = ".rdata", .data = R"(D:\work\checkout-a\src\tu.cpp)" } }, 1);
    auto const plan = PlanStore(portable, objectA, layoutA, std::nullopt);
    REQUIRE(plan.markerKey.has_value());
    cache[plan.objectKey] = ObjectValue("A'S OBJECT");
    cache[Testing::Unwrap(plan.markerKey)] = EncodeRootBoundMarker(plan.scan.parts);

    auto const forB = ResolveFetchedObject(cache.at(portable), portable, layoutB, FetchFrom(cache));
    CHECK(std::holds_alternative<BoundAbsence>(forB.answer));
    CHECK(forB.followedMarker);
    CHECK(forB.markerParts == std::optional<CheckoutParts> { plan.scan.parts });
    CHECK(forB.servedKey == ComputeRootBoundKey(portable, layoutB, plan.scan.parts));

    // A meets its own marker and is served its own object.
    auto const forA = ResolveFetchedObject(cache.at(portable), portable, layoutA, FetchFrom(cache));
    auto const* served = std::get_if<CompileValue>(&forA.answer);
    REQUIRE(served != nullptr);
    CHECK(forA.servedKey == plan.objectKey);
    CHECK(BlobText(*served) == "A'S OBJECT");
}

TEST_CASE("A portable object is served as it always was", "[launcher][rootbinding]")
{
    std::string const portable = "0123456789abcdef0123456789abcdef";
    std::map<std::string, std::vector<std::byte>> cache { { portable, ObjectValue("SHARED") } };
    auto const fetched = ResolveFetchedObject(cache.at(portable), portable, CheckoutAt(RootB), FetchFrom(cache));
    auto const* served = std::get_if<CompileValue>(&fetched.answer);
    REQUIRE(served != nullptr);
    CHECK_FALSE(fetched.followedMarker);
    CHECK_FALSE(fetched.markerParts.has_value());
    CHECK(fetched.servedKey == portable);
    CHECK(BlobText(*served) == "SHARED");
}

TEST_CASE("A marker under a bound key is refused rather than chased", "[launcher][rootbinding]")
{
    std::string const portable = "0123456789abcdef0123456789abcdef";
    auto const layout = CheckoutAt(RootA);
    auto const all = CheckoutParts::All();
    std::map<std::string, std::vector<std::byte>> cache {
        { portable, EncodeRootBoundMarker(all) }, { ComputeRootBoundKey(portable, layout, all), EncodeRootBoundMarker(all) }
    };
    auto const fetched = ResolveFetchedObject(cache.at(portable), portable, layout, FetchFrom(cache));
    CHECK(std::holds_alternative<ProtocolError>(fetched.answer));
}

TEST_CASE("An undecodable value under either key is reported as undecodable", "[launcher][rootbinding]")
{
    std::string const portable = "0123456789abcdef0123456789abcdef";
    auto const layout = CheckoutAt(RootA);
    auto const all = CheckoutParts::All();
    std::vector<std::byte> const junk { std::byte { 0xFF }, std::byte { 0x00 } };

    std::map<std::string, std::vector<std::byte>> const atPortable { { portable, junk } };
    CHECK(std::holds_alternative<ProtocolError>(ResolveFetchedObject(junk, portable, layout, FetchFrom(atPortable)).answer));

    std::map<std::string, std::vector<std::byte>> const atBound { { portable, EncodeRootBoundMarker(all) },
                                                                  { ComputeRootBoundKey(portable, layout, all), junk } };
    auto const fetched = ResolveFetchedObject(atBound.at(portable), portable, layout, FetchFrom(atBound));
    CHECK(std::holds_alternative<ProtocolError>(fetched.answer));
    CHECK(fetched.followedMarker);
    CHECK(fetched.markerParts == std::optional<CheckoutParts> { all });
}

// --- real compilers ----------------------------------------------------------

namespace
{

/// A translation unit that bakes its own path three ways, with no `#include` -- so a
/// bare driver with no `INCLUDE` compiles it. `__builtin_FILE()` is what
/// `std::source_location::current()` is built on, on every compiler this caches.
constexpr std::string_view PathBakingSource = "char const* File() { return __FILE__; }\n"
                                              "char const* Located() { return __builtin_FILE(); }\n"
                                              "wchar_t const* Wide() { return L\"\" __FILE__; }\n";

/// The control: the same shape, naming no path.
constexpr std::string_view PathFreeSource = "char const* File() { return \"no path here\"; }\n"
                                            "int Scale(int x) { return (x * 3) + 1; }\n";

/// A compiler family, and how to ask it for an object.
struct CompilerFamily
{
    std::vector<std::string> candidates;
    std::vector<std::string> (*argv)(std::string const& driver,
                                     std::filesystem::path const& source,
                                     std::filesystem::path const& object,
                                     bool debugInfo);
};

[[nodiscard]] std::vector<std::string> MsvcArgv(std::string const& driver,
                                                std::filesystem::path const& source,
                                                std::filesystem::path const& object,
                                                bool debugInfo)
{
    std::vector<std::string> argv { driver, "/nologo", "/c", "/Fo" + object.string(), source.string() };
    if (debugInfo)
        argv.emplace_back("/Z7");
    return argv;
}

[[nodiscard]] std::vector<std::string> GnuArgv(std::string const& driver,
                                               std::filesystem::path const& source,
                                               std::filesystem::path const& object,
                                               bool debugInfo)
{
    std::vector<std::string> argv { driver, "-c", "-o", object.string(), source.string() };
    if (debugInfo)
        argv.emplace_back("-g");
    return argv;
}

/// A GNU-spelled clang compile with source-based coverage, which is what writes the map.
[[nodiscard]] std::vector<std::string> CoverageGnuArgv(std::string const& driver,
                                                       std::filesystem::path const& source,
                                                       std::filesystem::path const& object,
                                                       bool debugInfo)
{
    auto argv = GnuArgv(driver, source, object, debugInfo);
    argv.insert(argv.begin() + 1, { "-fprofile-instr-generate", "-fcoverage-mapping" });
    return argv;
}

/// The same for clang-cl, which takes the GNU spellings of these two.
[[nodiscard]] std::vector<std::string> CoverageMsvcArgv(std::string const& driver,
                                                        std::filesystem::path const& source,
                                                        std::filesystem::path const& object,
                                                        bool debugInfo)
{
    auto argv = MsvcArgv(driver, source, object, debugInfo);
    argv.insert(argv.begin() + 1, { "-fprofile-instr-generate", "-fcoverage-mapping" });
    return argv;
}

/// The first driver of @p family on this host that can actually COMPILE -- presence is
/// not usability; see `ObjectEquivalence_test.cpp`.
[[nodiscard]] std::optional<std::string> FindWorkingDriver(IProcessRunner& runner,
                                                           CompilerFamily const& family,
                                                           std::filesystem::path const& dir)
{
    auto const probe = dir / "probe.cpp";
    std::ofstream { probe } << PathFreeSource;
    for (auto const& candidate: family.candidates)
    {
        auto const object = dir / "probe.o";
        std::error_code ec;
        std::filesystem::remove(object, ec);
        if (runner.RunCaptureCombined(family.argv(candidate, probe, object, false)).exitCode == 0
            && std::filesystem::exists(object))
            return candidate;
    }
    return std::nullopt;
}

/// Compile @p text as `<root>/src/tu.cpp` and scan the object against @p root.
[[nodiscard]] RootBindingScan CompileAndScan(IProcessRunner& runner,
                                             CompilerFamily const& family,
                                             std::string const& driver,
                                             std::filesystem::path const& root,
                                             std::string_view text,
                                             bool debugInfo)
{
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "build");
    auto const source = root / "src" / "tu.cpp";
    std::ofstream { source } << text;
    auto const object = root / "build" / "tu.o";
    REQUIRE(runner.RunCaptureCombined(family.argv(driver, source, object, debugInfo)).exitCode == 0);
    auto const bytes = ReadFileBytes(object).value_or(std::vector<std::byte> {});
    REQUIRE_FALSE(bytes.empty());
    return ScanForRoots(bytes,
                        RootSpellings(CheckoutWith(root.string(), (root / "build").string(), (root / "build").string())));
}

/// The real-compiler cases, for one family.
void CheckRealCompiler(CompilerFamily const& family, std::string_view tag)
{
    Testing::ScratchDirectory const scratch { tag };
    auto const runner = MakeProcessRunner();
    auto const found = FindWorkingDriver(*runner, family, scratch.Path());
    if (!found.has_value())
        SKIP("no driver of this family on this host can compile");
    REQUIRE(found.has_value());
    auto const& driver = Testing::Unwrap(found);
    INFO("driver: " << driver);

    // The object bakes the path into program data three ways: `__FILE__`, the builtin
    // `source_location` is made of, and a wide literal.
    auto const baking = CompileAndScan(*runner, family, driver, scratch.Path() / "checkout-a", PathBakingSource, false);
    INFO("evidence: " << baking.evidence);
    CHECK(baking.binding == RootBinding::Bound);

    // The control, and the half that keeps the fix from passing by binding everything:
    // an object naming no path stays portable -- WITH debug info on, whose records name
    // the checkout on every compile.
    auto const control = CompileAndScan(*runner, family, driver, scratch.Path() / "checkout-b", PathFreeSource, true);
    INFO("control evidence: " << control.evidence);
    CHECK(control.binding == RootBinding::Portable);
}

} // namespace

TEST_CASE("A real MSVC object baking its path is bound and one baking none is not", "[launcher][rootbinding][msvc]")
{
    CheckRealCompiler(CompilerFamily { .candidates = { "cl.exe", "clang-cl.exe" }, .argv = &MsvcArgv }, "root-binding-msvc");
}

TEST_CASE("A real GNU-driver object baking its path is bound and one baking none is not", "[launcher][rootbinding][gnu]")
{
    CheckRealCompiler(CompilerFamily { .candidates = { "c++", "g++", "clang++" }, .argv = &GnuArgv }, "root-binding-gnu");
}

namespace
{

/// A clang coverage object is bound BY ITS COVERAGE MAP, and the same compile without
/// coverage is not -- so the verdict is the map's, not the TU's.
///
/// The evidence must name the map as unreadable, never a plain-byte match. On ELF clang
/// compresses the map and no plain byte names the checkout, so the rule is the only thing
/// that binds it; on COFF VS's clang-cl leaves the map uncompressed, the root IS in plain
/// bytes, and an assertion on the verdict alone passed with the rule deleted (measured).
void CheckCoverageObject(CompilerFamily const& coverage, CompilerFamily const& plain, std::string_view tag)
{
    Testing::ScratchDirectory const scratch { tag };
    auto const runner = MakeProcessRunner();
    auto const found = FindWorkingDriver(*runner, coverage, scratch.Path());
    if (!found.has_value())
        SKIP("no clang driver of this family on this host can compile with coverage");
    REQUIRE(found.has_value());
    auto const& driver = Testing::Unwrap(found);
    INFO("driver: " << driver);

    auto const covered = CompileAndScan(*runner, coverage, driver, scratch.Path() / "checkout-a", PathFreeSource, false);
    INFO("evidence: " << covered.evidence);
    CHECK(covered.binding == RootBinding::Bound);
    CHECK(covered.evidence.contains("cov"));
    CHECK(covered.evidence.contains("cannot read"));

    auto const control = CompileAndScan(*runner, plain, driver, scratch.Path() / "checkout-b", PathFreeSource, false);
    INFO("control evidence: " << control.evidence);
    CHECK(control.binding == RootBinding::Portable);
}

} // namespace

TEST_CASE("A real clang coverage object is bound by its coverage map", "[launcher][rootbinding][gnu]")
{
    CheckCoverageObject(CompilerFamily { .candidates = { "clang++-22", "clang++" }, .argv = &CoverageGnuArgv },
                        CompilerFamily { .candidates = {}, .argv = &GnuArgv },
                        "root-binding-coverage-gnu");
}

TEST_CASE("A real clang-cl coverage object is bound by its coverage map", "[launcher][rootbinding][msvc]")
{
    CheckCoverageObject(CompilerFamily { .candidates = { "clang-cl.exe" }, .argv = &CoverageMsvcArgv },
                        CompilerFamily { .candidates = {}, .argv = &MsvcArgv },
                        "root-binding-coverage-msvc");
}

TEST_CASE("A worker-shaped compile of a client's preprocessed text is bound to the client", "[launcher][rootbinding][msvc]")
{
    // What a dispatched compile looks like from the object's side: the CLIENT preprocesses
    // with line markers, a worker compiles that text as a scratch file somewhere else
    // entirely, and the object comes back to be stored by the client. The line markers
    // name the client's paths, so `__FILE__` (expanded in the text) and the builtin behind
    // `source_location` (resolved by the worker's compiler from the markers) both name the
    // CLIENT's checkout -- which is the root the client scans for.
    Testing::ScratchDirectory const scratch { "root-binding-worker" };
    auto const runner = MakeProcessRunner();
    CompilerFamily const family { .candidates = { "cl.exe", "clang-cl.exe" }, .argv = &MsvcArgv };
    auto const found = FindWorkingDriver(*runner, family, scratch.Path());
    if (!found.has_value())
        SKIP("no MSVC-family driver on this host can compile");
    REQUIRE(found.has_value());
    auto const& driver = Testing::Unwrap(found);

    auto const client = scratch.Path() / "client";
    std::filesystem::create_directories(client / "src");
    auto const source = client / "src" / "tu.cpp";
    std::ofstream { source } << "char const* Located() { return __builtin_FILE(); }\n";

    auto const preprocessed = runner->RunCaptureSplit(std::vector<std::string> { driver, "/nologo", "/E", source.string() });
    REQUIRE(preprocessed.exitCode == 0);

    auto const worker = scratch.Path() / "worker" / "job-1";
    std::filesystem::create_directories(worker);
    auto const text = worker / "tu.cpp";
    std::ofstream { text, std::ios::binary } << preprocessed.out;
    auto const object = worker / "tu.obj";
    REQUIRE(runner
                ->RunCaptureCombined(
                    std::vector<std::string> { driver, "/nologo", "/c", "/Fo" + object.string(), text.string() })
                .exitCode
            == 0);
    auto const bytes = ReadFileBytes(object).value_or(std::vector<std::byte> {});
    REQUIRE_FALSE(bytes.empty());

    auto const clientBuild = (client / "build").string();
    auto const scan = ScanForRoots(bytes, RootSpellings(CheckoutWith(client.string(), clientBuild, clientBuild)));
    INFO("evidence: " << scan.evidence);
    CHECK(scan.binding == RootBinding::Bound);
}
