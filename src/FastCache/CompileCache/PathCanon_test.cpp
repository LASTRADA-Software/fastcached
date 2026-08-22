// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/PathCanon.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace FastCache;
using PathCanon::Grammar;
using PathCanon::Layout;

// ---------------------------------------------------------------------------
// Task 1: single-path canonicalize/localize round-trip (canon spec v1 vectors).
// ---------------------------------------------------------------------------

TEST_CASE("Canonicalize rewrites a source-root path to a SRCROOT token")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\runner\src)", .buildTree = R"(C:\ci\deep\runner\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\ci\deep\runner\src\include\foo.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == "<SRCROOT>/include/foo.h");
}

TEST_CASE("Canonicalize prefers the longest (build-tree) root when nested under source root")
{
    Layout const layout { .sourceRoot = R"(C:\src)", .buildTree = R"(C:\src\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\src\build\gen\config.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == "<BUILDTREE>/gen/config.h");
}

TEST_CASE("Localize is the inverse of Canonicalize under a different layout")
{
    Layout const producer { .sourceRoot = R"(C:\ci\deep\runner\src)", .buildTree = R"(C:\ci\deep\runner\build)" };
    Layout const consumer { .sourceRoot = R"(D:\project)", .buildTree = R"(D:\project\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\ci\deep\runner\src\include\foo.h)", producer);
    REQUIRE(token.has_value());
    auto const local = PathCanon::Localize(*token, consumer);
    REQUIRE(local.has_value());
    CHECK(*local == R"(D:\project\include\foo.h)");
}

TEST_CASE("A path under neither root round-trips verbatim")
{
    Layout const layout { .sourceRoot = R"(C:\src)", .buildTree = R"(C:\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\Windows\Kits\10\um\windows.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == R"(C:\Windows\Kits\10\um\windows.h)");
    auto const local = PathCanon::Localize(*token, layout);
    REQUIRE(local.has_value());
    CHECK(*local == R"(C:\Windows\Kits\10\um\windows.h)");
}

TEST_CASE("Root match is on a segment boundary, not a bare prefix")
{
    // sourceRoot C:\srclib must NOT match a path under C:\src\...
    Layout const layout { .sourceRoot = R"(C:\srclib)", .buildTree = R"(C:\build)" };
    auto const token = PathCanon::Canonicalize(R"(C:\src\other\x.h)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == R"(C:\src\other\x.h)");
}

TEST_CASE("Matching is case-insensitive on Windows drive and path")
{
    Layout const layout { .sourceRoot = R"(C:\Ci\Deep\Src)", .buildTree = R"(C:\Ci\Deep\Build)" };
    auto const token = PathCanon::Canonicalize(R"(c:\ci\deep\src\A.H)", layout);
    REQUIRE(token.has_value());
    // Tail preserves the ORIGINAL bytes' case; only separators normalize.
    CHECK(*token == "<SRCROOT>/A.H");
}

// ---------------------------------------------------------------------------
// Task 2: region grammar (showIncludes / diagnostics / depfile).
// ---------------------------------------------------------------------------

TEST_CASE("CanonicalizeRegion rewrites only the path in a /showIncludes line")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    std::string const in = "Note: including file: "
                           R"(C:\ci\deep\src\include\foo.h)"
                           "\r\n";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, layout);
    REQUIRE(out.has_value());
    CHECK(*out == "Note: including file: <SRCROOT>/include/foo.h\r\n");
}

TEST_CASE("LocalizeRegion round-trips a showIncludes block across layouts")
{
    Layout const producer { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    Layout const consumer { .sourceRoot = R"(D:\project)", .buildTree = R"(D:\project\build)" };
    std::string const in = "Note: including file:  "
                           R"(C:\ci\deep\src\a.h)"
                           "\r\n"
                           "Note: including file: "
                           R"(C:\ci\deep\src\b.h)"
                           "\r\n";
    auto const canon = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, producer);
    REQUIRE(canon.has_value());
    auto const local = PathCanon::LocalizeRegion(*canon, Grammar::ShowIncludes, consumer);
    REQUIRE(local.has_value());
    CHECK(local->contains(R"(D:\project\a.h)"));
    CHECK(local->contains(R"(D:\project\b.h)"));
    CHECK_FALSE(local->contains(R"(ci\deep)"));
}

TEST_CASE("A non-path showIncludes line is preserved verbatim")
{
    Layout const layout { .sourceRoot = R"(C:\src)", .buildTree = R"(C:\build)" };
    std::string const in = "some unrelated compiler note\r\n";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, layout);
    REQUIRE(out.has_value());
    CHECK(*out == in);
}

