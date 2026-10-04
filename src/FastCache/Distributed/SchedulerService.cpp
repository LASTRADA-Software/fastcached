// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Distributed/DialHint.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// One row per refusal that moves a counter: the wire code, and which one.
    ///
    /// A table rather than a `Count(...)` call beside each `Refuse(...)`, for the
    /// reason the worker's own version records: a refusal that forgets its counter
    /// is invisible in exactly the situation an operator is trying to diagnose, and
    /// hand-written pairs are as many chances to forget one.
    ///
    /// Both fields are plain and neither carries a default member initializer, which
    /// is the same rule the worker's `RefusalTable` states and for the same reason: a
    /// row answering one of the two questions is not a row, and `ErrorCode` has no
    /// zero enumerator for `{}` to name in the first place.
    struct RefusalDescriptor
    {
        Wire::ErrorCode code;          ///< The refusal.
        IMetricsSink::Counter counter; ///< What the operator sees rise.
    };

    // `NoWorker`, `NoCapacity` and `Withdrawn` are deliberately absent: those three
    // codes are pick refusals, counted by `PickErrorTable`'s own row rather than by
    // this code-keyed lookup, because `Excluded` shares `NoWorker`'s wire code and
    // must not share its counter -- the row is the refusal, not the code.
    // `PickRefusalsCountOnce` is what keeps them from being added back here.
    constexpr std::array RefusalTable {
        RefusalDescriptor { .code = Wire::ErrorCode::AlreadyInFlight,
                            .counter = IMetricsSink::Counter::DispatchLeasesDuplicate },
        RefusalDescriptor { .code = Wire::ErrorCode::MalformedRegistration,
                            .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed },
        // Its own counter, and the split is the diagnostic: `UnknownLease` beside it
        // names a lease this scheduler issued and has since forgotten, while this one
        // was never issued at all -- so a rise is somebody forging releases, or, far
        // more likely, a launcher from before signed leases still handing back the
        // bare serial it was given. This comment used to draw the contrast as
        // counted-against-uncounted, which stopped being true when one of the three
        // release refusals gained a counter of its own (#1074); the contrast that
        // matters is WHO is described, and that is unchanged.
        RefusalDescriptor { .code = Wire::ErrorCode::LeaseUnauthorized,
                            .counter = IMetricsSink::Counter::DispatchLeasesUnauthorized },
        // A verb a joining machine sends, on a connection that proved no live identity (#178).
        // Counted, unlike `NotAMember`: an address that is admitted and still cannot join is either
        // a node nobody admitted or one whose proof is being refused, and both are worth a series.
        RefusalDescriptor { .code = Wire::ErrorCode::NodeIdentityRequired,
                            .counter = IMetricsSink::Counter::SchedulerRequestsRefusedNodeIdentityRequired },
        // An operator's control verb from a caller `--fleet-open` alone admitted. Counted: the
        // caller is ADMITTED and asked to change the fleet anyway, which on an open node is
        // somebody trying the decision half of the policy -- the refusal carrying the argument.
        RefusalDescriptor { .code = Wire::ErrorCode::IdentifiedCallerRequired,
                            .counter = IMetricsSink::Counter::SchedulerRequestsRefusedIdentifiedCallerRequired },
    };

    /// The refusals this service makes that deliberately move nothing.
    ///
    /// Named rather than merely absent, so "this code has no counter" and "somebody
    /// forgot to give this code a counter" are different states rather than the same
    /// silence. Each is a *client* or *cluster* condition rather than a fleet one: a
    /// malformed frame is a broken client, an unknown worker id is a scheduler that
    /// restarted, and the two gate refusals are policy answers -- counting any of them
    /// beside the capacity refusals would put noise into the numbers a fleet is sized
    /// from, which is the very split those counters exist to preserve.
    ///
    /// `UnknownLease` is listed here as a statement about THIS table, and it is no
    /// longer the whole story about that code. A release refused reaches
    /// `ReleaseRefusalTable` below instead, which is keyed on what was observed
    /// rather than on the code and counts one of its three rows; the entry here is
    /// what keeps the code-keyed lookup from counting it a second time, and
    /// `ReleaseRefusalsCountOnce` is what enforces that rather than leaving it to
    /// this comment (#1074).
    constexpr std::array UncountedRefusals { Wire::ErrorCode::NotLeader,
                                             Wire::ErrorCode::NotAMember,
                                             Wire::ErrorCode::MalformedFrame,
                                             Wire::ErrorCode::UnknownLease,
                                             // Cluster administration: an operator typed something, and what
                                             // they typed is not a fact about fleet capacity. Counting these
                                             // beside the lease refusals would put one person's typo into the
                                             // numbers a fleet is sized from.
                                             Wire::ErrorCode::NoCluster,
                                             Wire::ErrorCode::InvalidClusterChange,
                                             // Neither is an event. One is what a
                                             // healthy cluster answers while a change
                                             // it accepted replicates, and the other
                                             // is an idempotent request arriving
                                             // twice -- so a rise in either would
                                             // measure how often somebody retried,
                                             // not anything a fleet is sized from.
                                             Wire::ErrorCode::ClusterChangeInFlight,
                                             Wire::ErrorCode::ClusterChangeNotNeeded,
                                             Wire::ErrorCode::StorageWriteFailed };

    /// Whether every refusal this service can produce is accounted for exactly once.
    ///
    /// The completeness check the two tables exist to make possible: a refusal added
    /// to neither, or to both, is a build failure rather than a counter an operator
    /// discovers is missing while diagnosing a fleet.
    /// @return True when the two tables are disjoint.
    [[nodiscard]] consteval bool RefusalsAreDisjoint() noexcept
    {
        for (auto const& row: RefusalTable)
            for (auto const uncounted: UncountedRefusals)
                if (row.code == uncounted)
                    return false;
        return true;
    }

    static_assert(RefusalsAreDisjoint(), "a refusal either moves a counter or is listed as moving none, never both");

    /// What one way of resolving nothing answers with, and whether anything rises.
    ///
    /// **A second table because the one above is keyed on the CODE, and this is the
    /// case a code-keyed table provably cannot hold.** All three release refusals
    /// answer `UnknownLease` -- a client acts on them identically, and telling it
    /// apart would be a wire change for every deployed launcher -- while an operator
    /// does not: one of the three is the only evidence a fleet has that its lease
    /// bound does not fit its work, and the other two have several causes each. One
    /// code, three rows. That is the same shape `SurfaceRefusal` records for the two
    /// refusals sharing `MalformedFrame`, arriving at this service (#1074).
    ///
    /// The `rationale` is never sent and never read at run time. It is the forcing
    /// function `UncountedRefusal` carries for the same purpose: an author adding a
    /// fourth row cannot write it without answering "would a rise here mean
    /// something happened", and a bare row with no counter would otherwise spell
    /// *forgot* in the vocabulary of *decided*.
    struct ReleaseRefusalRow
    {
        LeaseTable::ReleaseRefusal reason;            ///< What the table observed.
        Wire::ErrorCode code;                         ///< What the client is told.
        std::optional<IMetricsSink::Counter> counter; ///< What rises, or nothing at all.
        std::string_view rationale;                   ///< Why it rises, or why nothing does.
        std::string_view detail;                      ///< Words for a person.
    };

    /// One row per `LeaseTable::ReleaseRefusal`, in enumerator order.
    constexpr EnumTable<LeaseTable::ReleaseRefusal, ReleaseRefusalRow> ReleaseRefusalTable { {
        { .reason = LeaseTable::ReleaseRefusal::Expired,
          .code = Wire::ErrorCode::UnknownLease,
          .counter = IMetricsSink::Counter::DispatchLeasesReleasedLate,
          .rationale = "the one release refusal with a single cause: this client held its lease longer than "
                       "the fleet's bound, which is the only evidence a site has that the bound is too short",
          .detail = "this lease had already expired; the job outlived it" },
        { .reason = LeaseTable::ReleaseRefusal::UnknownToken,
          .code = Wire::ErrorCode::UnknownLease,
          .counter = std::nullopt,
          .rationale = "four causes share it -- an ordinary second release, a token from an instance that no "
                       "longer exists whose number is not yet reissued, an unissued serial on a keyless fleet, "
                       "and a lease ReleaseWorker reclaimed from a client that was still compiling -- so a "
                       "rise would name no one condition, and counting the last beside Expired would claim a "
                       "bound is too short for an event that says nothing about the bound",
          .detail = "unknown or already-resolved lease" },
        { .reason = LeaseTable::ReleaseRefusal::KeyMismatch,
          .code = Wire::ErrorCode::UnknownLease,
          .counter = std::nullopt,
          .rationale = "a token from a previous instance whose number this one has since reissued, or a client "
                       "naming the wrong key for its own token; the two are indistinguishable from here, and "
                       "the first is what an ordinary scheduler restart looks like",
          .detail = "this lease token belongs to another key" },
    } };

    static_assert(RowsInEnumeratorOrder(ReleaseRefusalTable, &ReleaseRefusalRow::reason),
                  "ReleaseRefusalTable must hold one row per LeaseTable::ReleaseRefusal, in enumerator order");

    /// Whether a release refusal's counter is the only one its answer can move.
    ///
    /// These rows carry their own counter, so a code that ALSO appears in
    /// `RefusalTable` would be counted twice for one refusal -- once from the row and
    /// once from the code-keyed lookup. It is safe today only because `UnknownLease`
    /// is not in that table, and "safe by coincidence of another table's contents" is
    /// what this file's own release path already records as the failure worth
    /// guarding: a credential honoured on one verb and ignored on another is trusted
    /// by accident of which verb ran.
    /// @return True when no release refusal's code carries a code-keyed counter.
    [[nodiscard]] consteval bool ReleaseRefusalsCountOnce() noexcept
    {
        for (auto const& row: ReleaseRefusalTable)
            for (auto const& counted: RefusalTable)
                if (row.code == counted.code)
                    return false;
        return true;
    }

    static_assert(ReleaseRefusalsCountOnce(),
                  "a release refusal carries its own counter, so its code must not also carry a code-keyed one");

    /// A refusal decided by one verb before anything is proposed, which moves a counter of
    /// its own and ONLY that. `RefuseAs` is its only spend path: `Increment(row.counter)` beside
    /// `Refuse(row.code)` counts twice whenever the code carries a counter of its own.
    ///
    /// The shape `ReleaseRefusalRow` has and for its reason: the row is the REFUSAL, not the
    /// code, so two refusals may share a code and must not share a counter. Spent through
    /// `RefuseAs`, which never consults `RefusalTable` -- so a row may answer with a code that
    /// carries a code-keyed counter of its own (`MalformedRegistration`) and still move exactly
    /// the one it names.
    struct VerbRefusalRow
    {
        Wire::ErrorCode code;          ///< What the client is told.
        IMetricsSink::Counter counter; ///< What the operator sees rise.
    };

    /// Refuse by a verb's own row: its counter rises, and no other.
    /// @param metrics Where the counter is.
    /// @param row The refusal.
    /// @param message Words for a person.
    /// @return The reply.
    [[nodiscard]] SchedulerReply RefuseAs(IMetricsSink& metrics, VerbRefusalRow const& row, std::string message)
    {
        metrics.Increment(row.counter);
        return SchedulerReply {
            .status = Wire::Status::Error, .error = row.code, .message = std::move(message), .payload = {}
        };
    }

    /// A CLUSTER-ADMIT naming a key that is not one (#178).
    ///
    /// `InvalidClusterChange`, which `UncountedRefusals` lists as moving nothing because an
    /// operator's typo read back to them is not a fact about the fleet. **This one is
    /// counted anyway, because the typo is not the likely cause.** Both of this project's
    /// clients parse the key where it is typed, through the one parser, so what arrives
    /// here malformed was sent by a client that does not -- and every member it admits
    /// joins with no identity while its operator was shown a key. The wire door is the
    /// last place that can be told, which is why the refusal exists at all; the counter is
    /// what makes a client doing it on every admission visible to somebody other than the
    /// person reading its output.
    ///
    /// Never `MalformedFrame` either: the frame decoded, and what it carried is the thing
    /// refused. A client told *malformed frame* goes looking for a version mismatch.
    constexpr VerbRefusalRow MalformedAdmissionKey {
        .code = Wire::ErrorCode::InvalidClusterChange,
        .counter = IMetricsSink::Counter::ClusterAdmissionsRefusedMalformedKey,
    };

    /// What one verb answers a string it keeps with, when the string fails one of the two questions
    /// `RefuseUnkept` asks of it.
    struct KeptFieldRefusals
    {
        VerbRefusalRow notText; ///< Bytes that are not UTF-8.
        VerbRefusalRow tooLong; ///< Text longer than the field's ceiling.
    };

    /// A LEASE's two.
    ///
    /// `MalformedFrame` on the wire, and not `MalformedRegistration`, although that is the code the
    /// other verbs' refusals answer with: a client asking for a lease is not a worker registering,
    /// and the launcher files both codes under one cause anyway. Two rows sharing one code, because
    /// bytes that are not text and text longer than any real client writes are different causes, and
    /// one counter would say neither; ONE row for every too-long field, because that cause is one --
    /// the sentence names which field, and the ceilings are sizes of the same rule.
    constexpr KeptFieldRefusals LeaseFieldRefusals {
        .notText = { .code = Wire::ErrorCode::MalformedFrame, .counter = IMetricsSink::Counter::DispatchLeasesMalformed },
        .tooLong = { .code = Wire::ErrorCode::MalformedFrame, .counter = IMetricsSink::Counter::DispatchLeasesFieldTooLong },
    };

    /// A REGISTER's two, both `MalformedRegistration` -- the code a worker's registration has always
    /// been refused with, which its `DescribeOutcome` reads back into the node's log. The not-text
    /// row moves the counter that code has always moved; the too-long row moves its own, and only
    /// its own, which is what `RefuseAs` is for.
    constexpr KeptFieldRefusals RegistrationFieldRefusals {
        .notText = { .code = Wire::ErrorCode::MalformedRegistration,
                     .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed },
        .tooLong = { .code = Wire::ErrorCode::MalformedRegistration,
                     .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsFieldTooLong },
    };

    /// A NODE-ANNOUNCE's two, with the code and the not-text counter REGISTER's refusals use: the
    /// announcement is the machine half of the same statement about itself. Its too-long row is its
    /// own, because a machine refused here registers no worker, and a registration counter would
    /// name one.
    constexpr KeptFieldRefusals PresenceFieldRefusals {
        .notText = { .code = Wire::ErrorCode::MalformedRegistration,
                     .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed },
        .tooLong = { .code = Wire::ErrorCode::MalformedRegistration,
                     .counter = IMetricsSink::Counter::DispatchNodeAnnouncementsFieldTooLong },
    };

    /// What a consensus refusal becomes on the wire.
    struct ProposalRefusalRow
    {
        ConsensusErrorCode code;  ///< What consensus said.
        Wire::ErrorCode reported; ///< What the client is told.
    };

    /// One row per `ConsensusErrorCode`, in enumerator order: the wire code it
    /// becomes when a proposal is refused.
    ///
    /// A table for the reason the two above are: a `switch` here and a `switch`
    /// somewhere else drift, and a refusal reported under the wrong code sends an
    /// operator to fix something that was never wrong. The last three rows are
    /// peer-wire decode failures that a *local* proposal cannot produce -- they
    /// exist because a reader disagreed with a sender about some bytes, and no
    /// bytes are involved here -- so they map to the generic refusal, which is the
    /// closed-by-default answer rather than a claim about what happened.
    constexpr EnumTable<ConsensusErrorCode, ProposalRefusalRow> ProposalRefusals { {
        { .code = ConsensusErrorCode::InvalidConfiguration, .reported = Wire::ErrorCode::InvalidClusterChange },
        // The two #196 split out, and the split reaches the wire because it is the
        // wire that an operator reads. One says ask again in a moment and the other
        // says there was nothing to ask; reporting either as `InvalidClusterChange`
        // sends somebody to correct a record that is already correct.
        { .code = ConsensusErrorCode::ConfigurationChangeInFlight, .reported = Wire::ErrorCode::ClusterChangeInFlight },
        { .code = ConsensusErrorCode::MembershipUnchanged, .reported = Wire::ErrorCode::ClusterChangeNotNeeded },
        // The generic PERMANENT refusal, and deliberately not a wire code of its own (#178).
        // A code of its own would claim a client acts differently on it, and none does: both
        // this and `InvalidClusterChange` mean *this request will never be accepted as it
        // stands*, and the sentence carries which key and whose it was. The permanence is
        // stated where it is decided -- `RefusalSubjects` -- which is what a reconciler reads.
        { .code = ConsensusErrorCode::KeyRevoked, .reported = Wire::ErrorCode::InvalidClusterChange },
        { .code = ConsensusErrorCode::NotLeader, .reported = Wire::ErrorCode::NotLeader },
        { .code = ConsensusErrorCode::StorageFailure, .reported = Wire::ErrorCode::StorageWriteFailed },
        // A store another build wrote is judged when the node STARTS, which it then
        // refuses to do -- so a running leader has no proposal to refuse with it, and
        // the row is the closed-by-default answer the decode rows below give, for
        // their reason.
        { .code = ConsensusErrorCode::UnsupportedFormatVersion, .reported = Wire::ErrorCode::InvalidClusterChange },
        { .code = ConsensusErrorCode::MalformedFrame, .reported = Wire::ErrorCode::InvalidClusterChange },
        { .code = ConsensusErrorCode::UnknownMessageType, .reported = Wire::ErrorCode::InvalidClusterChange },
        { .code = ConsensusErrorCode::UnsupportedVersion, .reported = Wire::ErrorCode::InvalidClusterChange },
    } };

    static_assert(RowsInEnumeratorOrder(ProposalRefusals, &ProposalRefusalRow::code),
                  "ProposalRefusals must hold one row per ConsensusErrorCode, in enumerator order");

    /// The wire code a consensus refusal is reported as.
    /// @param code What consensus said.
    /// @return The wire code.
    [[nodiscard]] constexpr Wire::ErrorCode WireCodeFor(ConsensusErrorCode code) noexcept
    {
        return ProposalRefusals[static_cast<std::size_t>(code)].reported;
    }

    /// What a pick refusal becomes on the wire, and what rises when it does.
    struct PickErrorRow
    {
        PickError error;               ///< Why no worker could be chosen.
        Wire::ErrorCode reported;      ///< What the client is told.
        IMetricsSink::Counter counter; ///< What the operator sees rise.
        std::string_view detail;       ///< Words for a person; empty where the code says it all.
        /// Whether the refusal says the fleet serves no such TOOLCHAIN, and is remembered for the
        /// leader's `unserved-toolchain`. Only `NoWorker`: a full or withdrawn fleet serves the
        /// toolchain, and so does one whose every worker this client excluded -- naming it would
        /// send an operator to install a compiler they already have.
        bool unservedToolchain;
    };

    /// One row per `PickError`, in enumerator order: the wire code it becomes and the
    /// counter that rises for it.
    ///
    /// The counter travels WITH the row rather than through the code-keyed
    /// `RefusalTable`, because `Excluded` and `NoWorker` share a wire code and must
    /// not share a counter -- one names a fingerprint nobody serves, the other a
    /// fleet the client could not reach, and they are opposite fixes. This is the
    /// same shape `ReleaseRefusalTable` uses for `UnknownLease`'s three causes, for
    /// the same reason: the row is the refusal, not the code.
    constexpr EnumTable<PickError, PickErrorRow> PickErrorTable { {
        { .error = PickError::NoWorker,
          .reported = Wire::ErrorCode::NoWorker,
          .counter = IMetricsSink::Counter::DispatchLeasesNoWorker,
          .detail = {},
          .unservedToolchain = true },
        { .error = PickError::NoCapacity,
          .reported = Wire::ErrorCode::NoCapacity,
          .counter = IMetricsSink::Counter::DispatchLeasesNoCapacity,
          .detail = {},
          .unservedToolchain = false },
        { .error = PickError::Withdrawn,
          .reported = Wire::ErrorCode::Withdrawn,
          .counter = IMetricsSink::Counter::DispatchLeasesWithdrawn,
          .detail = {},
          .unservedToolchain = false },
        // The same CODE as the first row and a different COUNTER: the row is the refusal.
        { .error = PickError::Excluded,
          .reported = Wire::ErrorCode::NoWorker,
          .counter = IMetricsSink::Counter::DispatchLeasesAllExcluded,
          .detail = "every worker serving this toolchain is on this client's exclusion list",
          .unservedToolchain = false },
    } };

    static_assert(RowsInEnumeratorOrder(PickErrorTable, &PickErrorRow::error),
                  "PickErrorTable must hold one row per PickError, in enumerator order");

    /// Whether a pick refusal's counter is the only one its answer can move.
    ///
    /// `PickErrorTable` rows now carry their own counter, so a reported code that
    /// also appeared as a row of the code-keyed `RefusalTable` would be counted
    /// twice for one refusal -- once from the pick row and once from `Refuse`'s
    /// lookup. The same guard `ReleaseRefusalsCountOnce` makes for release refusals,
    /// here for pick refusals.
    /// @return True when no pick refusal's reported code carries a code-keyed counter.
    [[nodiscard]] consteval bool PickRefusalsCountOnce() noexcept
    {
        for (auto const& row: PickErrorTable)
            for (auto const& counted: RefusalTable)
                if (row.reported == counted.code)
                    return false;
        return true;
    }

    static_assert(PickRefusalsCountOnce(),
                  "a pick refusal carries its own counter, so its code must not also carry a code-keyed one");

    /// What one kept field holds, as `RefuseUnkept` asks it: its bytes when it is text, and its
    /// length either way.
    struct KeptReading
    {
        /// The bytes, when the field is text; absent for a list that is measured and never rendered
        /// as text, which the text question does not reach.
        std::optional<std::string_view> text;
        std::size_t size; ///< Its length in bytes.
    };

    /// @param value A field that is text.
    /// @return It, as a reading both questions are asked of.
    [[nodiscard]] constexpr KeptReading Text(std::string_view value) noexcept
    {
        return KeptReading { .text = value, .size = value.size() };
    }

    /// One field a verb carries that this scheduler KEEPS, and the most of it that is kept.
    ///
    /// Both questions in one row, so a kept string cannot be checked for text and forgotten for
    /// length: the frame's ceiling is `MaxControlPayload`, and a kept field is held for as long as
    /// its record lives. The sizing of each ceiling is at its constant.
    template <typename Record>
    struct KeptField
    {
        std::string_view name;              ///< What a refusal calls it.
        KeptReading (*read)(Record const&); ///< Where to read it.
        /// The longest a scheduler records, in bytes. Zero if a row forgets it, so the row refuses
        /// every value rather than none -- a missing ceiling fails closed.
        std::size_t ceiling {};
    };

    /// Every field a REGISTER carries that the worker's entry keeps, in one place -- the strings
    /// rendered on the fleet page, `/fleet.json` and `--cluster-status`, and the codec list read
    /// against every lease.
    ///
    /// A table rather than checks written out, because the failure this
    /// guards against is another field being added to `WorkerRegistration` and
    /// nobody remembering to check it -- which stays invisible until a peer sends
    /// one that is not text, or one a megabyte long, by which time the bytes are in
    /// the leader's view of the fleet and in everything rendered from it.
    ///
    /// The extent is DEDUCED rather than spelled out. A row count written beside the
    /// rows is a second place the same fact lives, and the two part company the first
    /// time somebody appends one -- which is not hypothetical here: this table has
    /// already grown its fourth row once.
    constexpr std::array RegistrationFields {
        KeptField<WorkerRegistration> { .name = "fingerprint",
                                        .read = [](WorkerRegistration const& r) { return Text(r.fingerprint); },
                                        .ceiling = Wire::MaxToolchainFingerprintBytes },
        KeptField<WorkerRegistration> { .name = "endpoint",
                                        .read = [](WorkerRegistration const& r) { return Text(r.endpoint); },
                                        .ceiling = Wire::MaxEndpointBytes },
        KeptField<WorkerRegistration> { .name = "version",
                                        .read = [](WorkerRegistration const& r) { return Text(r.version); },
                                        .ceiling = Wire::MaxNodeVersionBytes },
        // The fourth string this table's own comment anticipated. It matters more than
        // most: it is raw compiler output rather than anything this project composed,
        // so it is the likeliest field to arrive as bytes that are not text.
        KeptField<WorkerRegistration> { .name = "toolchain label",
                                        .read = [](WorkerRegistration const& r) { return Text(r.toolchainLabel); },
                                        .ceiling = Wire::MaxToolchainLabelBytes },
        // The fifth, and it comes from the same place the fourth does: the machine
        // itself, not this project. `gethostname` and `GetComputerNameExA` hand back
        // whatever the host is called, in whatever encoding the host chose -- so this
        // is a row rather than an exemption, and it is refused where it ENTERS. One
        // byte that is not UTF-8 makes `/fleet.json` unparseable for the whole fleet,
        // and a renderer that repaired it would be a second author of the value while
        // every surface that did not repair still carried the original (#1024).
        KeptField<WorkerRegistration> { .name = "display name",
                                        .read = [](WorkerRegistration const& r) { return Text(r.displayName); },
                                        .ceiling = Wire::MaxDisplayNameBytes },
        // Not a string, and kept all the same: the entry holds it for as long as the worker
        // heartbeats and reads it against every lease. Measured and never rendered as text, so
        // only the length question reaches it -- a codec id is a byte, not a character.
        KeptField<WorkerRegistration> {
            .name = "codec list",
            .read =
                [](WorkerRegistration const& r) { return KeptReading { .text = std::nullopt, .size = r.codecs.size() }; },
            .ceiling = Wire::MaxCodecListIds },
    };

    /// The strings a LEASE carries that this scheduler KEEPS, and so renders.
    ///
    /// The fingerprint and the label are kept when the lease is refused `no-worker`
    /// (`UnservedToolchains`, for the leader's `unserved-toolchain`), and the key is kept by the
    /// lease table and listed among the fleet page's outstanding leases -- so all three are text a
    /// peer sent entering the fleet's state, refused where they enter rather than repaired by
    /// whichever renderer meets them first. Sixteen unserved toolchains at the frame's bound would
    /// be a megabyte a peer chose, which is what each row's ceiling is for.
    constexpr std::array LeaseFields {
        KeptField<Wire::LeaseRequest> { .name = "key",
                                        .read = [](Wire::LeaseRequest const& r) { return Text(r.key); },
                                        .ceiling = Wire::MaxLeaseKeyBytes },
        KeptField<Wire::LeaseRequest> { .name = "fingerprint",
                                        .read = [](Wire::LeaseRequest const& r) { return Text(r.fingerprint); },
                                        .ceiling = Wire::MaxToolchainFingerprintBytes },
        KeptField<Wire::LeaseRequest> { .name = "toolchain label",
                                        .read = [](Wire::LeaseRequest const& r) { return Text(r.toolchainLabel); },
                                        .ceiling = Wire::MaxToolchainLabelBytes },
    };

    /// The strings a presence announcement carries that this scheduler KEEPS in the machine's row.
    ///
    /// Its own table rather than a reuse of `RegistrationFields`, which projects from
    /// `WorkerRegistration`: two records, two projections, one validator. Shorter for the
    /// honest reason -- a presence announcement carries no fingerprint, no toolchain label and
    /// no display name, because it registers nothing. Its condition rows are kept too, and are
    /// asked the same two questions against their own table's ceilings: `KeptPresenceValues`.
    constexpr std::array PresenceFields {
        KeptField<NodePresence> { .name = "endpoint",
                                  .read = [](NodePresence const& p) { return Text(p.endpoint); },
                                  .ceiling = Wire::MaxEndpointBytes },
        KeptField<NodePresence> { .name = "version",
                                  .read = [](NodePresence const& p) { return Text(p.version); },
                                  .ceiling = Wire::MaxNodeVersionBytes },
    };

    /// One kept field as a verb carried it: what a refusal calls it, what it holds and its ceiling.
    ///
    /// Flattened out of the tables above so a verb whose kept strings live in more than one record
    /// -- a NODE-ANNOUNCE's own fields and its condition rows -- asks each question over all of them
    /// before the next, exactly as a verb with one record does.
    struct KeptValue
    {
        std::string_view qualifier; ///< What the field belongs to, prefixed to its name; empty for the verb's own.
        std::string_view name;      ///< What a refusal calls the field.
        KeptReading reading;        ///< What the peer sent.
        std::size_t ceiling;        ///< The longest a scheduler records, in bytes.
    };

    /// Every row of @p fields, read from @p record.
    /// @param record What the verb carried.
    /// @param fields The strings of it this scheduler keeps.
    /// @return One value per row, in the table's order.
    template <typename Record, std::size_t Count>
    [[nodiscard]] std::array<KeptValue, Count> KeptValuesOf(Record const& record,
                                                            std::array<KeptField<Record>, Count> const& fields)
    {
        auto values = std::array<KeptValue, Count> {};
        std::ranges::transform(fields, values.begin(), [&record](KeptField<Record> const& field) {
            return KeptValue {
                .qualifier = {}, .name = field.name, .reading = field.read(record), .ceiling = field.ceiling
            };
        });
        return values;
    }

    /// Every string a NODE-ANNOUNCE carries that this scheduler keeps: `PresenceFields`, then every
    /// field of every condition row against the ceiling its column of `ConditionFieldTable` states --
    /// the table that says which fields a row HAS, so a field appended to it is checked without
    /// anybody remembering to (#1364).
    /// @param presence What the machine announced.
    /// @return The values, the machine's own first.
    [[nodiscard]] std::vector<KeptValue> KeptPresenceValues(NodePresence const& presence)
    {
        auto const own = KeptValuesOf(presence, PresenceFields);
        auto values = std::vector<KeptValue> { own.begin(), own.end() };
        if (presence.conditions.has_value())
            for (auto const& row: *presence.conditions)
                for (auto const& column: Wire::ConditionFieldTable)
                    values.push_back(KeptValue { .qualifier = "condition ",
                                                 .name = column.name,
                                                 .reading = Text(row.*column.member),
                                                 .ceiling = column.maxBytes });
        return values;
    }

    /// The refusal a verb's kept strings earn, if any.
    ///
    /// Refused where they ENTER, before anything can keep a byte of them, rather than repaired by
    /// whichever renderer meets them first: a renderer that repaired one would be a second author of
    /// what the peer said, and the surfaces that did not would still carry the original. The whole
    /// request goes rather than the offending string -- a fingerprint, a key and an endpoint are
    /// matched byte for byte, so a repaired one would be a different request.
    ///
    /// Text first, over every value, then length: a string that is not text is the stronger claim
    /// about the peer, whichever field carries it.
    /// @param metrics Where the refusal's counter is.
    /// @param values What the verb carried that this scheduler keeps.
    /// @param refusals The verb's two refusals.
    /// @return The refusal, or nullopt when every text value is text and every value is within its ceiling.
    [[nodiscard]] std::optional<SchedulerReply> RefuseUnkept(IMetricsSink& metrics,
                                                             std::span<KeptValue const> values,
                                                             KeptFieldRefusals const& refusals)
    {
        for (auto const& kept: values)
            if (kept.reading.text.has_value() && !IsValidUtf8(*kept.reading.text))
                return RefuseAs(metrics, refusals.notText, NotTextRefusal(std::format("{}{}", kept.qualifier, kept.name)));
        for (auto const& kept: values)
            if (kept.reading.size > kept.ceiling)
                return RefuseAs(metrics,
                                refusals.tooLong,
                                std::format("{}{} is {} bytes; a scheduler records at most {}",
                                            kept.qualifier,
                                            kept.name,
                                            kept.reading.size,
                                            kept.ceiling));
        return std::nullopt;
    }

    /// Whether a registration's endpoint names the host it arrived from.
    ///
    /// **Hosts only, never ports**, which is forced rather than chosen: a peer dials
    /// from an ephemeral source port, so `peerId` carries none and there is nothing
    /// for an endpoint's port to be compared against. `Core/HostPort` owns the rest
    /// of the rule -- what an endpoint's host is, and when two hosts are the same
    /// machine.
    /// @param endpoint What the registration advertises, `host:port` or a bare host.
    /// @param peerHost The host the registration arrived from.
    /// @return True when the endpoint's host is the caller's own.
    [[nodiscard]] bool EndpointNamesCaller(std::string_view endpoint, std::string_view peerHost) noexcept
    {
        return SameHost(peerHost, HostOfEndpoint(endpoint));
    }

    /// Mismatch lines one leader will write before it goes quiet.
    ///
    /// The line carries the pair of addresses the counter cannot, and it is needed
    /// once per worker rather than once per registration: `Register` runs per
    /// toolchain and again on every re-registration, which a fleet does whenever a
    /// heartbeat is refused -- so an unhealthy fleet drives this hardest at exactly
    /// the moment its log is least readable. A cap keeps the diagnostic and drops the
    /// flood; the counter is what carries the rate afterwards, forever.
    ///
    /// Rate-limited rather than deduplicated because a table keyed on what a peer
    /// sent is a table a peer can grow, which this project already refuses for the
    /// discovery beacon's own provokable line.
    constexpr std::uint64_t MismatchLineBudget = 20;

    /// The counter a refusal moves, if any.
    /// @param code The refusal.
    /// @return Its counter, or nullopt when the code moves none.
    [[nodiscard]] constexpr std::optional<IMetricsSink::Counter> CounterFor(Wire::ErrorCode code) noexcept
    {
        // A range-based scan rather than `std::ranges::find`, and the reason is
        // portability rather than taste: over a `std::array`, libc++ and libstdc++
        // yield a raw pointer -- so clang-tidy's `readability-qualified-auto`
        // requires `auto const* const` -- while MSVC yields a class-type iterator
        // that such a declaration cannot deduce. There is no spelling of the
        // iterator that satisfies both. `CompileCacheWire::FindOp` already scans its
        // own table this way, so this is the tree's existing idiom as well as the
        // one that compiles everywhere.
        for (auto const& row: RefusalTable)
            if (row.code == code)
                return row.counter;
        return std::nullopt;
    }

    /// The keys a RELEASE is verified under: this scheduler's own, for its own id, and nothing
    /// else. A grant handed back here was signed here, or it names a serial this table never
    /// issued.
    class OwnSignature final: public ILeaseSignerKeys
    {
      public:
        /// @param signer This scheduler's identity.
        explicit OwnSignature(ILeaseSigner const& signer) noexcept:
            _signer { signer }
        {
        }

        [[nodiscard]] LeaseSignerKeys KeysOf(std::string_view signer) const override
        {
            if (signer != _signer.SignerId())
                return {};
            return LeaseSignerKeys { .live = _signer.PublicKey(), .revoked = {} };
        }

      private:
        ILeaseSigner const& _signer;
    };
} // namespace

