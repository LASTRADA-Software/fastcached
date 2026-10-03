// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

/// @file EnrollAutoApprove.hpp
/// Which auto-approve durations a leader accepts, as one table both ends read.
///
/// **Two refusal sites, one table.** The CLI refuses a bad duration before sending it, because
/// the operator is watching; the leader refuses it again, because a peer may send anything.
/// Both read `AutoApproveRefusals`, so each rule has exactly one sentence. A header of its own
/// so the option table can reach it without the enrollment list it belongs to.
namespace FastCache::Node
{

/// The longest an auto-approve window may be armed for.
///
/// A day, because the window admits anybody who asks under whatever key they ask with, and the
/// audit question afterwards -- *who got in while nobody was looking* -- gets harder to answer
/// the longer nobody looked. A rollout longer than that re-arms before the deadline, which is an
/// operator saying again that they mean it.
inline constexpr std::chrono::hours AutoApproveCeiling { 24 };

/// Why an auto-approve duration is refused, by name.
///
/// **Private**: never transmitted or persisted. The wire carries the duration and the leader's
/// refusal carries the sentence.
enum class AutoApproveRefusal : std::uint8_t
{
    Zero,        ///< A window of no length, which admits nobody and reads as though it did.
    OverCeiling, ///< Longer than `AutoApproveCeiling`.
    Last,        ///< Not a refusal: the length of a table keyed by one.
};

/// One refusal, and the sentence an operator reads for it.
struct AutoApproveRefusalRow
{
    AutoApproveRefusal refusal; ///< Which rule.

    /// What the operator is told, at the CLI and from the leader alike. A `{}` is the ceiling in
    /// hours, filled from `AutoApproveCeiling` by `AutoApproveSentence` -- never spelled here, or
    /// changing the constant would leave the refusal naming the old value.
    std::string_view sentence;
};

/// One row per `AutoApproveRefusal`, in enumerator order.
inline constexpr EnumTable<AutoApproveRefusal, AutoApproveRefusalRow> AutoApproveRefusals { {
    { .refusal = AutoApproveRefusal::Zero,
      .sentence = "an auto-approve window of zero admits nobody; --enroll-auto-approve=off ends one" },
    { .refusal = AutoApproveRefusal::OverCeiling,
      .sentence = "an auto-approve window is at most {}h; re-arm it before it ends to extend it" },
} };

static_assert(RowsInEnumeratorOrder(AutoApproveRefusals, &AutoApproveRefusalRow::refusal),
              "AutoApproveRefusals must hold one row per AutoApproveRefusal, in enumerator order");

/// The sentence an operator reads for @p refusal, with the ceiling it names taken from
/// `AutoApproveCeiling`.
/// @param refusal Which rule refused.
/// @return Its row's sentence, filled in.
[[nodiscard]] inline std::string AutoApproveSentence(AutoApproveRefusal refusal)
{
    auto const ceilingHours = AutoApproveCeiling.count();
    return std::vformat(AutoApproveRefusals[static_cast<std::size_t>(refusal)].sentence,
                        std::make_format_args(ceilingHours));
}

/// Which rule, if any, refuses @p duration.
/// @param duration What an operator asked to arm.
/// @return The refusal, or nothing when the duration may be armed.
[[nodiscard]] constexpr std::optional<AutoApproveRefusal> JudgeAutoApprove(std::chrono::seconds duration) noexcept
{
    if (duration <= std::chrono::seconds::zero())
        return AutoApproveRefusal::Zero;
    if (duration > AutoApproveCeiling)
        return AutoApproveRefusal::OverCeiling;
    return std::nullopt;
}

} // namespace FastCache::Node