TEST_CASE("CanonicalizeRegion leaves a final line without newline intact")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    // no trailing CRLF
    std::string const in = "Note: including file: "
                           R"(C:\ci\deep\src\z.h)";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::ShowIncludes, layout);
    REQUIRE(out.has_value());
    CHECK(*out == "Note: including file: <SRCROOT>/z.h");
}

TEST_CASE("MsvcDiagnostics rewrites the leading path of a diagnostic line")
{
    Layout const layout { .sourceRoot = R"(C:\ci\deep\src)", .buildTree = R"(C:\ci\deep\build)" };
    std::string const in = R"(C:\ci\deep\src\a.cpp(42): warning C4100: unreferenced)"
                           "\r\n";
    auto const out = PathCanon::CanonicalizeRegion(in, Grammar::MsvcDiagnostics, layout);
    REQUIRE(out.has_value());
    CHECK(*out == "<SRCROOT>/a.cpp(42): warning C4100: unreferenced\r\n");
}

// ---------------------------------------------------------------------------
// GccDepfile: the multi-token grammar. A depfile line names a target AND a whole
// dependency list, so every token on it is a path that must travel.
// ---------------------------------------------------------------------------

TEST_CASE("GccDepfile canonicalizes both the target and every dependency")
{
    Layout const layout { .sourceRoot = "/home/ci/src", .buildTree = "/home/ci/build" };
    constexpr std::string_view depFile = "/home/ci/build/a.o: /home/ci/src/a.cpp /home/ci/src/a.hpp\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    // Every checkout-rooted path becomes a token; nothing machine-specific left.
    CHECK_FALSE(canonical->contains("/home/ci"));
    CHECK(canonical->contains("<BUILDTREE>"));
    CHECK(canonical->contains("<SRCROOT>"));
    // The rule syntax itself is untouched.
    CHECK(canonical->contains(": "));
    CHECK(canonical->ends_with("\n"));
}

TEST_CASE("GccDepfile round-trips a depfile into a different checkout")
{
    // The property the cache depends on: a depfile stored from one checkout must
    // come back naming the CONSUMER's paths. Replaying it verbatim would point
    // the build system at another tree — or at nothing at all.
    Layout const producer { .sourceRoot = "/build/deep/a/b/c/src", .buildTree = "/build/deep/a/b/c/out" };
    Layout const consumer { .sourceRoot = "/s", .buildTree = "/s/out" };

    constexpr std::string_view depFile = "/build/deep/a/b/c/out/t.o: /build/deep/a/b/c/src/t.cpp \\\n"
                                         "  /usr/include/stdc-predef.h \\\n"
                                         "  /build/deep/a/b/c/src/hdr.hpp\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, producer);
    REQUIRE(canonical.has_value());
    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, consumer);
    REQUIRE(localized.has_value());

    CHECK(localized->contains("/s/out/t.o"));
    CHECK(localized->contains("/s/t.cpp"));
    CHECK(localized->contains("/s/hdr.hpp"));
    // The producer's layout must be entirely gone.
    CHECK_FALSE(localized->contains("/build/deep"));
    // A path under neither root (a system header) is not ours to rewrite.
    CHECK(localized->contains("/usr/include/stdc-predef.h"));
    // Continuations survive: a depfile whose line structure changed could stop
    // parsing as a rule at all.
    CHECK(localized->contains("\\\n"));
}