SchedulerService::SchedulerService(core::platform::IClock& clock,
                                   core::platform::WallClockRef wallClock,
                                   IMetricsSink& metrics,
                                   ILogger& logger,
                                   ILeaseSigner const& signer,
                                   std::string_view clusterId):
    _wallClock { wallClock },
    _metrics { metrics },
    _logger { logger },
    _signer { signer },
    _clusterId { clusterId },
    _workers { clock },
    _leases { clock },
    _unserved { clock }
{
}

std::string SchedulerService::MintGrantToken(Distributed::Lease const& lease,
                                             std::string_view endpoint,
                                             std::string_view fingerprint)
{
    // The expiry comes from the LEASE, not from the table. A token that outlived its
    // lease would be a capability with no record anywhere; one that died first would
    // have a worker refusing work whose key this scheduler is still suppressing.
    //
    // It used to read `_leases.Timeout()` here, which was one derivation of the fact
    // and `Acquire`'s `issuedAt` stamp was another -- correct only while the lifetime
    // was a constant. It is a replicated setting since #522, so the two could be
    // separated by an operator changing it between the two statements. The lease
    // carries what it was granted under and every end reads that.
    return MintLeaseToken(_signer,
                          LeaseClaims { .serial = lease.token,
                                        .endpoint = std::string { endpoint },
                                        .fingerprint = std::string { fingerprint },
                                        .key = lease.key,
                                        .expiresAt = _wallClock.now() + lease.lifetime,
                                        .clusterId = _clusterId,
                                        .epoch = _epoch.load(std::memory_order_acquire),
                                        .signer = {} });
}

