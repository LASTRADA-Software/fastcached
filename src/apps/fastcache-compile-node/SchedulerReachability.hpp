// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConditions.hpp"
#include "NodeProofClient.hpp"
#include "SchedulerLink.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

/// How often a setback that LASTS is said above Debug, and how often one scheduler may be said at
/// Warn at all.
///
/// The initializer of the `cadence` column of `ReachabilityTable`, not a second copy of it: the rows
/// are what `SchedulerReachability` consults. Its one other reader is the forgetting bound -- a
/// record nothing touched for a whole cadence -- which is the same interval by meaning, not by
/// coincidence: a reminder is a claim about a place that is still being asked.
///
/// Ten minutes turns an hour of a dead scheduler from 180 Warn lines per announce loop into one
/// Warn, five Info reminders and the recovery, per machine.
inline constexpr std::chrono::seconds SchedulerUnreachableCadence { std::chrono::minutes { 10 } };

/// How far one announce round got at one scheduler endpoint.
///
/// PRIVATE: never transmitted and never persisted. **Ordered on purpose**: a success at a stage ends
/// every setback of that stage and of the stages before it, and none after -- a dial that connects
/// says nothing about a proof the scheduler goes on refusing.
enum class AnnounceStage : std::uint8_t
{
    Dial,         ///< A connection opened.
    Proof,        ///< This machine proved itself on it.
    Announcement, ///< What the loop says was accepted.
};

/// What stopped a round at one scheduler endpoint.
///
/// PRIVATE: never transmitted and never persisted -- the words travel, never the enumerator -- so
/// its enumerators carry no explicit values.
enum class SchedulerOutcome : std::uint8_t
{
    Unreachable,         ///< The dial did not connect.
    NoHandshake,         ///< It serves no identity handshake, so it is no scheduler of this fleet.
    Untrusted,           ///< This machine will not prove itself to it.
    IdentityRefused,     ///< It did not accept this machine's identity.
    IdentityDeferred,    ///< It could not judge this machine's identity yet.
    RegistrationRefused, ///< It did not register one of this worker's toolchains.
    PresenceRefused,     ///< It did not record this machine.
    Last                 ///< Not an outcome.
};

/// One outcome: where in a round it happens, and how it is said.
struct SchedulerOutcomeRow
{
    SchedulerOutcome outcome {}; ///< The enumerator this row describes.
    AnnounceStage stage {};      ///< Where in a round it happens.
    /// The proof result it stands for, when it is one; how `SchedulerOutcomeOfProof` finds it.
    std::optional<NodeProofResult> proof {};
    /// What went wrong. `{0}` the scheduler, `{1}` the subject, `{2}` its reason, `{3}` whole seconds.
    /// The words from before this table existed, unchanged: the docs and operators read them.
    std::string_view failure {};
    /// What went right again, with the same arguments.
    std::string_view recovery {};
    /// The condition this outcome holds raised while any place suffers it; nullopt for one that
    /// raises none. A column, so the next condition an outcome should raise is a cell, not an `if`.
    std::optional<NodeCondition> raises {};
};

