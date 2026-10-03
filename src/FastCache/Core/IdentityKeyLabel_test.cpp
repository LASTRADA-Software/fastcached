// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::TestKeyPair;
using FastCache::Testing::Unwrap;

TEST_CASE("A labelled message is its construction's label, then exactly the fields it was given", "[core][identity-key]")
{
    auto const first = std::string_view { "first" };
    auto const second = std::string_view { "second" };
    for (auto const purpose: Enumerators<IdentityKeyPurpose>())
    {
        INFO(LabelOf(purpose));
        auto const message = LabelledMessage::Of(purpose, { WireFields::AsBytes(first), WireFields::AsBytes(second) });
        CHECK(message.Purpose() == purpose);
        auto const fields = WireFields::SplitAll(message.Bytes());
        REQUIRE(fields.has_value());
        REQUIRE(Unwrap(fields).size() == 3);
        CHECK(std::ranges::equal(Unwrap(fields)[0], WireFields::AsBytes(LabelOf(purpose))));
        CHECK(std::ranges::equal(Unwrap(fields)[1], WireFields::AsBytes(first)));
        CHECK(std::ranges::equal(Unwrap(fields)[2], WireFields::AsBytes(second)));
    }
}

TEST_CASE("A signature over one construction's message never verifies as another's over the same fields",
          "[core][identity-key]")
{
    // What the label is FOR, asked of the signing seam itself: the same key over the same fields,
    // and only the construction differs.
    auto const key = TestKeyPair("signer");
    auto const field = WireFields::AsBytes(std::string_view { "shared fields" });
    auto const lease = LabelledMessage::Of(IdentityKeyPurpose::Lease, { field });
    auto const signature = SignLabelled(key, lease);
    CHECK(VerifyLabelled(key.PublicKey(), lease, signature));
    for (auto const purpose: Enumerators<IdentityKeyPurpose>())
    {
        INFO(LabelOf(purpose));
        auto const other = LabelledMessage::Of(purpose, { field });
        CHECK(VerifyLabelled(key.PublicKey(), other, signature) == (purpose == IdentityKeyPurpose::Lease));
    }
}