std::chrono::milliseconds SchedulerService::AgreedLeaseLifetime() const
{
    // No cluster is not a missing answer: it is the one-machine deployment, which has
    // no replicated state for anybody to set and must go on working with none.
    if (_admin == nullptr)
        return _leases.Timeout();

    auto const configured = _admin->ClusterState().SettingOf(Cluster::LeaseLifetimeSetting);
    if (!configured.has_value())
        return _leases.Timeout();

    // The SAME predicate `Validate` refuses with, so the two cannot disagree about
    // what a value means -- see `Cluster::ParseLeaseLifetime`.
    auto const parsed = Cluster::ParseLeaseLifetime(*configured);
    if (parsed.has_value())
        return *parsed;

    // Not fatal, and deliberately not a refusal to schedule. `Validate` runs on the
    // leader before the append, so no build in this tree can put an unreadable value
    // here; what can is a NEWER build with wider bounds, mid rolling upgrade, or an
    // OLDER one's value committed before this setting took a unit (#1402: `1200000`
    // names none) -- and a node that refused every lease on meeting one would take the
    // fleet down for an upgrade rather than for a fault. Serving under the default is
    // the outcome that degrades; saying so once is what keeps it from being silent.
    if (!_warnedLeaseLifetime.exchange(true, std::memory_order_relaxed))
        _logger.Logf(LogLevel::Warn,
                     "this cluster's {} is {}, which this build cannot read ({}); granting leases of {} until it "
                     "is set to something this build understands",
                     Cluster::LeaseLifetimeSetting,
                     *configured,
                     parsed.error(),
                     FormatDuration(_leases.Timeout()));
    return _leases.Timeout();
}