/// Every outcome a round can meet at one scheduler. A new kind of refusal is a row here.
inline constexpr auto SchedulerOutcomeTable = EnumTable<SchedulerOutcome, SchedulerOutcomeRow> {
    SchedulerOutcomeRow { .outcome = SchedulerOutcome::Unreachable,
                          .stage = AnnounceStage::Dial,
                          .proof = std::nullopt,
                          .failure = "scheduler {0} unreachable",
                          .recovery = "scheduler {0} reachable again after {3}s unreachable",
                          .raises = NodeCondition::SchedulerUnreachable },
    SchedulerOutcomeRow { .outcome = SchedulerOutcome::NoHandshake,
                          .stage = AnnounceStage::Proof,
                          .proof = NodeProofResult::NotOffered,
                          .failure = "{0} serves no identity handshake, so it is no scheduler of this fleet: {2}",
                          .recovery = "{0} accepted this machine's identity again after {3}s" },
    SchedulerOutcomeRow { .outcome = SchedulerOutcome::Untrusted,
                          .stage = AnnounceStage::Proof,
                          .proof = NodeProofResult::Untrusted,
                          .failure = "this machine will not prove itself to {0}: {2}",
                          .recovery = "{0} accepted this machine's identity again after {3}s" },
    SchedulerOutcomeRow { .outcome = SchedulerOutcome::IdentityRefused,
                          .stage = AnnounceStage::Proof,
                          .proof = NodeProofResult::Refused,
                          .failure = "{0} did not accept this machine's identity: {2}",
                          .recovery = "{0} accepted this machine's identity again after {3}s" },
    SchedulerOutcomeRow { .outcome = SchedulerOutcome::IdentityDeferred,
                          .stage = AnnounceStage::Proof,
                          .proof = NodeProofResult::Deferred,
                          .failure = "{0} cannot judge this machine's identity yet, asking again shortly: {2}",
                          .recovery = "{0} accepted this machine's identity after {3}s" },
    SchedulerOutcomeRow { .outcome = SchedulerOutcome::RegistrationRefused,
                          .stage = AnnounceStage::Announcement,
                          .proof = std::nullopt,
                          .failure = "scheduler {0} did not register {1}: {2}",
                          .recovery = "scheduler {0} registered {1} again after {3}s" },
    SchedulerOutcomeRow { .outcome = SchedulerOutcome::PresenceRefused,
                          .stage = AnnounceStage::Announcement,
                          .proof = std::nullopt,
                          .failure = "scheduler {0} did not record this machine {2}",
                          .recovery = "scheduler {0} recorded this machine again after {3}s" },
};
static_assert(RowsInEnumeratorOrder(SchedulerOutcomeTable, &SchedulerOutcomeRow::outcome));

/// What one failure or success means for the log.
///
/// PRIVATE, for `SchedulerOutcome`'s reason.
enum class ReachabilityEvent : std::uint8_t
{
    Lost,     ///< A new setback, at a scheduler with no Warn inside the cadence.
    Further,  ///< A new setback -- another outcome, or another toolchain -- at a scheduler already warned about.
    Relapse,  ///< The same setback at the same place again, inside a cadence of its last opening.
    Reminder, ///< The setback lasts, a whole cadence after its last line above Debug.
    Repeat,   ///< The setback lasts, with nothing owed above Debug.
    Regained, ///< The setback ended.
    Last      ///< Not an event.
};

/// How one event is said.
struct ReachabilityRow
{
    ReachabilityEvent event {}; ///< The enumerator this row describes.
    /// How loudly; nullopt for `Regained`, which is said at the loudest level anything said about
    /// its outage.
    std::optional<LogLevel> level {};
    /// For `Lost`, the least time between two Warns for one scheduler. For `Relapse`, how long
    /// after an opening the same setback at the same place counts as the same trouble. For
    /// `Reminder`, the least time since the outage's last line above Debug. Nullopt elsewhere.
    std::optional<std::chrono::seconds> cadence {};
    /// Which of the outcome's sentences this event says.
    std::string_view SchedulerOutcomeRow::* sentence {};
    /// The frame: `{0}` the outcome's sentence, `{1}` whole seconds, `{2}` `"; trying <endpoint>"` or empty.
    std::string_view pattern {};
};

