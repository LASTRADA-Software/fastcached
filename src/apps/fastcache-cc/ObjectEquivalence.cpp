// SPDX-License-Identifier: Apache-2.0
#include "ObjectEquivalence.hpp"
#include "ObjectSections.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

namespace
{
    /// The `TimeDateStamp` field is four bytes wide in both COFF layouts.
    constexpr std::size_t TimeDateStampWidth = 4;

    /// How many differing offsets to look at when describing a difference.
    ///
    /// Enough to name every section involved in the cases measured (twelve bytes is
    /// the worst of them) with room to spare, and bounded so that two entirely
    /// unrelated objects do not walk a megabyte to say "these differ".
    constexpr std::size_t DescribeBudget = 4096;

    /// Where two images differ, and whether that list is the whole story.
    struct DifferenceMap
    {
        /// The differing offsets, in order.
        std::vector<std::size_t> offsets;
        /// True when the scan REACHED ITS BUDGET with differences still to find, so
        /// `offsets` is a PREFIX.
        ///
        /// **"Reached its budget", never "stopped before the end of the file."** The
        /// two coincide on the failing case and differ on every passing one, which is
        /// why the second spelling passed the case written for it and was wrong
        /// everywhere else: the scan stops one byte past the LAST difference, which is
        /// almost never end-of-file, so `at < left.size()` called every ordinary
        /// comparison truncated and silently disabled the classification this flag
        /// exists to protect.
        ///
        /// Carried rather than inferred from `offsets.size() == DescribeBudget`,
        /// because two conclusions below are only sound over a COMPLETE list: that a
        /// difference is confined to the driver's path records (so the code is
        /// identical), and that a count is a total. Both are stated to an operator as
        /// facts, and a prefix would let each of them be confidently wrong.
        bool truncated { false };
    };

    /// The offsets at which @p left and @p right differ, up to `DescribeBudget`.
    ///
    /// Driven by `std::mismatch` rather than a byte loop, because the equal RUNS are
    /// what dominate: the ordinary Windows hit differs in four bytes of a file that
    /// may be megabytes, and a per-byte compare would walk all of it scalar. This
    /// skips each equal run at `memcmp` speed and stops at the budget.
    ///
    /// @param left One image.
    /// @param right The other, of the same length.
    /// @return The differing offsets, in order, and whether the budget cut them short.
    [[nodiscard]] DifferenceMap DifferingOffsets(std::span<std::byte const> left, std::span<std::byte const> right)
    {
        std::vector<std::size_t> offsets;
        std::size_t at = 0;
        auto truncated = false;
        while (at < left.size())
        {
            auto const tail = left.subspan(at);
            auto const [l, r] = std::ranges::mismatch(tail, right.subspan(at));
            // No further difference: the list is COMPLETE, however far through the
            // images this stopped. Deriving truncation from `at < left.size()` instead
            // called every ordinary comparison truncated -- the scan stops one byte
            // past the last difference, which is almost never the end of the file --
            // and that suppressed the classification these flags exist to protect.
            if (l == tail.end())
                break;
            if (offsets.size() == DescribeBudget)
            {
                truncated = true;
                break;
            }
            at += static_cast<std::size_t>(std::ranges::distance(tail.begin(), l));
            offsets.push_back(at);
            ++at;
        }
        return { .offsets = std::move(offsets), .truncated = truncated };
    }

    /// Sections `cl` fills with a record of WHERE it compiled rather than WHAT.
    ///
    /// **Not an excuse list.** Nothing here is overlooked; a difference confined to
    /// these still answers `Different`, because a hit whose object was built in
    /// another checkout is exactly
    /// [#489](https://github.com/LASTRADA-Software/fastcached/issues/489) and is the
    /// case an operator runs the verifier to catch. It is a list of names that lets
    /// the MESSAGE say which finding this is -- a foreign build path, or different
    /// code -- because those are acted on differently and "wrong object" alone means
    /// neither.
    constexpr std::array<std::string_view, 2> PathRecordSections { ".debug$S", ".chks64" };