void SchedulerService::SetRole(SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t epoch)
{
    auto observer = std::function<void(SchedulerRole)> {};
    {
        std::scoped_lock const guard { _leaderMutex };
        _leaderEndpoint.assign(leaderEndpoint);
        observer = _roleObserver;
    }
    // Before the role, for the reason the endpoint is: a thread that has seen
    // `Leader` must already be able to see the term it leads under, or the first
    // grant after an election carries the previous one.
    _epoch.store(epoch, std::memory_order_release);
    // Published after the endpoint it describes, so a reader that sees `Leader`
    // has already been able to see the address that came with it.
    _role.store(role, std::memory_order_release);
    // After the role is published, and outside the lock, so an observer may read this service.
    if (observer)
        observer(role);
}

void SchedulerService::ObserveRole(std::function<void(SchedulerRole)> observer)
{
    std::scoped_lock const guard { _leaderMutex };
    _roleObserver = std::move(observer);
}

SchedulerReply SchedulerService::Offer(Cluster::Command const& command)
{
    // Asked here as well as by whoever proposes, and `Validate` in full rather than
    // the one rule this surface happens to care about.
    //
    // The honest statement of why: `IClusterAdmin` is an untrusted seam. Its contract
    // says nothing about validation, so an implementation that skipped it would take
    // an empty key, an endpointless member and an unknown setting name as readily as
    // a name that is not text -- and this surface is what answers an operator. With
    // the only implementation in this tree, `ConsensusTier`, the reply is identical
    // either way, so what this buys is that the answer does not depend on which
    // implementation is behind the seam.
    //
    // Safe to refuse here precisely because this door is one-shot: an operator typed
    // `--cluster-admit`, `--cluster-set` or `--cluster-forget` and is reading the
    // answer. The reconciler is where the identical refusal is a trap, because it
    // would re-offer the same command every interval forever.
    //
    // Against the state as this node holds it (#178), so a revoked key or one somebody
    // else holds is refused HERE, where the operator reads the answer; the proposer below
    // asks the same question again, and `Apply` enforces it on commit whatever either said.
    if (auto const allowed = Cluster::ValidateAgainst(_admin->ClusterState(), command); !allowed.has_value())
        return Refuse(WireCodeFor(allowed.error().code), allowed.error().context);

    auto const proposed = _admin->ProposeToCluster(command);
    if (proposed.has_value())
        // Appended, not committed, and the reply says only that. A leader cannot
        // know the difference until a majority answers, and an operator who wants
        // to see the result asks for the state again -- which is a round trip they
        // were going to make anyway.
        return SchedulerReply::Success();

    // The context rather than the code as the message, because these refusals are
    // read by a person: "no such cluster setting: upsteam" is actionable and a
    // numeric code is not. `NotLeader` is the exception the other way -- its
    // message is a machine-readable endpoint a client redirects to -- so it names
    // the leader when consensus knew one.
    auto const& error = proposed.error();
    auto const code = WireCodeFor(error.code);
    if (code == Wire::ErrorCode::NotLeader)
        return Refuse(code, error.knownLeader.value_or(std::string {}));
    return Refuse(code, error.context);
}