TEST_CASE("GccDepfile preserves escaped spaces inside a path")
{
    // A checkout under a directory with a space in its name is ordinary; make
    // escapes the space, and losing the escape splits one path into two.
    Layout const layout { .sourceRoot = "/home/My Code", .buildTree = "/home/My Build" };
    constexpr std::string_view depFile = R"(/home/My\ Build/a.o: /home/My\ Code/a.cpp)"
                                         "\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, layout);
    REQUIRE(localized.has_value());
    // Byte-identical round-trip through the same layout.
    CHECK(*localized == depFile);
}

TEST_CASE("GccDepfile keeps a Windows drive letter out of the rule separator")
{
    // "D:" must not read as the target/dependency separator, or every absolute
    // Windows path in the file would be cut down to its drive letter.
    Layout const layout { .sourceRoot = R"(D:\src)", .buildTree = R"(D:\build)" };
    constexpr std::string_view depFile = R"(D:\build\a.obj: D:\src\a.cpp D:\src\a.hpp)"
                                         "\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    CHECK(canonical->contains("<BUILDTREE>"));
    CHECK(canonical->contains("<SRCROOT>"));

    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, layout);
    REQUIRE(localized.has_value());
    CHECK(*localized == depFile);
}

TEST_CASE("GccDepfile leaves a depfile with no in-tree paths untouched")
{
    Layout const layout { .sourceRoot = "/home/ci/src", .buildTree = "/home/ci/build" };
    constexpr std::string_view depFile = "/other/a.o: /other/a.cpp /usr/include/stdio.h\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    CHECK(*canonical == depFile);
}

TEST_CASE("GccDepfile preserves the phony rules -MP emits")
{
    Layout const layout { .sourceRoot = "/s", .buildTree = "/b" };
    constexpr std::string_view depFile = "/b/a.o: /s/a.cpp /s/a.hpp\n"
                                         "\n"
                                         "/s/a.hpp:\n";

    auto const canonical = PathCanon::CanonicalizeRegion(depFile, Grammar::GccDepfile, layout);
    REQUIRE(canonical.has_value());
    auto const localized = PathCanon::LocalizeRegion(*canonical, Grammar::GccDepfile, layout);
    REQUIRE(localized.has_value());
    CHECK(*localized == depFile);
}

// ---------------------------------------------------------------------------
// Task 3: AnchorForLayout — what a compiler-emitted path is anchored to.
// ---------------------------------------------------------------------------