    /// Which sections a set of differing offsets falls in.
    struct TouchedSections
    {
        /// The section names touched, in file order and without repeats.
        std::vector<std::string> names;
        /// Whether at least one offset fell in no section at all -- the header, the
        /// symbol table, or padding between sections.
        bool anyOutside { false };
    };

    /// Name the sections @p offsets fall in.
    /// @param sections The section spans of the image.
    /// @param offsets Where the images differ.
    /// @return The names touched, and whether anything fell outside them all.
    [[nodiscard]] TouchedSections SectionsTouchedBy(std::vector<ObjectSection> const& sections,
                                                    std::vector<std::size_t> const& offsets)
    {
        TouchedSections touched;
        for (auto const offset: offsets)
        {
            auto const hit = std::ranges::find_if(sections, [offset](ObjectSection const& section) {
                return offset >= section.at && offset < section.at + section.size;
            });
            if (hit == sections.end())
            {
                touched.anyOutside = true;
                continue;
            }
            if (std::ranges::find(touched.names, hit->name) == touched.names.end())
                touched.names.push_back(hit->name);
        }
        return touched;
    }

    /// Render @p names as `a, b and c`.
    /// @param names What to join; never empty.
    /// @return The rendered list.
    [[nodiscard]] std::string JoinNames(std::vector<std::string> const& names)
    {
        std::string out;
        for (auto const index: std::views::iota(std::size_t { 0 }, names.size()))
        {
            if (index != 0)
                out += index + 1 == names.size() ? " and " : ", ";
            out += names[index];
        }
        return out;
    }

    /// Say where two images of the same length differ, as well as can be told.
    ///
    /// @param layout The layout both were read by, or null when neither is COFF.
    /// @param served The cache's bytes.
    /// @param fresh The compiler's bytes.
    /// @param offsets Where they differ; never empty.
    /// @return A sentence naming the difference.
    [[nodiscard]] std::string DescribeDifference(CoffLayout const* layout,
                                                 std::span<std::byte const> served,
                                                 std::span<std::byte const> fresh,
                                                 DifferenceMap const& difference)
    {
        auto const& offsets = difference.offsets;
        // Not `const`: it is returned by value on three paths, and constness would
        // turn each of those into a copy.
        //
        // "at least" when the scan was cut short. A capped count rendered as a total
        // is a precise-looking wrong number, in the one message this feature exists
        // to produce.
        auto positions = std::format("{}{} differing byte(s), first at offset {}",
                                     difference.truncated ? "at least " : "",
                                     offsets.size(),
                                     offsets.front());
        if (layout == nullptr)
            return positions;

        // The served image's own section COUNT, only when it disagrees: a section that
        // appeared or vanished is a different shape of wrongness from a section whose
        // contents changed, and the two are fixed in different places. A count rather
        // than a walk, because a count is all this asks -- and on a `/Gy` object a walk
        // is one heap-allocated name per function.
        // Spelled as a statement rather than as `cond ? SectionCount(...) : std::nullopt`.
        // GCC 14 at -O3 inlines that ternary far enough to report the disengaged arm as
        // `-Werror=maybe-uninitialized` inside `<optional>`'s own `operator!=`, and clang
        // emits nothing at any level -- the second-compiler class this project's gate
        // exists for. Fixed at the source, since the rule here is never to silence.
        std::optional<std::uint64_t> servedCount;
        if (IsConsistentCoff(*layout, served))
            servedCount = CoffSectionCount(*layout, served);
        auto const freshCount = CoffSectionCount(*layout, fresh);
        if (servedCount != freshCount)
            return std::format("the cached object has {} section(s) where this compile produces {}",
                               servedCount.has_value() ? std::format("{}", *servedCount) : "an unreadable number of",
                               freshCount.has_value() ? std::format("{}", *freshCount) : "an unreadable number");

        auto const sections = CoffSections(*layout, fresh);
        if (sections.empty())
            return positions;

        auto const touched = SectionsTouchedBy(sections, offsets);
        if (touched.names.empty())
            return std::format("{}, outside every section (the header or the symbol table)", positions);

        // A TRUNCATED list cannot support this conclusion, because the conclusion is a
        // claim about bytes nobody looked at. `.debug$S` precedes `.text$mn` in a `cl`
        // object and carries the whole of `/Z7`'s debug info, which a launcher-active
        // build forces -- so a genuinely stale object can differ there by more than the
        // budget and have its `.text$mn` difference fall off the end. The operator
        // would then be told the code is identical and sent to hunt for a checkout that
        // is not the problem. Refused rather than hedged: this sentence is only worth
        // printing when it is certain.
        auto const onlyPathRecords =
            !difference.truncated && !touched.anyOutside && std::ranges::all_of(touched.names, [](auto const& name) {
                return std::ranges::find(PathRecordSections, name) != PathRecordSections.end();
            });
        if (onlyPathRecords)
            return std::format("{} -- which record WHERE the compile happened, not what it compiled. The cached "
                               "object was built at a different path, in another checkout or on another machine "
                               "(its code and data are identical)",
                               JoinNames(touched.names));

        return std::format("{}, in {}", positions, JoinNames(touched.names));
    }
} // namespace