SchedulerReply SchedulerService::ClusterStatus(CallerContext const& caller)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    return SchedulerReply::Success(Cluster::Encode(_admin->ClusterState()));
}

SchedulerReply SchedulerService::ClusterSet(CallerContext const& caller, std::string_view name, std::string_view value)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    return Offer(Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                    .key = std::string { name },
                                    .value = std::string { value },
                                    .schedulerEndpoint = {},
                                    .publicKey = std::nullopt });
}

SchedulerReply SchedulerService::ClusterForget(CallerContext const& caller, std::string_view memberId)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    return Offer(Cluster::Command { .kind = Cluster::CommandKind::Forget,
                                    .key = std::string { memberId },
                                    .value = {},
                                    .schedulerEndpoint = {},
                                    .publicKey = std::nullopt });
}

SchedulerReply SchedulerService::ClusterAdmit(CallerContext const& caller,
                                              std::string_view memberId,
                                              std::string_view raftEndpoint,
                                              std::optional<std::string_view> schedulerEndpoint,
                                              std::optional<std::string_view> publicKey,
                                              std::optional<Cluster::MemberSeat> seat)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    // The key is read HERE, through the one parser, before anything is proposed (#178):
    // whatever client sent it, the leader is the last place anybody can be told, and a
    // command carrying bytes it guessed at would be applied after it is committed with
    // nobody left to refuse it. A key that is REVOKED is `ValidateAgainst`'s refusal, in
    // `Offer`, because that one is a fact about the replicated state and this one is not.
    auto key = std::optional<Ed25519PublicKey> {};
    if (publicKey.has_value())
    {
        auto parsed = ParseEd25519PublicKey(*publicKey);
        if (!parsed.has_value())
            return RefuseAs(_metrics,
                            MalformedAdmissionKey,
                            std::format("{} was sent with a key that is not one ({}): {}",
                                        memberId,
                                        *publicKey,
                                        DescribePublicKeyFault(parsed.error())));
        key = *parsed;
    }

    // The `0xFC` endpoint is the member's word: the one its `Enroll` stated, or -- for an
    // operator's verb, which states none -- the one already recorded, since `AddMember` applies
    // wholesale and an absent value would CLEAR it. A promotion is the same machine at the same
    // port; a member that moved announces its new endpoint itself, proven, and the leader
    // re-proposes its record (`AnnounceNode`).
    //
    // **Kept only for the SAME machine** -- the key unchanged, or absent, which keeps the recorded
    // key. A re-admit naming ANOTHER key records a REPLACED machine, and the old one's endpoint is
    // where a resolver would then dial expecting the new key: so it clears, and `Cleared` means
    // exactly "a machine was replaced and has not announced yet" until the new one does.
    auto const state = _admin->ClusterState();
    auto const* const recorded = core::findOrNull(state.members, memberId, &Cluster::ClusterMember::id);
    auto const sameMachine = recorded != nullptr && (!key.has_value() || recorded->publicKey == key);
    auto const kept = sameMachine ? std::string_view { recorded->schedulerEndpoint } : std::string_view {};
    auto endpoint = std::string { schedulerEndpoint.value_or(kept) };

    // The verb is the seat's (#1449): `MemberSeatTable` is the one statement of which
    // command records which set, so a promotion is this call with `Voter` on a learner
    // and a demotion is this call with `Learner` on a voter. A caller with no opinion --
    // an enrollment approval, which recovery repeats -- keeps the seat already recorded,
    // or a voter for a member there is no record of: absent is not `Voter`, or a
    // re-approval would promote a member the operator demoted.
    auto const resolved = seat.has_value() ? *seat : Cluster::RecordedSeatOf(state, memberId);
    auto const command = Cluster::Command { .kind = Cluster::MemberSeatTable[static_cast<std::size_t>(resolved)].admittedBy,
                                            .key = std::string { memberId },
                                            .value = std::string { raftEndpoint },
                                            .schedulerEndpoint = std::move(endpoint),
                                            .publicKey = key };

    auto reply = Offer(command);
    if (reply.status != Wire::Status::Ok)
        return reply;

    // **The receipt is attached HERE and not inside `Offer`, which is a decision and
    // not simply the narrower of two spellings.** `Offer` is a MECHANISM -- put this
    // command to consensus, translate the outcome -- shared with `ClusterSet` and
    // `ClusterForget`, and a reply BODY is the answer to a verb. Letting the mechanism
    // choose it would hand three verbs one answer because they share a transport,
    // which is the collapse `RefusalTable` avoids by keying on the refusal rather than
    // on the wire code it happens to be sent under.
    //
    // And the hazard is `AddMember`'s alone. What #1296 is about is two spellings of
    // one ADDRESS on two machines disagreeing, and `AddMember` is the only command
    // that carries an address at all. `SetSetting` refuses an unknown key BY NAME
    // already, and its value is read back by CLUSTER-STATUS, a verb that exists for
    // exactly that; `Forget` names a member the cluster is already holding, so
    // it has no second machine to disagree with.
    //
    // The third reason is about the verb after these three rather than about them: a
    // payload every command gets is a payload no command is responsible for, and a
    // fourth `CommandKind` would inherit a reply shape nobody decided it should have.
    //
    // It costs one conditional and duplicates nothing -- `Validate`, the refusal
    // translation and `NotLeader`'s endpoint all stay in `Offer`, which this still
    // goes through.
    //
    // Read off `command` rather than off the parameters, deliberately: the receipt's
    // one claim is *these are the bytes I wrote down*, so it is taken from the thing
    // that was written down. Spelling the parameters again here would make the two
    // able to disagree, which is the whole defect one level in.
    //
    // The key by the same rule, and it is where the rule earns its keep (#178): the text a
    // client sent and the key the command carries can differ, since the one is PARSED into
    // the other, so the receipt spells the command's key back through the one encoder.
    reply.payload = Wire::EncodeClusterAdmitReceipt(
        Wire::ClusterAdmitReceipt { .memberId = command.key,
                                    .raftEndpoint = command.value,
                                    .publicKey = command.publicKey.transform([](Ed25519PublicKey const& recorded) {
                                        return FormatEd25519PublicKey(recorded);
                                    }) });
    return reply;
}

