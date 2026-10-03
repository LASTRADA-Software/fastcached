// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/ForwardingSink.hpp>
#include <FastCache/Consensus/IRaftMessageSink.hpp>
#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

using namespace FastCache::Consensus;

namespace
{

/// Records every message handed on.
class RecordingSink final: public IRaftMessageSink
{
  public:
    /// @copydoc IRaftMessageSink::Deliver
    void Deliver(RaftMessage message) override
    {
        received.push_back(std::move(message));
    }

    std::vector<RaftMessage> received; ///< In arrival order.
};

/// @param term The vote's term, so two messages are distinguishable.
/// @return A vote response, the smallest message there is.
[[nodiscard]] RaftMessage Vote(std::uint64_t term)
{
    return RaftMessage { RequestVoteResponse {
        .term = Term { .value = term }, .decision = VoteDecision::Granted, .voterId = NodeId { "n1" } } };
}

} // namespace

TEST_CASE("A forwarding sink drops and counts what arrives before it is bound", "[consensus][raft][learner]")
{
    // The transport exists before the driver it delivers into, so a message could in principle
    // arrive before the bind. It is dropped by name -- never delivered through a null target.
    ForwardingSink forwarding;
    CHECK_FALSE(forwarding.Bound());
    forwarding.Deliver(Vote(1));
    forwarding.Deliver(Vote(2));
    CHECK(forwarding.DroppedUnbound() == 2);

    RecordingSink target;
    forwarding.Bind(target);
    CHECK(forwarding.Bound());
    CHECK(target.received.empty()); // what was dropped stays dropped

    forwarding.Deliver(Vote(3));
    REQUIRE(target.received.size() == 1);
    CHECK(std::get<RequestVoteResponse>(target.received[0]).term.value == 3);
    CHECK(forwarding.DroppedUnbound() == 2);
}

TEST_CASE("A forwarding sink bound before anything arrives delivers everything and drops nothing",
          "[consensus][raft][learner]")
{
    ForwardingSink forwarding;
    RecordingSink target;
    forwarding.Bind(target);

    forwarding.Deliver(Vote(1));
    forwarding.Deliver(Vote(2));
    CHECK(target.received.size() == 2);
    CHECK(forwarding.DroppedUnbound() == 0);
}