ObjectComparisonResult CompareObjectImages(std::span<std::byte const> served, std::span<std::byte const> fresh)
{
    if (served.size() == fresh.size() && (served.empty() || std::memcmp(served.data(), fresh.data(), served.size()) == 0))
        return { .outcome = ObjectComparison::Identical, .detail = {} };

    // The FRESH image decides which layout applies, and only the fresh one: it is
    // this toolchain's own output, so a format this cannot lay out is this code's
    // blind spot. Deciding from the served image instead would let a damaged or
    // foreign object talk the verifier out of answering, which is the same silence
    // by another route.
    // Before any layout question, because it does not need one: two files of
    // different lengths are not the same compile whatever format they are in, and
    // answering `Unsupported` here would throw away unambiguous evidence of a wrong
    // object -- a truncated cache entry on a toolchain this build cannot lay out
    // would then be reported as "cannot verify" rather than as the finding it is.
    if (served.size() != fresh.size())
        return { .outcome = ObjectComparison::Different,
                 .detail = std::format(
                     "the cached object is {} bytes where this compile produces {}", served.size(), fresh.size()) };

    auto const* layout = ChooseCoffLayout(fresh);
    if (layout == nullptr && HasBigObjSignature(fresh))
        return { .outcome = ObjectComparison::Unsupported,
                 .detail = "this compiler writes an MSVC object format this build cannot lay out, so the clock it "
                           "stamps into every object cannot be told apart from a real difference" };

    auto difference = DifferingOffsets(served, fresh);

    // Drop the one normalised region, and only it. A difference reaching past it is a
    // finding -- see PathRecordSections for why that includes the path records.
    //
    // Dropped BEFORE the description as well as before the decision, because the
    // clock lies in the header rather than in any section: left in, it made every
    // difference look as though it reached outside the sections, which suppressed the
    // classification that tells a foreign build path from stale code.
    if (layout != nullptr)
    {
        auto const stampAt = layout->timeDateStampAt;
        auto const removed = std::ranges::remove_if(difference.offsets, [stampAt](std::size_t offset) {
            return offset >= stampAt && offset < stampAt + TimeDateStampWidth;
        });
        difference.offsets.erase(removed.begin(), removed.end());
    }

    if (difference.offsets.empty())
        return { .outcome = ObjectComparison::EquivalentApartFromVolatile,
                 .detail = "the compiler's timestamp, which every MSVC-family driver stamps into the object header" };

    return { .outcome = ObjectComparison::Different, .detail = DescribeDifference(layout, served, fresh, difference) };
}

} // namespace FastCache::Cc