namespace
{
Layout WindowsLayout()
{
    return { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
}

Layout PosixLayout()
{
    return { .sourceRoot = "/home/dev/proj", .buildTree = "/home/dev/proj/build" };
}
} // namespace

TEST_CASE("A rooted drive path is absolute, in either separator style")
{
    CHECK(PathCanon::AnchorForLayout(R"(C:\src\proj\a.hpp)", WindowsLayout()) == PathCanon::Anchor::Absolute);
    CHECK(PathCanon::AnchorForLayout("C:/src/proj/a.hpp", WindowsLayout()) == PathCanon::Anchor::Absolute);
    // Case and drive are irrelevant to the shape question.
    CHECK(PathCanon::AnchorForLayout(R"(d:\other\x.h)", WindowsLayout()) == PathCanon::Anchor::Absolute);
}

TEST_CASE("A drive-relative path is neither absolute nor working-directory-relative")
{
    // The defect issue #65 records: this used to report absolute, because the
    // drive test stopped at the colon. `C:foo` resolves against drive C's own
    // current directory, so it is portable to nothing and checkable by nobody.
    CHECK(PathCanon::AnchorForLayout(R"(C:foo\bar.hpp)", WindowsLayout()) == PathCanon::Anchor::DriveRelative);
    CHECK(PathCanon::AnchorForLayout("C:foo/bar.hpp", WindowsLayout()) == PathCanon::Anchor::DriveRelative);
    // A bare specifier has no tail at all, and *is* "the current directory of
    // drive C" — the same state, spelled with nothing after it.
    CHECK(PathCanon::AnchorForLayout("C:", WindowsLayout()) == PathCanon::Anchor::DriveRelative);
}

TEST_CASE("A root-relative path and a UNC share are both absolute on Windows")
{
    // Neither may be resolved against a working directory: both name a fixed
    // location, and a UNC share opens with the same byte as a root-relative path.
    CHECK(PathCanon::AnchorForLayout(R"(\Windows\x.h)", WindowsLayout()) == PathCanon::Anchor::Absolute);
    CHECK(PathCanon::AnchorForLayout(R"(\\host\share\x.h)", WindowsLayout()) == PathCanon::Anchor::Absolute);
}

TEST_CASE("A Windows path with no anchor at all resolves against the working directory")
{
    CHECK(PathCanon::AnchorForLayout(R"(inc\a.hpp)", WindowsLayout()) == PathCanon::Anchor::WorkingDirectory);
    CHECK(PathCanon::AnchorForLayout("inc/a.hpp", WindowsLayout()) == PathCanon::Anchor::WorkingDirectory);
}

TEST_CASE("A POSIX layout asks only about the leading separator")
{
    CHECK(PathCanon::AnchorForLayout("/usr/include/x.h", PosixLayout()) == PathCanon::Anchor::Absolute);
    CHECK(PathCanon::AnchorForLayout("inc/a.hpp", PosixLayout()) == PathCanon::Anchor::WorkingDirectory);
    // A POSIX file may legitimately be named `C:foo`. There are no drives here, so
    // the drive rule must not fire — the same mistake IsWindowsRoot's own letter
    // and separator tests exist to prevent for a root like `a:b/proj`.
    CHECK(PathCanon::AnchorForLayout("C:foo", PosixLayout()) == PathCanon::Anchor::WorkingDirectory);
    CHECK(PathCanon::AnchorForLayout(R"(C:\foo)", PosixLayout()) == PathCanon::Anchor::WorkingDirectory);
}

TEST_CASE("An empty path is classified without reading past its end")
{
    CHECK(PathCanon::AnchorForLayout("", WindowsLayout()) == PathCanon::Anchor::WorkingDirectory);
    CHECK(PathCanon::AnchorForLayout("", PosixLayout()) == PathCanon::Anchor::WorkingDirectory);
}

TEST_CASE("A bare drive specifier is a Windows root, though it is a drive-relative path")
{
    // AnchorForLayout and IsWindowsRoot share their drive rule but ask different
    // questions of it, and `C:` is where they part: as a ROOT it is the degenerate
    // spelling of the drive root, while as a PATH it names the drive's current
    // directory. Both halves are asserted against the SAME layout, because it is
    // their disagreement that is the property: merge the two and one of these two
    // lines must break, whichever direction the merge went.
    Layout const layout { .sourceRoot = "C:", .buildTree = "C:" };
    // Root half: were `C:` rejected as a root, this layout would read as POSIX and
    // the backslash path would come back WorkingDirectory.
    CHECK(PathCanon::AnchorForLayout(R"(C:\a.hpp)", layout) == PathCanon::Anchor::Absolute);
    // Path half: the same two bytes, asked about as a path.
    CHECK(PathCanon::AnchorForLayout("C:", layout) == PathCanon::Anchor::DriveRelative);
}

TEST_CASE("A drive-relative root still makes a Windows layout, and still canonicalizes")
{
    // `C:src\proj` is drive-relative AND backslash-separated, so IsWindowsRoot
    // accepts it on its separator branch. Paths under such a root are themselves
    // drive-relative — which is what the callers' DriveRelative arms turn on, so
    // it is pinned here rather than left to be inferred.
    Layout const layout { .sourceRoot = R"(C:src\proj)", .buildTree = R"(C:src\build)" };
    CHECK(PathCanon::AnchorForLayout(R"(C:src\proj\a.hpp)", layout) == PathCanon::Anchor::DriveRelative);

    // And it canonicalizes anyway: a token is portable because the CONSUMER
    // substitutes its own root, whatever shape the producer's root had. This is
    // why the key filter leaves such a path to the root tests instead of dropping
    // it on the anchor alone.
    auto const token = PathCanon::Canonicalize(R"(C:src\proj\a.hpp)", layout);
    REQUIRE(token.has_value());
    CHECK(*token == "<SRCROOT>/a.hpp");
}

// ---------------------------------------------------------------------------
// RewritePaths: the same grammars, driven by a caller's own transform. The
// launcher needs this to reconcile a driver's spelling of a path (an 8.3 short
// component, a `subst` drive) with the roots a STORE is canonicalized against —
// issue #66. Asking the filesystem is the caller's job; finding the spans is
// this file's.
// ---------------------------------------------------------------------------

namespace
{
/// A stand-in for the launcher's path resolver: maps one aliased spelling of a
/// directory onto its real one, and leaves everything else alone.
[[nodiscard]] std::string DealiasShortName(std::string_view path)
{
    constexpr std::string_view Aliased = R"(C:\Users\RUNNER~1\src)";
    constexpr std::string_view Real = R"(C:\Users\runneradmin\src)";
    if (!path.starts_with(Aliased))
        return std::string { path };
    return std::string { Real } + std::string { path.substr(Aliased.size()) };
}
} // namespace

TEST_CASE("RewritePaths reconciles a showIncludes path so the roots can then match it")
{
    // The measured Windows shape: the root is spelled long and `clang-cl` echoes
    // the short spelling it was handed, so canonicalization finds nothing under
    // either root and the value keeps this machine's absolute paths.
    Layout const layout { .sourceRoot = R"(C:\Users\runneradmin\src)", .buildTree = R"(C:\Users\runneradmin\build)" };
    constexpr std::string_view region = "Note: including file:  C:\\Users\\RUNNER~1\\src\\inc\\h1.h\r\n";

    auto const unreconciled = PathCanon::CanonicalizeRegion(region, Grammar::ShowIncludes, layout);
    REQUIRE(unreconciled.has_value());
    CHECK(*unreconciled == region); // nothing matched: the defect this exists to end

    auto const reconciled = PathCanon::RewritePaths(region, Grammar::ShowIncludes, DealiasShortName);
    REQUIRE(reconciled.has_value());
    auto const canonical = PathCanon::CanonicalizeRegion(*reconciled, Grammar::ShowIncludes, layout);
    REQUIRE(canonical.has_value());
    CHECK(*canonical == "Note: including file:  <SRCROOT>/inc/h1.h\r\n");
}

TEST_CASE("RewritePaths leaves the non-path parts of every grammar byte-for-byte")
{
    // The transform sees only what the grammar identifies as a path, so a line that
    // merely quotes the marker and a diagnostic's location suffix both survive.
    constexpr std::string_view notes = "char const* s = \"Note: including file: x\";\n"
                                       "Note: including file: C:\\Users\\RUNNER~1\\src\\a.h\n";
    auto const rewritten = PathCanon::RewritePaths(notes, Grammar::ShowIncludes, DealiasShortName);
    REQUIRE(rewritten.has_value());
    CHECK(*rewritten
          == "char const* s = \"Note: including file: x\";\n"
             "Note: including file: C:\\Users\\runneradmin\\src\\a.h\n");

    constexpr std::string_view diagnostic = "C:\\Users\\RUNNER~1\\src\\a.cpp(12,3): warning C4100: unused\n";
    auto const diag = PathCanon::RewritePaths(diagnostic, Grammar::MsvcDiagnostics, DealiasShortName);
    REQUIRE(diag.has_value());
    CHECK(*diag == "C:\\Users\\runneradmin\\src\\a.cpp(12,3): warning C4100: unused\n");
}

TEST_CASE("RewritePaths reaches every token of a depfile, target included")
{
    // The depfile grammar carries many spans per line, so it takes the other
    // walker; both must be reachable through this entry point or a GNU build's
    // stored depfile keeps the producing machine's spelling.
    constexpr std::string_view depFile = "C:\\Users\\RUNNER~1\\src\\a.o: C:\\Users\\RUNNER~1\\src\\a.cpp\\\n"
                                         "  C:\\Users\\RUNNER~1\\src\\inc\\h1.h\n";
    auto const rewritten = PathCanon::RewritePaths(depFile, Grammar::GccDepfile, DealiasShortName);
    REQUIRE(rewritten.has_value());
    CHECK(*rewritten
          == "C:\\Users\\runneradmin\\src\\a.o: C:\\Users\\runneradmin\\src\\a.cpp\\\n"
             "  C:\\Users\\runneradmin\\src\\inc\\h1.h\n");
}

TEST_CASE("RewritePaths lets a transform preserve one span and rewrite its neighbours")
{
    // The per-span decision is the mechanism the launcher builds its depfile rule
    // on: it preserves the object path — the `-o` value, which the BUILD SYSTEM
    // named — while reconciling every dependency the compiler reported around it.
    // Decided by VALUE and not by position, because position does not say what a
    // path is: `-MP` emits a phony rule whose target is a HEADER, and that one
    // must be rewritten like any other or a consumer's depfile names a file that
    // does not exist there.
    constexpr std::string_view objectPath = R"(C:\Users\RUNNER~1\src\a.o)";
    auto const preserveObject = [objectPath](std::string_view span) {
        return span == objectPath ? std::string { span } : DealiasShortName(span);
    };

    constexpr std::string_view depFile = "C:\\Users\\RUNNER~1\\src\\a.o: C:\\Users\\RUNNER~1\\src\\a.cpp\\\n"
                                         "  C:\\Users\\RUNNER~1\\src\\inc\\h1.h\n"
                                         "\n"
                                         "C:\\Users\\RUNNER~1\\src\\inc\\h1.h:\n";

    auto const rewritten = PathCanon::RewritePaths(depFile, Grammar::GccDepfile, preserveObject);
    REQUIRE(rewritten.has_value());
    CHECK(*rewritten
          == "C:\\Users\\RUNNER~1\\src\\a.o: C:\\Users\\runneradmin\\src\\a.cpp\\\n"
             "  C:\\Users\\runneradmin\\src\\inc\\h1.h\n"
             "\n"
             "C:\\Users\\runneradmin\\src\\inc\\h1.h:\n");

    // The dependency on the `\`-continued line is reached too. gcc wraps at ~76
    // columns, so a walker that stopped at the first line would exempt most of a
    // real depfile rather than none of it.
    CHECK(rewritten->contains(R"(  C:\Users\runneradmin\src\inc\h1.h)"));
}

TEST_CASE("RewritePaths with an identity transform is a byte-exact no-op")
{
    // What makes reconciling twice safe: RecordManifest re-reconciles inputs its
    // caller already did, and a correctly-spelled build reconciles nothing at all.
    auto const identity = [](std::string_view path) {
        return std::string { path };
    };
    constexpr std::string_view depFile = "/b/a.o: /s/a.cpp /s/a\\ b.hpp\n"
                                         "\n"
                                         "/s/a.hpp:\n";
    auto const rewritten = PathCanon::RewritePaths(depFile, Grammar::GccDepfile, identity);
    REQUIRE(rewritten.has_value());
    CHECK(*rewritten == depFile);

    constexpr std::string_view notes = "Note: including file: /s/inc/h1.h\r\nunrelated\n";
    auto const notesOut = PathCanon::RewritePaths(notes, Grammar::ShowIncludes, identity);
    REQUIRE(notesOut.has_value());
    CHECK(*notesOut == notes);
}