std::optional<SchedulerReply> SchedulerService::RefuseUnlessIdentified(CallerContext const& caller, Wire::Op op) const
{
    // Asked of a caller the surface ADMITS: one it does not is membership's refusal, which the door
    // answers first and `Gate` answers again after the payload. Answering it here instead would give
    // `Answer` a different refusal from the door's for one stranger, where the two must be the same
    // bytes.
    auto const* const descriptor = Wire::FindOp(static_cast<std::uint8_t>(op));
    if (descriptor == nullptr || caller.membership != Membership::Member)
        return std::nullopt;
    auto const& row = RequirementRowOf(descriptor->identity);
    if (row.satisfiedBy(caller))
        return std::nullopt;
    return Refuse(row.refusal, std::format("{} {}", descriptor->name, row.remedy));
}

std::optional<Cluster::ClusterState> SchedulerService::AdministeredState() const
{
    if (_admin == nullptr)
        return std::nullopt;
    return _admin->ClusterState();
}

SchedulerReply SchedulerService::Refuse(Wire::ErrorCode code, std::string message) const
{
    if (auto const counter = CounterFor(code); counter.has_value())
        _metrics.Increment(*counter);
    return SchedulerReply { .status = Wire::Status::Error, .error = code, .message = std::move(message), .payload = {} };
}

std::optional<SchedulerReply> SchedulerService::Gate(CallerContext const& caller, GateScope scope) const
{
    // Leadership first, and not only because it is cheaper to answer. A follower
    // holds a registry that is a stale copy of somebody else's, so admitting a
    // worker here would put it in a fleet nothing schedules onto -- and the worker
    // would heartbeat happily into it forever. Refusing with the leader's address
    // is what turns that into one redirect.
    //
    // Skipped for a settlement, and `GateScope`'s comment carries the whole of why:
    // a release resolves a lease this node minted, in a table nobody else has a copy
    // of. There is no decision here for a leader to be making instead.
    if (scope == GateScope::Scheduling && Role() != SchedulerRole::Leader)
        return Refuse(Wire::ErrorCode::NotLeader, LeaderEndpoint());

    // Then the anti-leeching rule, which is the half the transport can also ask
    // before it reads a payload -- so it lives in one function that both call.
    return RefuseUnlessMember(caller);
}

std::optional<SchedulerReply> SchedulerService::RefuseUnlessMember(CallerContext const& caller) const
{
    // A non-member is *not* refused the cache -- it reads and writes objects exactly
    // as before -- it is refused the fleet's CPU time, which is the thing membership
    // pays for.
    if (caller.membership != Membership::Member)
        return Refuse(Wire::ErrorCode::NotAMember);

    return std::nullopt;
}

SchedulerReply SchedulerService::Register(CallerContext const& caller, WorkerRegistration const& registration)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // Checked HERE, where the wire becomes fleet state, rather than in whichever
    // renderer notices first. Everything below copies these strings into the
    // leader's view of the fleet, and `/fleet.json`, `/fleet`, `--cluster-status`
    // and the logs all read them back out again -- so a renderer that repaired
    // them would be a second place the value is decided, and the surfaces that did
    // not repair would still carry the originals.
    //
    // The whole registration goes, not the offending field: the fingerprint is
    // matched byte for byte, so a worker admitted with a blanked-out one would
    // match nothing and sit in the fleet never being picked. A refusal reaches the
    // worker's own log through `DescribeOutcome`; the counter is what an operator
    // sees when the peer is not one of ours and never says anything at all.
    //
    // Length as well as encoding: every one of these strings is kept for as long as the worker
    // heartbeats, so each has a ceiling of its own far below the frame's.
    if (auto refusal = RefuseUnkept(_metrics, KeptValuesOf(registration, RegistrationFields), RegistrationFieldRefusals);
        refusal.has_value())
        return std::move(*refusal);

    // No zero-slot refusal any more, and its removal is a decision rather than a
    // simplification. A zero used to mean "a worker that will never be picked" and
    // was refused for that reason; since `OfferableSlots` it means "size me from my
    // own hardware", which is the *preferred* spelling -- a node that computed its
    // own slot count would be the one place a workstation's reserve could be got
    // wrong with nothing downstream able to tell. The failure the refusal protected
    // against is closed by construction instead: the wire's zero is a request to
    // derive (`RequestedSlots`) and a derived count is never zero, so no registration
    // can produce a worker that matches leases and is never picked. An operator's
    // zero (#206) never registers at all.

    // Measured, deliberately NOT refused (#242).
    //
    // A registration asserts where work for a toolchain should be sent and nothing
    // ties that claim to the connection carrying it. Refusing a mismatch was priced
    // and REJECTED -- it turns away the documented setup, and stops only a third
    // host, since membership already admitted this one. The full argument, and what
    // closing it properly needs, is the #242 entry in
    // `.agent/rules/distributed-compilation.md`; do not re-derive it from here.
    //
    // So: a counter, a bounded line, and no wire code at all -- one that nothing
    // returns would put a lie in the refusal table.
    if (!EndpointNamesCaller(registration.endpoint, caller.peerId))
    {
        _metrics.Increment(IMetricsSink::Counter::DispatchWorkerEndpointMismatch);

        // `Info`, not a warning. On a fleet that advertises DNS names this is every
        // registration and nothing is wrong, and a signal that fires permanently on
        // correct deployments is one operators learn to filter -- so it would not be
        // there on the day it means something. Info is the default production level,
        // so it is still read.
        if (auto const written = _mismatchLines.fetch_add(1, std::memory_order_relaxed); written < MismatchLineBudget)
            _logger.Logf(LogLevel::Info,
                         "dispatch: worker at {} registered endpoint {} for {}, which this scheduler does not verify "
                         "(#242); expected with DNS names, NAT, VPN or multi-homing{}",
                         caller.peerId.empty() ? std::string_view { "an unnameable peer" } : caller.peerId,
                         registration.endpoint,
                         registration.fingerprint,
                         written + 1 == MismatchLineBudget ? " -- further mismatches are counted only" : "");
    }

    // Where it was seen is the KERNEL's fact, never the registration's claim: whatever a
    // caller put in `observedHost` is overwritten with the connection's own peer.
    auto seen = registration;
    seen.observedHost = caller.peerId;
    auto const id = _workers.Register(seen);

    // A re-registration deliberately does NOT release this worker's leases, even
    // though it resets `inFlight` two lines above on the reasoning that whatever it
    // was running is gone. The two are not the same bet: a node re-registers after
    // any refused heartbeat -- `EndpointBusy`, or `NotLeader` during an election --
    // and not only after a restart, so releasing here would wipe leases for compiles
    // that are still running and hand a second client the same work. `inFlight`
    // recovers from that guess at the next heartbeat; a lease does not.
    //
    // A worker that genuinely restarted needs nothing here anyway: its client's
    // compile exchange fails, and the client resolves its own lease on that path
    // like every other (#212).

    // Counted as an event, not as fleet size. This interface is counter-only, so it
    // cannot express a gauge -- and the event turns out to be the more useful
    // number anyway: a rate that stays high means workers keep re-registering,
    // which is what a fleet whose heartbeats are not arriving looks like from the
    // scheduler's side.
    _metrics.Increment(IMetricsSink::Counter::DispatchWorkerRegistrations);

    // The reply carries the fleet identity and the term, not only the id. This is
    // the one exchange in which a worker talks to the scheduler it was CONFIGURED to
    // reach, so it is the only place an identity can be handed over without either
    // end inferring it (#401).
    return SchedulerReply::Success(Wire::EncodeRegisterReply(
        { .workerId = std::string { id }, .clusterId = _clusterId, .epoch = _epoch.load(std::memory_order_relaxed) }));
}

void SchedulerService::SetHistorySink(IFleetHistorySink* sink) noexcept
{
    _history = sink;
}