namespace
{
/// One file's calls of a member function named `Sign`, and why none of them signs raw bytes.
struct SignCallRow
{
    std::string_view file; ///< Relative to the repository root, forward slashes.
    std::size_t calls;     ///< How many `.Sign(` or `->Sign(` the file holds, outside comments.
    std::string_view why;  ///< What the call is, and why it is not an unlabelled signature.
};

/// Every first-party, non-test call spelled `.Sign(` or `->Sign(`, file by file: the census of the
/// ONE construction the type cannot route -- `Ed25519KeyPair::Sign`, which takes raw bytes and lives
/// in `Core/Ed25519`, a file this change does not touch.
///
/// Its stated blind spot, and its direction: a raw signature spelled any other way -- through a
/// member pointer, `std::invoke`, or text after a `//` inside a string literal -- is not counted, so
/// this fails OPEN for those spellings; what it cannot miss is a new `pair.Sign(bytes)` anywhere under
/// `src/FastCache` or `src/apps`, in a file listed or not.
constexpr std::array SignCalls {
    SignCallRow { .file = "src/FastCache/Core/IdentityKeyLabel.hpp",
                  .calls = 1,
                  .why = "SignLabelled: the ONE raw call of Ed25519KeyPair::Sign, over a LabelledMessage's bytes" },
    SignCallRow { .file = "src/FastCache/Distributed/LeaseToken.hpp",
                  .calls = 1,
                  .why = "MintLeaseToken calls ILeaseSigner::Sign, which takes a LabelledMessage" },
    SignCallRow { .file = "src/FastCache/Consensus/RaftPeerSession.cpp",
                  .calls = 2,
                  .why = "IRaftPeerIdentity::Sign, which builds its LabelledMessage from a RaftPeerSignature" },
};

/// @param line One source line.
/// @return The line up to its first `//`.
[[nodiscard]] std::string_view CodeOf(std::string_view line)
{
    return line.substr(0, line.find("//"));
}

/// @param text A whole source file.
/// @return How many `.Sign(` and `->Sign(` it holds outside `//` comments.
[[nodiscard]] std::size_t SignCallsIn(std::string const& text)
{
    auto count = std::size_t { 0 };
    auto lines = std::istringstream { text };
    auto line = std::string {};
    while (std::getline(lines, line))
    {
        auto const code = CodeOf(line);
        auto at = code.find("Sign(");
        while (at != std::string_view::npos)
        {
            auto const receiver = code.substr(0, at);
            if (receiver.ends_with('.') || receiver.ends_with("->"))
                ++count;
            at = code.find("Sign(", at + 1);
        }
    }
    return count;
}

/// @param path A file.
/// @return Its bytes as text.
[[nodiscard]] std::string ReadWhole(std::filesystem::path const& path)
{
    std::ifstream input { path, std::ios::binary };
    std::ostringstream read;
    read << input.rdbuf();
    return std::move(read).str();
}

/// What the census found.
struct SignCallCensus
{
    std::size_t filesRead { 0 };                           ///< Every first-party, non-test source read.
    std::vector<std::pair<std::string, std::size_t>> hits; ///< Each file holding a call, and how many.
};

/// Read every first-party, non-test C++ source under `src/FastCache` and `src/apps`.
/// @return What was found.
[[nodiscard]] SignCallCensus TakeSignCallCensus()
{
    auto const root = std::filesystem::path { FASTCACHED_SOURCE_DIR };
    auto census = SignCallCensus {};
    for (auto const* const tree: { "src/FastCache", "src/apps" })
    {
        for (auto const& entry: std::filesystem::recursive_directory_iterator { root / tree })
        {
            auto const& path = entry.path();
            auto const name = path.filename().string();
            auto const isSource = path.extension() == ".cpp" || path.extension() == ".hpp";
            if (!entry.is_regular_file() || !isSource || name.ends_with("_test.cpp"))
                continue;
            ++census.filesRead;
            if (auto const calls = SignCallsIn(ReadWhole(path)); calls != 0)
                census.hits.emplace_back(std::filesystem::relative(path, root).generic_string(), calls);
        }
    }
    return census;
}
} // namespace

TEST_CASE("Every call named Sign in the tree is a row saying why it does not sign raw bytes", "[core][identity-key]")
{
    // What an identity key signs is a `LabelledMessage` by TYPE wherever the key is reached through
    // a seam; the one place the type cannot reach is `Ed25519KeyPair::Sign` itself, which takes any
    // bytes. So its calls are counted, and the only raw one allowed is `SignLabelled`'s.
    auto const census = TakeSignCallCensus();

    // The walk read the tree, not an empty directory: absence of a finding is worth nothing from a
    // census that looked at nothing.
    CHECK(census.filesRead > 200);

    for (auto const& [file, calls]: census.hits)
    {
        INFO(file << " holds " << calls
                  << " call(s) spelled .Sign( or ->Sign(. An identity key signs a LabelledMessage: sign through "
                     "SignLabelled, or a seam taking one. If this call IS such a seam, add a SignCalls row saying which.");
        auto const* const row = core::findOrNull(SignCalls, std::string_view { file }, &SignCallRow::file);
        REQUIRE(row != nullptr);
        CHECK(row->calls == calls);
    }

    // And every row still describes a file that calls: a stale row is a claim about a tree that no
    // longer exists, and the raw row going missing would mean `SignLabelled` stopped being the door.
    for (auto const& row: SignCalls)
    {
        INFO(row.file << ": " << row.why);
        CHECK(std::ranges::contains(census.hits, row.file, [](auto const& hit) { return std::string_view { hit.first }; }));
    }
}