/// Every event and how it is said. A new event is a row here and an arm in nothing.
inline constexpr auto ReachabilityTable = EnumTable<ReachabilityEvent, ReachabilityRow> {
    ReachabilityRow { .event = ReachabilityEvent::Lost,
                      .level = LogLevel::Warn,
                      .cadence = SchedulerUnreachableCadence,
                      .sentence = &SchedulerOutcomeRow::failure,
                      .pattern = "{0}{2}" },
    ReachabilityRow { .event = ReachabilityEvent::Further,
                      .level = LogLevel::Info,
                      .cadence = std::nullopt,
                      .sentence = &SchedulerOutcomeRow::failure,
                      .pattern = "{0}{2}" },
    ReachabilityRow { .event = ReachabilityEvent::Relapse,
                      .level = LogLevel::Debug,
                      .cadence = SchedulerUnreachableCadence,
                      .sentence = &SchedulerOutcomeRow::failure,
                      .pattern = "{0} (again){2}" },
    ReachabilityRow { .event = ReachabilityEvent::Reminder,
                      .level = LogLevel::Info,
                      .cadence = SchedulerUnreachableCadence,
                      .sentence = &SchedulerOutcomeRow::failure,
                      .pattern = "{0} -- still, after {1}s{2}" },
    ReachabilityRow { .event = ReachabilityEvent::Repeat,
                      .level = LogLevel::Debug,
                      .cadence = std::nullopt,
                      .sentence = &SchedulerOutcomeRow::failure,
                      .pattern = "{0} (for {1}s){2}" },
    ReachabilityRow { .event = ReachabilityEvent::Regained,
                      .level = std::nullopt,
                      .cadence = std::nullopt,
                      .sentence = &SchedulerOutcomeRow::recovery,
                      .pattern = "{0}" },
};
static_assert(RowsInEnumeratorOrder(ReachabilityTable, &ReachabilityRow::event));

/// @param outcome Any enumerator but `Last`.
/// @return Its row.
[[nodiscard]] constexpr SchedulerOutcomeRow const& SchedulerOutcomeRowOf(SchedulerOutcome outcome)
{
    return SchedulerOutcomeTable.at(static_cast<std::size_t>(outcome));
}

/// @param event Any enumerator but `Last`.
/// @return Its row.
[[nodiscard]] constexpr ReachabilityRow const& ReachabilityRowOf(ReachabilityEvent event)
{
    return ReachabilityTable.at(static_cast<std::size_t>(event));
}

/// The outcome an unproved identity is, read off the table's `proof` column.
/// @param result Anything but `Proved`, which is no setback; `Proved` answers `IdentityRefused`, the
///        catch-all the sentence this replaced used for everything it did not name.
/// @return The outcome.
[[nodiscard]] SchedulerOutcome SchedulerOutcomeOfProof(NodeProofResult result);

/// One failure at one scheduler, as the round met it.
struct SchedulerFailure
{
    std::string_view endpoint;          ///< The scheduler.
    std::string_view subject {};        ///< A toolchain fingerprint for a registration; otherwise empty.
    std::string_view reason {};         ///< What it said, where it said anything.
    std::optional<std::string> next {}; ///< Where the round goes next, when it goes on.
};

/// Say one event of one outcome, through both rows.
///
/// A pure function, so every pair can be walked by a test: the patterns are runtime format strings.
/// @param event What happened.
/// @param outcome Which setback it happened to.
/// @param failure Where, and what it said.
/// @param lasted How long the setback has lasted; whole seconds are said.
/// @param inherited The level a row with none says at: the loudest line said about this outage.
/// @return The level and the sentence.
[[nodiscard]] RoundReport DescribeSetback(ReachabilityEvent event,
                                          SchedulerOutcome outcome,
                                          SchedulerFailure const& failure,
                                          core::platform::SteadyDuration lasted,
                                          LogLevel inherited);

