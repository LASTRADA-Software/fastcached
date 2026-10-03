// SPDX-License-Identifier: Apache-2.0
#include "StreamGrammar.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using FastCache::Testing::Unwrap;

namespace
{

constexpr std::string_view ProducerSource = R"(C:\work\aaa\src)";
constexpr std::string_view ProducerBuild = R"(C:\work\aaa\build)";

/// The checkout that replays the value.
[[nodiscard]] PathCanon::Layout Consumer()
{
    return PathCanon::Layout { .sourceRoot = R"(D:\ci\bbb\src)", .buildTree = R"(D:\ci\bbb\build)" };
}

/// One stream through the production seam, in production's order: tagged with what
/// `StreamGrammar` answers for the flavor (both streams, as the launcher tags both), canonicalized
/// by `CanonicalStoredValue` as the daemon and the node do on a STORE, decoded, and localized under
/// the consumer's roots by the grammar the stored region CARRIES -- which is what the hit path reads.
/// @param flavor The compiler family that wrote @p text.
/// @param text   The captured stream.
/// @return What the consumer's build sees.
[[nodiscard]] std::string Replayed(Flavor flavor, std::string_view text)
{
    CompileValue produced;
    produced.objectBlob = { std::byte { 0x64 }, std::byte { 0x86 } };
    produced.textRegions.push_back(TextRegion { .grammar = StreamGrammar(flavor), .bytes = std::string { text } });
    produced.textRegions.push_back(TextRegion { .grammar = StreamGrammar(flavor), .bytes = std::string { text } });

    auto const stored = CanonicalStoredValue(EncodeCompileValue(produced), ProducerSource, ProducerBuild);
    REQUIRE(stored.outcome == CanonicalizationOutcome::Canonicalized);
    auto const decoded = DecodeCompileValue(stored.bytes);
    REQUIRE(decoded.has_value());
    auto const& region = Unwrap(decoded).textRegions.at(1);
    return PathCanon::LocalizeRegion(region.bytes, region.grammar, Consumer());
}

/// A line of real compiler output with the capture's root replaced, and what the consumer must see.
struct Line
{
    std::string_view shape;    ///< What this row pins.
    std::string_view produced; ///< As the compiler wrote it on the producing checkout.
    std::string_view replayed; ///< As the consumer's build must read it.
    Flavor flavor;             ///< Which driver writes this shape.
};

/// Every line language an MSVC-family stream carries, measured on VS 18 Community (MSVC 14.51 and
/// its clang-cl) under `/c /showIncludes /W4 /FC`. The investigation measured C4100 in a header
/// only; the rest are the shapes it inferred, taken here from real output rather than recalled.
constexpr auto Lines = std::to_array<Line>({
    { .shape = "a /showIncludes note",
      .produced = "Note: including file: C:\\work\\aaa\\src\\inc/probe.h\r\n",
      .replayed = "Note: including file: D:\\ci\\bbb\\src\\inc\\probe.h\r\n",
      .flavor = Flavor::Cl },
    { .shape = "a warning in a header",
      .produced = "C:\\work\\aaa\\src\\inc\\probe.h(3): warning C4100: 'x': unreferenced parameter\r\n",
      .replayed = "D:\\ci\\bbb\\src\\inc\\probe.h(3): warning C4100: 'x': unreferenced parameter\r\n",
      .flavor = Flavor::Cl },
    { .shape = "a TU-level warning, whose path is the source itself",
      .produced = "C:\\work\\aaa\\src\\a.cpp(3): warning C4101: 'z': unreferenced local variable\r\n",
      .replayed = "D:\\ci\\bbb\\src\\a.cpp(3): warning C4101: 'z': unreferenced local variable\r\n",
      .flavor = Flavor::Cl },
    { .shape = "an error line",
      .produced = "C:\\work\\aaa\\src\\a.cpp(2): error C2660: 'f': function does not take 2 arguments\r\n",
      .replayed = "D:\\ci\\bbb\\src\\a.cpp(2): error C2660: 'f': function does not take 2 arguments\r\n",
      .flavor = Flavor::Cl },
    { .shape = "a note: continuation",
      .produced = "C:\\work\\aaa\\src\\a.cpp(1): note: see declaration of 'f'\r\n",
      .replayed = "D:\\ci\\bbb\\src\\a.cpp(1): note: see declaration of 'f'\r\n",
      .flavor = Flavor::Cl },
    { .shape = "a /diagnostics:caret head with a column",
      .produced = "C:\\work\\aaa\\src\\a.cpp(4,20): error C2065: 'y': undeclared identifier\r\n",
      .replayed = "D:\\ci\\bbb\\src\\a.cpp(4,20): error C2065: 'y': undeclared identifier\r\n",
      .flavor = Flavor::Cl },
    { .shape = "the caret's echoed source line, byte for byte",
      .produced = "int bad() { return y; }\r\n",
      .replayed = "int bad() { return y; }\r\n",
      .flavor = Flavor::Cl },
    { .shape = "the echoed source name", .produced = "a.cpp\r\n", .replayed = "a.cpp\r\n", .flavor = Flavor::Cl },
    { .shape = "clang-cl's warning, file(line,col): warning: ...",
      .produced = "C:\\work\\aaa\\src\\inc/probe.h(3,29): warning: unused parameter 'x' [-Wunused-parameter]\r\n",
      .replayed = "D:\\ci\\bbb\\src\\inc\\probe.h(3,29): warning: unused parameter 'x' [-Wunused-parameter]\r\n",
      .flavor = Flavor::ClangCl },
    { .shape = "clang-cl's note",
      .produced = "C:\\work\\aaa\\src\\a.cpp(2,3): note: 'old' has been explicitly marked deprecated here\r\n",
      .replayed = "D:\\ci\\bbb\\src\\a.cpp(2,3): note: 'old' has been explicitly marked deprecated here\r\n",
      .flavor = Flavor::ClangCl },
    { .shape = "clang-cl's error",
      .produced = "C:\\work\\aaa\\src\\a.cpp(4,20): error: use of undeclared identifier 'y'\r\n",
      .replayed = "D:\\ci\\bbb\\src\\a.cpp(4,20): error: use of undeclared identifier 'y'\r\n",
      .flavor = Flavor::ClangCl },
    { .shape = "clang-cl's GCC-style include chain",
      .produced = "In file included from C:\\work\\aaa\\src\\a.cpp:1:\r\n",
      .replayed = "In file included from D:\\ci\\bbb\\src\\a.cpp:1:\r\n",
      .flavor = Flavor::ClangCl },
    { .shape = "clang-cl's indented caret echo, byte for byte",
      .produced = "    3 | inline int unused_param(int x) { return 0; }\r\n",
      .replayed = "    3 | inline int unused_param(int x) { return 0; }\r\n",
      .flavor = Flavor::ClangCl },
});

} // namespace