SchedulerReply SchedulerService::AnnounceNode(CallerContext const& caller,
                                              NodePresence const& presence,
                                              std::span<FleetBucket const> history)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // Refused where it ENTERS, through the same gate the registration goes through: one byte
    // that is not UTF-8 makes the fleet document unparseable for the whole fleet, one string as
    // long as the frame is held in the machine's row for as long as it announces, and a renderer that
    // repaired either would be a second author of the value. Every string of every condition row
    // included, for the same reason (#1364).
    if (auto refusal = RefuseUnkept(_metrics, KeptPresenceValues(presence), PresenceFieldRefusals); refusal.has_value())
        return std::move(*refusal);

    // The endpoint is the KEY, so an empty one is not a machine that declined to say where it
    // answers -- it is a row that would collide with every other machine that did the same.
    if (presence.endpoint.empty())
        return Refuse(Wire::ErrorCode::MalformedRegistration, "a machine announces the endpoint it answers on");

    _workers.NoteNodePresent(std::string { presence.endpoint },
                             presence.capacity,
                             presence.load,
                             std::string { presence.version },
                             presence.conditions);

    // Filed under the ENDPOINT, exactly as a worker's batch is. The node's cursor advances only
    // because this verb was accepted, which is the rule `Heartbeat` already follows.
    if (_history != nullptr && !history.empty())
        _history->AcceptHistory(std::string { presence.endpoint }, history);

    // The fleets it once asked, filed apart from the history above and under the id the caller
    // PROVED rather than the endpoint it names: they are the evidence a split of THIS fleet is told
    // to an operator on, not a reading of the machine, and evidence is worth only whose it provably is.
    RecordJoinMemos(caller, presence.joinMemos);

    // Where its `0xFC` port answers, under the id it PROVED -- never one the payload names, so a
    // member only ever speaks for itself -- and only for a member the state records: an
    // announcement admits nobody. Noted when it differs from the record; the leader's reconcile
    // re-proposes the record (`IClusterAdmin::NoteAnnouncedEndpoint`).
    NoteAnnouncedEndpoint(caller, presence.endpoint);
    return SchedulerReply::Success();
}

void SchedulerService::NoteAnnouncedEndpoint(CallerContext const& caller, std::string_view endpoint)
{
    if (!caller.provenNodeId.has_value() || _admin == nullptr)
        return;
    auto const state = _admin->ClusterState();
    auto const* const recorded = core::findOrNull(state.members, *caller.provenNodeId, &Cluster::ClusterMember::id);
    if (recorded == nullptr || recorded->schedulerEndpoint == endpoint)
        return;
    _admin->NoteAnnouncedEndpoint(*caller.provenNodeId, std::string { endpoint });
}

void SchedulerService::RecordJoinMemos(CallerContext const& caller, std::span<Wire::JoinMemoFields const> memos)
{
    // Under the id the caller PROVED, never one it named: a machine's memos are evidence only for
    // the machine that asked. And only while the state records it -- one this fleet does not record
    // speaks for nobody in it -- so a machine that leaves takes its memos with it at the next
    // announcement anybody makes.
    if (!caller.provenNodeId.has_value() || _admin == nullptr)
        return;
    auto const state = _admin->ClusterState();
    auto const recorded = [&state](std::string_view id) {
        return std::ranges::contains(state.members, id, &Cluster::ClusterMember::id);
    };

    std::scoped_lock const lock { _joinMemosMutex };
    std::erase_if(_joinMemos, [&recorded](auto const& entry) { return !recorded(entry.first); });
    if (!recorded(*caller.provenNodeId))
        return;
    auto& filed = _joinMemos[*caller.provenNodeId];
    filed.clear();
    for (auto const& memo: memos)
        filed.push_back(Cluster::AskedJoinBy {
            .askerId = *caller.provenNodeId, .clusterId = memo.clusterId, .provenKey = memo.provenKey });
}

std::vector<Cluster::AskedJoinBy> SchedulerService::AnnouncedJoinMemos() const
{
    std::scoped_lock const lock { _joinMemosMutex };
    auto all = std::vector<Cluster::AskedJoinBy> {};
    for (auto const& [asker, memos]: _joinMemos)
        all.insert(all.end(), memos.begin(), memos.end());
    return all;
}

SchedulerReply SchedulerService::Heartbeat(CallerContext const& caller,
                                           std::string_view workerId,
                                           NodeLoad const& load,
                                           std::span<FleetBucket const> history,
                                           std::span<std::string const> interfaceAddresses)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // An unknown id is answered, not ignored: it means the scheduler restarted or
    // expired this worker, and the worker's correct response is to register again.
    // Silence would leave it heartbeating into a void forever while the fleet ran
    // without it.
    //
    // Where it was seen comes from the connection, on EVERY beat: a VPN reconnect moves the
    // worker's address while its process, and so its registration, stays up.
    auto const endpoint = _workers.Heartbeat(
        workerId, load, WorkerAddresses { .observedHost = caller.peerId, .interfaceAddresses = interfaceAddresses });
    if (!endpoint.has_value())
        return Refuse(Wire::ErrorCode::UnknownLease, "unknown worker; register again");

    // Under the ENDPOINT, never the worker id: a machine with two `--toolchain`
    // flags heartbeats twice with the same figures, and keying per id would have the
    // fleet counting one machine's contribution once per toolchain -- the rule
    // `WorkerRegistry::NodeCaches()` already exists to enforce on the neighbouring
    // number.
    if (_history != nullptr && !history.empty())
        _history->AcceptHistory(*endpoint, history);

    // **This reply deliberately carries no term.** #421 first put the scheduler's term
    // here, and review found it: this channel is unauthenticated -- `Credential` is
    // client-to-server, the frame surface is plaintext, and a reply has no integrity
    // protection at all. Anything that can answer a worker's `--scheduler` dial could
    // hand it a term of `UINT64_MAX` once and that worker would refuse every authentic
    // grant until its process restarted, which is the fleet ceasing to distribute --
    // the exact failure #421 exists to prevent, caused by #421, from one packet.
    //
    // A worker learns the term from the grant instead, where the MAC has already run.
    // Deleting the surface rather than defending it is what this repository does when
    // both are available: `CacheResponder` takes no membership oracle, and its absence
    // IS the fix.
    return SchedulerReply::Success();
}

SchedulerReply SchedulerService::Withdraw(CallerContext const& caller, std::string_view workerId)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // **An unknown id is `Ok`, and this is where it diverges from `Heartbeat` on
    // purpose.** That one refuses `UnknownLease` in order to PROVOKE re-registration,
    // which is the opposite of what a withdrawal wants: the end state the caller is
    // asking for -- this registration is gone -- is already true. A worker that lost
    // the race to its own expiry, or that is talking to a scheduler which has
    // restarted, has succeeded rather than failed, and refusing it would have a node
    // log a refusal for getting what it asked for. It also keeps the reply set at
    // `Ok | Error`, which is what leaves `MinSupportedVersion` where it is.
    if (!_workers.Remove(workerId))
        return SchedulerReply::Success();

    // Two locks one after the other rather than one spanning both, for
    // `ReapExpiredWorkers`' reason, and the follow-up is not optional: a worker being
    // dropped is an EVENT, or nothing releases what was held against it. Reached
    // deliberately here instead of by timeout, but the obligation is identical.
    //
    // **A client mid-compile under one of these leases will resolve its token on the
    // way out and meet `UnknownLease`.** That is a fourth cause for that code, and it
    // is NOT the operator-actionable "this job outlived its lease" timing signal --
    // nothing here says the lease bound is too short. Whatever splits those causes
    // must not fold this one in with them
    // ([#1074](https://github.com/LASTRADA-Software/fastcached/issues/1074)).
    auto const reclaimed = _leases.ReleaseWorker(workerId);

    _metrics.Increment(IMetricsSink::Counter::DispatchWorkersWithdrawn);
    if (reclaimed != 0)
        _metrics.Increment(IMetricsSink::Counter::DispatchLeasesReclaimed, static_cast<std::uint64_t>(reclaimed));

    return SchedulerReply::Success();
}

void SchedulerService::ReapExpiredWorkers()
{
    // Two locks, taken one after the other rather than one spanning both, and the
    // gap is deliberate: the registry and the lease table each guard their own, and
    // a lock covering the pair would put every reader of either behind the other.
    // What fits in the gap is a concurrent `Lease` that picked this worker just
    // before it was erased, whose lease is then held against a worker no later reap
    // can name. It is not stranded: the client that took it resolves it when its
    // job ends, and expiry is behind that -- which is exactly the pair of
    // guarantees every lease already has.
    auto const dropped = _workers.ExpireStale();
    if (dropped.empty())
        // The ordinary case on a live fleet, and this is a hot path -- every lease
        // request runs it.
        return;

    std::size_t reclaimed = 0;
    for (auto const& workerId: dropped)
        reclaimed += _leases.ReleaseWorker(workerId);

    _metrics.Increment(IMetricsSink::Counter::DispatchWorkersExpired, static_cast<std::uint64_t>(dropped.size()));
    if (reclaimed != 0)
        _metrics.Increment(IMetricsSink::Counter::DispatchLeasesReclaimed, static_cast<std::uint64_t>(reclaimed));
}

std::vector<UnservedToolchain> SchedulerService::UnservedToolchainsNow() const
{
    auto const live = _workers.LiveWorkers();
    auto unserved = _unserved.Recent();
    std::erase_if(unserved, [&live](UnservedToolchain const& toolchain) {
        return std::ranges::contains(live, toolchain.fingerprint, &WorkerInfo::fingerprint);
    });
    return unserved;
}

