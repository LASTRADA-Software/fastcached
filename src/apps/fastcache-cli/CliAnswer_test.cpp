// SPDX-License-Identifier: Apache-2.0
#include "CliAnswer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cli;

TEST_CASE("the connections' remarks come first and a sentence already said is not said again", "[cli][answer]")
{
    // lane-livestats S2: `live-stats node --addr=<nothing>` dialled RESP and 0xFC at one address and printed
    // `cannot reach 127.0.0.1:26999 (...)` twice -- one fault read as two. WHAT DISTINGUISHES: the repeat of a
    // connection's sentence goes, a repeat of it in the verb's own remarks goes, the ORDER is the first saying,
    // and a different sentence about the same address stays -- a dedup by address would drop that one too.
    auto answer = Concluded(Outcome::Unreachable, "cannot tell what this endpoint is (no 0xFC connection was opened)");
    answer.advisories.emplace_back("cannot reach 127.0.0.1:26999 (waiting for connect to complete failed)");

    auto const remarks = std::vector<std::string> {
        "cannot reach 127.0.0.1:26999 (waiting for connect to complete failed)",
        "cannot reach 127.0.0.1:26999 (waiting for connect to complete failed)",
        "cannot reach 127.0.0.1:26999 (connection refused)",
    };
    PrependRemarks(answer, remarks);

    CHECK(answer.advisories
          == std::vector<std::string> {
              "cannot reach 127.0.0.1:26999 (waiting for connect to complete failed)",
              "cannot reach 127.0.0.1:26999 (connection refused)",
              "cannot tell what this endpoint is (no 0xFC connection was opened)",
          });
    CHECK(answer.outcome == Outcome::Unreachable);

    // The control: nothing to say twice leaves every remark, connections' first.
    auto plain = Concluded(Outcome::Affirmative, "the verb's own");
    PrependRemarks(plain, std::vector<std::string> { "a connection's" });
    CHECK(plain.advisories == std::vector<std::string> { "a connection's", "the verb's own" });
}