TEST_CASE("Every line an MSVC-family stream carries replays into the consumer's checkout", "[stream-grammar]")
{
    for (auto const& line: Lines)
    {
        INFO(line.shape);
        CHECK(Replayed(line.flavor, line.produced) == line.replayed);
    }
}

TEST_CASE("A whole MSVC stream replays with no trace of the producing checkout", "[stream-grammar]")
{
    for (auto const flavor: { Flavor::Cl, Flavor::ClangCl, Flavor::Unknown })
    {
        std::string stream;
        for (auto const& line: Lines)
            stream += line.produced;
        auto const replayed = Replayed(flavor, stream);
        INFO(replayed);
        CHECK_FALSE(replayed.contains(R"(C:\work\aaa)"));
        CHECK_FALSE(replayed.contains("C:/work/aaa"));
        CHECK_FALSE(replayed.contains("<SRCROOT>"));
    }
}

TEST_CASE("The stream grammar per flavor", "[stream-grammar]")
{
    // The MSVC family carries notes AND diagnostics on one stream; the GNU family writes its
    // dependency record to a file and only diagnostics on the streams (#202).
    CHECK(StreamGrammar(Flavor::Cl) == PathCanon::Grammar::MsvcStream);
    CHECK(StreamGrammar(Flavor::ClangCl) == PathCanon::Grammar::MsvcStream);
    CHECK(StreamGrammar(Flavor::Unknown) == PathCanon::Grammar::MsvcStream);
    CHECK(StreamGrammar(Flavor::Gcc) == PathCanon::Grammar::GccDiagnostics);
    CHECK(StreamGrammar(Flavor::Clang) == PathCanon::Grammar::GccDiagnostics);
    // Both must be able to carry notes or not, as the note-only readers are told.
    CHECK(PathCanon::CarriesIncludeNotes(StreamGrammar(Flavor::Cl)));
    CHECK_FALSE(PathCanon::CarriesIncludeNotes(StreamGrammar(Flavor::Gcc)));
}