SchedulerReply SchedulerService::Lease(CallerContext const& caller, Wire::LeaseRequest const& request)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // Refused where it ENTERS, before anything below can keep a byte of it: a lease refused
    // `no-worker` is remembered with its fingerprint and label, and a granted one with its key.
    if (auto refusal = RefuseUnkept(_metrics, KeptValuesOf(request, LeaseFields), LeaseFieldRefusals); refusal.has_value())
        return std::move(*refusal);

    // Before the key is asked about, not after: a worker that vanished mid-job left
    // its leases behind, and duplicate suppression consults the key first -- so
    // every client that later missed on one of those keys was refused
    // `AlreadyInFlight` and compiled locally until the lease timed out. Losing one
    // machine quietly stopped distributing part of the build.
    ReapExpiredWorkers();

    // Duplicate suppression is asked BEFORE capacity, and the order is the
    // diagnostic. `Acquire` needs a worker id, so the code this was lifted from had
    // to pick first -- which meant a second client missing the same key at a busy
    // fleet was told `NoCapacity`. That reads as "buy more machines" when the truth
    // is "this build asked for the same object twice", and it lands hardest exactly
    // where duplicate suppression does the most good: a wide parallel build where
    // many translation units miss one key at once. Same refusal either way, so no
    // client behaviour changes; only what an operator is told about their fleet.
    if (_leases.IsInFlight(request.key))
        // Not a failure: duplicate-work suppression refusing the second of many
        // clients that missed the same key, each of which compiles locally.
        return Refuse(Wire::ErrorCode::AlreadyInFlight);

    auto const picked = _workers.Pick(request.fingerprint, request.excluded);
    if (!picked.has_value())
    {
        // Counted by the ROW rather than by `Refuse`'s code-keyed lookup: `Excluded`
        // reports the same code as `NoWorker` and must not move its counter, for the
        // reason `PickErrorTable`'s own comment gives.
        //
        // A table rather than a `switch`, so a fifth `PickError` is a build failure
        // here rather than a refusal that silently arrives as one of the other four.
        auto const& row = PickErrorTable[static_cast<std::size_t>(picked.error())];
        _metrics.Increment(row.counter);
        // Remembered before answering: the refusal reaches ONE client, and a toolchain nobody
        // serves refuses every client using it one at a time. The leader names it once.
        if (row.unservedToolchain)
            _unserved.Refused(request.fingerprint, request.toolchainLabel);
        return Refuse(row.reported, std::string { row.detail });
    }

    auto const lease = _leases.Acquire(request.key, picked->id, AgreedLeaseLifetime());
    if (!lease.has_value())
        // Still reachable, and the reason it must stay: `IsInFlight` above is
        // advisory, so two callers can both pass it and race here. `Acquire` is the
        // one that decides atomically, and the loser gets the same refusal it would
        // have got a few microseconds earlier.
        return Refuse(Wire::ErrorCode::AlreadyInFlight);

    // Accounted only once the lease exists. Counting at Pick would inflate the load
    // of a worker whose key turned out to be already in flight, and the correction
    // would not arrive until its next heartbeat.
    _workers.JobStarted(picked->id);
    _metrics.Increment(IMetricsSink::Counter::DispatchLeasesGranted);

    // Signed over the endpoint as well as the key, which is what makes it a grant on
    // ONE worker rather than on the fleet: without the endpoint inside the MAC, a
    // lease minted for this machine replays against every other machine that trusts
    // the same key.
    //
    // The PICKED worker's fingerprint, not the request's, though `Pick` matches the
    // two byte for byte and they are equal today. What the worker will compare the
    // claim against is its own registered fingerprint, so taking it from the same
    // record the endpoint comes from keeps both halves of the binding local to this
    // registry entry -- rather than resting on `Pick`'s comparison staying exact.
    auto const token = MintGrantToken(*lease, picked->endpoint, picked->fingerprint);

    // A hint BESIDE the name, never instead of it: the token above signs `picked->endpoint`,
    // so a hint that has gone stale onto another machine is refused `LeaseEndpointMismatch`
    // there and the client falls back to the name. Empty whenever `DecideDialHint` vetoes.
    auto const hint = DialHintFor(DialHintInputs { .advertised = picked->endpoint,
                                                   .observedHost = picked->observedHost,
                                                   .interfaceAddresses = picked->interfaceAddresses });

    // The worker's codecs travel with the grant so the client can choose one for the
    // preprocessed payload it is about to send -- without a negotiation round trip,
    // and without guessing at something the worker cannot decode after the whole
    // payload has already crossed the network.
    return SchedulerReply::Success(
        Wire::EncodeLeaseGrant(Wire::LeaseGrant { .endpoint = picked->endpoint,
                                                  .leaseToken = token,
                                                  .workerCodecs = picked->codecs,
                                                  // From the LEASE, like the token's own expiry, so the client's
                                                  // bound and the grant's cannot be two readings of one setting
                                                  // taken a moment apart.
                                                  .lifetime = lease->lifetime,
                                                  .dialHint = hint }));
}

SchedulerReply SchedulerService::Release(CallerContext const& caller, std::string_view leaseToken, std::string_view key)
{
    // A settlement, not a scheduling decision -- so leadership does not apply and
    // membership still does. Before this, a node demoted between granting a lease and
    // being handed it back refused its own lease's release with `NotLeader`, and the
    // key stayed pinned on the ONE machine that could have freed it until it expired
    // (#371). The client was doing exactly the right thing: the rulebook requires a
    // release to go to whoever ISSUED the lease, and this is where that arrived.
    if (auto refusal = Gate(caller, GateScope::Settlement); refusal.has_value())
        return std::move(*refusal);

    // The token the client hands back is the SIGNED grant, so the serial the lease
    // table knows has to be unwrapped out of it -- which is also the point at which a
    // release that was never granted stops being able to resolve anything. Verified
    // rather than merely parsed: it costs one signature check on a verb that already
    // crossed the network, and a forged release frees a key somebody else is building.
    //
    // Against THIS scheduler's own key and nothing else (#178). A release goes to whoever
    // issued the grant -- the rulebook's rule, and `Dispatch`'s behaviour -- so "a grant I
    // signed" is the whole question, and it needs no roster: a grant another member signed
    // names a serial this table never issued.
    std::string serial { leaseToken };
    {
        auto authentic = AuthenticateLeaseToken(OwnSignature { _signer }, leaseToken);
        if (!authentic.has_value())
            return Refuse(Wire::ErrorCode::LeaseUnauthorized);

        // The cluster, because a release names a SERIAL and this fleet issues serials
        // from the same space another fleet sharing the key does: an authentic token
        // from cluster A would otherwise release whatever cluster B's table happens to
        // hold under that number (#322).
        if (authentic->clusterId != _clusterId)
            return Refuse(Wire::ErrorCode::LeaseUnauthorized);

        // The KEY the token itself names, rather than only the one the caller states
        // (#323). Until this, the credential was read on the compile path and
        // discarded here: the token said what it authorised, and the release path
        // trusted the caller's word and left `LeaseTable` to notice.
        //
        // That worked, and worked BY COINCIDENCE of another component's strictness --
        // `LeaseTable` holds the authoritative mapping, so a wrong key finds no live
        // lease. Any change making that lookup more permissive (a normalisation, a
        // fallback, an index by worker rather than by key) would silently remove the
        // only check that the released lease is the one the token names, with nothing
        // here to fail. A credential that is honoured on one verb and ignored on
        // another is trusted by accident of which verb ran.
        if (authentic->key != key)
            return Refuse(Wire::ErrorCode::LeaseUnauthorized);

        // And deliberately NOT the epoch. A release under a term other than the
        // current one is what a compile that outlived an election looks like, which is
        // ordinary and is the case `Release` exists for -- refusing it would leave the
        // key marked in flight until the lease expired, which is #212 exactly. The
        // epoch guards SPENDING a grant, never tidying one up.
        serial = std::move(authentic->serial);
    }

    auto const lease = _leases.Release(serial, key);
    if (!lease.has_value())
    {
        // Already gone, and WHICH of the three ways decides whether anything rises.
        // This used to be one refusal for all three, uncounted on the grounds that it
        // is a statement about one client's timing rather than about the fleet's
        // capacity -- right about capacity, and beside the point about fit. Fit is
        // what nothing else can observe: a job that outlived its lease is the only
        // evidence a site has that `DefaultCompileLeaseTimeout` is shorter than its
        // slowest translation unit, and this file's own `LeaseTable` names it as the
        // one condition worth telling an operator about (#1074).
        //
        // The counter travels on the ROW, and `RowsInEnumeratorOrder` is what makes
        // it unforgettable rather than the shape of this call: a fourth way of
        // resolving nothing cannot be added without a row, and a row cannot be
        // written without deciding whether anything rises and saying why. That is
        // the same guarantee `RefusalTable` gets from being consulted by `Refuse`,
        // reached differently -- there is exactly one call site here, so the
        // decision lives in the table rather than in what this line remembers.
        auto const& row = ReleaseRefusalTable[static_cast<std::size_t>(lease.error())];
        if (row.counter.has_value())
            _metrics.Increment(*row.counter);
        return Refuse(row.code, std::string { row.detail });
    }

    // The registry's speculative count, undone. `JobStarted` at `Lease` above is what
    // stops the scheduler over-assigning a worker inside one heartbeat window, and
    // without this it only ever climbed -- corrected solely by the next heartbeat
    // overwrite, so a burst of leases took a worker out of rotation until it landed.
    _workers.JobFinished(lease->workerId);
    _metrics.Increment(IMetricsSink::Counter::DispatchLeasesReleased);
    return SchedulerReply::Success();
}

} // namespace FastCache::Distributed
