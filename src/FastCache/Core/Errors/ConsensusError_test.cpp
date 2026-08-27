// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace FastCache;

TEST_CASE("A consensus refusal is about the command or about the moment", "[core][consensus][errors]")
{
    // What a caller does with this is `RefusalSubject`'s own doc; what this pins is
    // that a code on the wrong side is a wrong answer HERE rather than, three layers
    // up, a cluster that quietly stops admitting anybody.
    //
    // Asserted at compile time throughout, which is the point of the table being
    // `constexpr`: a code appended without a row is already a build failure at
    // `RowsInEnumeratorOrder`, and this is the other half of that guard.

    // Exactly one describes the change itself. `Cluster::Validate` produces it, and
    // it is permanent: the same command offered next interval is refused again.
    static_assert(SubjectOf(ConsensusErrorCode::InvalidConfiguration) == RefusalSubject::Command);

    // Everything else describes this node or this instant, so the command is fine
    // and the next one would be refused identically -- which is what makes stopping
    // the right answer rather than skipping.
    static_assert(SubjectOf(ConsensusErrorCode::NotLeader) == RefusalSubject::Moment);
    static_assert(SubjectOf(ConsensusErrorCode::MalformedFrame) == RefusalSubject::Moment);
    static_assert(SubjectOf(ConsensusErrorCode::UnknownMessageType) == RefusalSubject::Moment);
    static_assert(SubjectOf(ConsensusErrorCode::UnsupportedVersion) == RefusalSubject::Moment);

    // The one worth pausing on. A durable write that failed says nothing about the
    // command -- so "permanent" is tempting -- but the next command would fail the
    // same way, and skipping to it would offer the whole list into a disk that is
    // not answering. What a caller needs from it is "stop", which is what `Moment`
    // means here.
    static_assert(SubjectOf(ConsensusErrorCode::StorageFailure) == RefusalSubject::Moment);

    // And the builders agree with the enumerators, so a caller that switches on a
    // constructed error reaches the same row.
    CHECK(SubjectOf(InvalidConfiguration("a member id is not valid UTF-8").code) == RefusalSubject::Command);
    CHECK(SubjectOf(NotLeader(std::string { "10.0.0.1:6680" }).code) == RefusalSubject::Moment);
    CHECK(SubjectOf(StorageFailure("the log could not be appended").code) == RefusalSubject::Moment);
}