/// How loudly a scheduler that does not answer, or refuses, is said -- across rounds and across loops.
///
/// ## Why this exists
///
/// Both announce loops -- the worker's heartbeat and the machine's presence -- logged every
/// unreachable dial, every unproved identity and every refused registration or presence at Warn,
/// on EVERY round: one line per loop per `NodeAnnounceInterval`, about 360 an hour on a machine
/// running both. A log that says the same thing 360 times an hour is one nobody reads for the line
/// that changed.
///
/// ## The rule, keyed on the round's OUTCOME
///
/// A setback lives at a place, `(scheduler, subject)`, and carries the outcome that made it. A
/// failure at a place with no setback, or with one of a DIFFERENT outcome, is a transition.
///
/// - **At most one Warn per scheduler per cadence, and the budget counts LOSSES.** A transition is
///   said at Warn (`Lost`), unless that scheduler had a Warn inside the cadence (`Further`, Info), or
///   the same outcome at the same place was already opened above Debug inside it (`Relapse`, Debug).
///   Otherwise a flapping VPN link is 90 Warns an hour through a different door.
/// - **While a setback lasts:** a reminder at Info once per cadence (`Reminder`), Debug otherwise
///   (`Repeat`).
/// - **The recovery is said at the level of the loudest line said about that setback** (`Regained`):
///   a Warn loss is answered at Warn, a dampened loss quietly. An operator never told something went
///   away is not told it came back; one who was told is.
/// - **A success at a stage ends a setback of that stage or an earlier one** (`AnnounceStage`).
///
/// ## Only what is still being asked
///
/// A setback, a Warn record or an opening record nothing touched for a whole cadence is forgotten,
/// silently. `SchedulerLink` lets go of a remembered leader that stopped answering, and a machine
/// that slept dialled nothing: about either, "still" and "again" are claims this process can no
/// longer make. A failure after that is news again, at Warn.
///
/// ## ONE per process
///
/// `main` owns one and lends it to both loops, so a machine says each transition once rather than
/// once per loop. Two threads therefore call in, hence the mutex. The clock is injected, as for
/// every other time-dependent answer here.
class SchedulerReachability
{
  public:
    /// @param clock What times setbacks and cadences. Injected so a test can drive an hour.
    /// @param conditions Where the conditions an outcome raises are answered; null where nothing
    ///        reports them. When given, every such row is answered at once -- `clear` -- so a wired
    ///        node never shows `undecided` for it.
    explicit SchedulerReachability(core::platform::IClock const& clock, NodeConditions* conditions = nullptr);

    SchedulerReachability(SchedulerReachability const&) = delete;
    SchedulerReachability(SchedulerReachability&&) = delete;
    SchedulerReachability& operator=(SchedulerReachability const&) = delete;
    SchedulerReachability& operator=(SchedulerReachability&&) = delete;
    ~SchedulerReachability() = default;

    /// A round met @p outcome at @p failure's place.
    /// @param outcome What stopped it.
    /// @param failure Where, what it said, and where the round goes next.
    /// @return What to log, and how loudly; always something, if only at Debug.
    [[nodiscard]] RoundReport Failed(SchedulerOutcome outcome, SchedulerFailure const& failure);

    /// A round got through @p stage at a place.
    /// @param stage How far it got.
    /// @param endpoint The scheduler.
    /// @param subject A toolchain fingerprint for a registration; otherwise empty.
    /// @return What to log when that ends a setback; nothing when there was none to end.
    [[nodiscard]] std::optional<RoundReport> Succeeded(AnnounceStage stage,
                                                       std::string_view endpoint,
                                                       std::string_view subject = {});

  private:
    /// Where a setback lives: the scheduler, and the subject at it.
    using Place = std::pair<std::string, std::string>;
    /// One setback's opening record: a place and an outcome.
    using Opening = std::tuple<std::string, std::string, SchedulerOutcome>;

    /// One place that is failing.
    struct Setback
    {
        SchedulerOutcome outcome {};                ///< What is failing there.
        core::platform::SteadyTimePoint since {};   ///< The first failure of this setback.
        core::platform::SteadyTimePoint said {};    ///< The last line above Debug, or `since`.
        core::platform::SteadyTimePoint touched {}; ///< The last failure.
        LogLevel loudest { LogLevel::Debug };       ///< The loudest line said about it.
    };

    /// Which event opening @p outcome at @p failure's place is, recording what it spends.
    /// `_mutex` held.
    [[nodiscard]] ReachabilityEvent Open(SchedulerOutcome outcome,
                                         SchedulerFailure const& failure,
                                         core::platform::SteadyTimePoint now);

    /// Drop every record nothing touched for a whole cadence. `_mutex` held.
    void Forget(core::platform::SteadyTimePoint now);

    /// Answer every condition an outcome raises, from the setbacks now held. `_mutex` held.
    void Evaluate();

    core::platform::IClock const& _clock;
    NodeConditions* _conditions;
    std::mutex _mutex;
    std::map<Place, Setback> _setbacks;                                          ///< Places failing now.
    std::map<std::string, core::platform::SteadyTimePoint, std::less<>> _warned; ///< Last Warn per scheduler.
    std::map<Opening, core::platform::SteadyTimePoint> _opened;                  ///< Last opening above Debug.
};

} // namespace FastCache::Node
