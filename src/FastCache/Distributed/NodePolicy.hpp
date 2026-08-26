// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace FastCache::Distributed
{

/// How hard a machine may be driven.
///
/// Zero is `Workstation`, and that is the safety property rather than an ordering
/// convenience: a node whose class nobody set is somebody's desktop until proven
/// otherwise. Getting this backwards means a developer's machine is saturated by
/// other people's builds because an operator forgot a flag — a failure they
/// experience as "my editor stutters" and never connect to a build fleet.
enum class NodeClass : std::uint8_t
{
    /// Somebody works at this machine. Capacity is left free for them.
    Workstation = 0,
    /// Nobody works at this machine. It may be driven to its slot limit.
    Dedicated,
    Last, ///< Not a class, and has no row: `NodeClassTable`'s length.
};

/// One row per class: what it is called, and how much of the machine is not ours.
///
/// A table rather than an `if`, because a third class is foreseeable — a laptop on
/// battery, a CI runner that may be driven hard but not swapped — and each would
/// otherwise be another branch threaded through the scheduler.
struct NodeClassTraits
{
    NodeClass nodeClass;         ///< The class this row describes.
    std::string_view name;       ///< Spelling accepted on the command line.
    std::uint32_t reservedCores; ///< Cores never offered to the fleet.
};

/// Every class, in enumerator order.
///
/// The workstation reserve is **2** rather than 1. One core free is enough for a
/// scheduler to keep running and not enough for a person: a modern editor, its
/// language server and a browser will each want one, and the whole point of the
/// reserve is that the machine stays usable while it contributes. It is a default
/// rather than a rule -- `--reserve-cores` overrides it -- but a default nobody
/// changes is the one most people run.
inline constexpr EnumTable<NodeClass, NodeClassTraits> NodeClassTable { {
    { .nodeClass = NodeClass::Workstation, .name = "workstation", .reservedCores = 2 },
    { .nodeClass = NodeClass::Dedicated, .name = "dedicated", .reservedCores = 0 },
} };

// The completeness check the table exists to make possible: a class added to the
// enum without a row is a build failure rather than a machine that is silently
// driven by whichever row happened to sort first.
static_assert(RowsInEnumeratorOrder(NodeClassTable, &NodeClassTraits::nodeClass),
              "NodeClassTable must hold one row per NodeClass, in enumerator order");

/// Look a class up by the name an operator spelled.
/// @param name The spelling from the command line.
/// @return The class, or nullopt when the name is not one of them.
[[nodiscard]] constexpr std::optional<NodeClass> NodeClassByName(std::string_view name) noexcept
{
    for (auto const& row: NodeClassTable)
        if (row.name == name)
            return row.nodeClass;
    return std::nullopt;
}

/// The traits for a class.
/// @param nodeClass The class.
/// @return Its row.
[[nodiscard]] constexpr NodeClassTraits const& TraitsFor(NodeClass nodeClass) noexcept
{
    return NodeClassTable[static_cast<std::size_t>(nodeClass)];
}

/// Turn a raw wire byte into a class.
///
/// Returns an optional rather than clamping, because the two failure modes are not
/// symmetric. A byte this build does not know comes from a peer built with a class
/// this one lacks, and the honest answer is to refuse the registration and say so:
/// silently reading it as `Workstation` would offer a dedicated build server two
/// cores fewer than it has, and silently reading it as `Dedicated` would saturate
/// somebody's desktop. Neither is visible from either end.
/// @param raw The byte as received.
/// @return The class, or nullopt when this build does not know it.
[[nodiscard]] constexpr std::optional<NodeClass> NodeClassFromRaw(std::uint8_t raw) noexcept
{
    if (raw >= NodeClassTable.size())
        return std::nullopt;
    return NodeClassTable[raw].nodeClass;
}

/// A node's cache budget, as it announces it at registration.
///
/// Every member of this fleet is a cache, and until this record existed the
/// leader — the one node that talks to every other — could say nothing at all
/// about any of them: not how many objects a member holds, not how close to its
/// budget it is, not whether it has a disk tier.
///
/// Split from `NodeCacheLoad` exactly as `NodeCapacity` is split from `NodeLoad`,
/// and the boundary is the same one: a budget is fixed for the process's life, so
/// re-reading it on every heartbeat would be a number nobody chose to resend.
///
/// **Not scheduling input.** `AvailableSlots` does not consult any of this and
/// must not start to: a node's cache says nothing about whether it can take
/// another compile. It is here to be reported.
struct NodeCacheCapacity
{
    /// Bytes each tier may hold, or absent when the node has no such tier.
    ///
    /// Absent is not zero, and here the two are opposite claims: absent means the
    /// node runs no tier of that kind, while zero is a tier configured with no
    /// ceiling. A dashboard renders the second as "unbounded" and would render a
    /// flattened first as the same thing.
    EnumTable<StorageTier, std::optional<std::uint64_t>> tierBytesLimit {};
};

/// What one of a node's cache tiers holds.
struct CacheTierUsage
{
    std::uint64_t itemCount { 0 }; ///< Live entries.
    std::uint64_t bytesUsed { 0 }; ///< Bytes held.
    std::uint64_t evictions { 0 }; ///< Entries dropped to stay within the budget.
};

/// What a node's cache holds right now, as it reports on each heartbeat.
///
/// The dynamic counterpart of `NodeCacheCapacity`, for the reason `NodeLoad` is
/// the dynamic counterpart of `NodeCapacity`.
struct NodeCacheLoad
{
    /// What each tier holds, or absent when the node has no such tier.
    EnumTable<StorageTier, std::optional<CacheTierUsage>> tiers {};

    /// Reads the node's cache served, and reads it could not.
    ///
    /// Node-wide rather than per tier, deliberately: a lower tier is consulted
    /// only when the one above it missed, so per-tier hit counts do not sum to the
    /// node's and a consumer adding them reports a cache serving every read at
    /// well under 100%.
    std::optional<std::uint64_t> hits;
    std::optional<std::uint64_t> misses; ///< @see hits.
};

/// What a machine is, as far as scheduling onto it is concerned.
///
/// Deliberately not `Platform::HostFacts`: that describes what a machine *is* — its
/// OS, its architecture, the facts an operator reads — while this is what a
/// scheduler *weighs*. They are collected together and travel together, but a
/// scheduler that took the whole of `HostFacts` would be coupled to every field
/// somebody adds to it for a diagnostic.
///
/// Everything here is **stable for the life of a worker process**, which is what
/// makes it a registration fact rather than a heartbeat one. Free disk is the field
/// that most looks like it belongs and does not: it moves while the process runs, so
/// a copy captured at registration would be a number the scheduler kept believing
/// long after it stopped being true.
struct NodeCapacity
{
    /// Hardware threads the machine has. Zero means "did not say", which is
    /// treated as one core rather than as none — see `OfferableSlots`.
    std::uint32_t logicalCores { 0 };
    /// Physical memory, in bytes. Zero means "did not say".
    std::uint64_t totalMemoryBytes { 0 };
    /// How hard this machine may be driven.
    NodeClass nodeClass { NodeClass::Workstation };
    /// Cores held back from the fleet. Defaults to the class's reserve; an
    /// operator may raise or lower it.
    std::uint32_t reservedCores { 0 };
    /// Whether `reservedCores` was set explicitly, or should follow the class.
    bool reserveIsExplicit { false };

    /// What this node's cache is configured to hold.
    ///
    /// Here rather than in `NodeLoad` because a budget does not move, and here
    /// rather than nowhere because the leader is the one node with a view of
    /// every member and had no view of any member's cache at all.
    NodeCacheCapacity cache {};
};

/// Memory a single translation unit is budgeted at, for slot derivation.
///
/// One job per core is the obvious rule and is wrong on a machine whose cores
/// outrun its RAM: a C++ translation unit with heavy template instantiation
/// routinely peaks in the hundreds of megabytes, so a 128-thread box with 32 GiB
/// asked for 128 concurrent compiles swaps, or the OOM killer takes them. Neither
/// failure is quiet in the right way -- the jobs come back as refusals the client
/// retries locally, so distribution appears to work while making the build slower
/// than not distributing at all, which is the shape this list already records twice.
///
/// 1 GiB is a *heuristic*, and it is deliberately generous enough that it binds
/// only where the ratio is genuinely skewed: it does not bind on any machine with
/// at least a gigabyte per hardware thread, which is every ordinary build host. An
/// operator who knows better says so with an explicit slot count, which is a
/// ceiling this never raises.
inline constexpr std::uint64_t MemoryBudgetPerJobBytes = 1ULL << 30;

/// How many concurrent jobs this machine's memory supports.
///
/// Separate from `OfferableSlots` so the arithmetic that turns bytes into a job
/// count has one author and one test, rather than being inlined into a larger
/// expression where an off-by-one is invisible.
/// @param totalMemoryBytes Physical memory, or 0 when the machine did not say.
/// @return The memory-implied job count, or nullopt when there is nothing to say.
[[nodiscard]] constexpr std::optional<std::uint32_t> MemorySlotCeiling(std::uint64_t totalMemoryBytes) noexcept
{
    // Absent is not zero: a machine that could not read its own memory must be
    // scheduled on its other properties, not clamped to the one slot a literal
    // reading of "0 bytes" would give it.
    if (totalMemoryBytes == 0)
        return std::nullopt;
    auto const jobs = totalMemoryBytes / MemoryBudgetPerJobBytes;
    return jobs == 0 ? 1U
                     : static_cast<std::uint32_t>(std::min<std::uint64_t>(jobs, std::numeric_limits<std::uint32_t>::max()));
}

/// How many concurrent jobs this machine should be offered.
///
/// Two rules, and which one applies is decided by whether the operator named a
/// number:
///
///   - **They did not** (`advertisedSlots == 0`): derive it. The core count,
///     clamped by what the memory supports, minus the class's reserve.
///   - **They did**: use it, untouched. `--slots` is set by the person whose
///     machine this is, so it is not a hint to be clamped by a heuristic written
///     here -- it is the answer. Capping it at the core count would silently
///     refuse the deliberate oversubscription an I/O-heavy build wants, and
///     subtracting the reserve on top of it would mean `--slots 4` on a
///     workstation quietly offered two, which is not what the flag says.
///
/// Never zero, and that is the load-bearing half. A node offering no slots would
/// register, match every lease for its toolchain, never be picked, and be
/// indistinguishable at the client from a fleet that is permanently busy -- the
/// diagnosis an operator would then chase is "buy more machines" for a node that
/// is sitting idle. Guaranteeing it here is what lets the scheduler stop
/// hand-checking for it, and what lets the pick comparison divide by a slot count.
///
/// A machine that reported no core count is treated as having one. Refusing to
/// schedule onto it would punish a worker for a fact it merely failed to collect,
/// and one slot is the answer that is never wrong by much.
/// @param capacity What the machine says about itself.
/// @param advertisedSlots What the operator asked for, or 0 to derive it.
/// @return Concurrent jobs to offer; never zero.
[[nodiscard]] constexpr std::uint32_t OfferableSlots(NodeCapacity const& capacity, std::uint32_t advertisedSlots) noexcept
{
    if (advertisedSlots != 0)
        return advertisedSlots;

    auto ceiling = capacity.logicalCores == 0 ? 1U : capacity.logicalCores;
    if (auto const byMemory = MemorySlotCeiling(capacity.totalMemoryBytes); byMemory.has_value())
        ceiling = std::min(ceiling, *byMemory);
    auto const reserve = capacity.reserveIsExplicit ? capacity.reservedCores : TraitsFor(capacity.nodeClass).reservedCores;

    // Saturating rather than wrapping: a two-core workstation reserving two cores
    // must offer one, not 4294967295.
    return reserve >= ceiling ? 1U : ceiling - reserve;
}

/// Disk a single job is budgeted at on the worker's scratch filesystem.
///
/// A preprocessed translation unit and the object it produces, with room to spare.
/// Smaller than `MemoryBudgetPerJobBytes` by an order of magnitude because the
/// artefacts genuinely are: what this guards against is not a large job but a
/// filesystem that has quietly filled up, which on a build host is one of the most
/// common failures there is and one of the least visible from the other end -- a
/// worker with no scratch room refuses every job it accepts, so the fleet looks
/// busy while the build compiles everything locally.
inline constexpr std::uint64_t ScratchBudgetPerJobBytes = 128ULL << 20;

/// What a machine is doing right now, as the scheduler is told on each heartbeat.
///
/// The dynamic counterpart of `NodeCapacity`, and separate for the reason that
/// struct's own comment gives: these move while the worker runs, so a copy captured
/// at registration would be a number the scheduler kept believing long after it
/// stopped being true.
///
/// Everything but `inFlight` is optional, because **absent is not zero** and here
/// the two lead to opposite decisions: a machine that could not read its CPU must
/// be scheduled on its other properties, while one that read it and got zero is
/// idle and should be given work. `inFlight` needs no optional -- the worker always
/// knows what it is running, and that is the one number it cannot fail to have.
struct NodeLoad
{
    std::uint32_t inFlight { 0 };                      ///< Fleet jobs running here right now.
    std::optional<std::uint32_t> cpuBusyPermille;      ///< Host-wide CPU busy, 0..1000.
    std::optional<std::uint64_t> availableMemoryBytes; ///< Memory a new job could get.
    std::optional<std::uint64_t> freeScratchBytes;     ///< Room where jobs are compiled.

    /// What this node's cache holds right now.
    ///
    /// Reported for an operator rather than weighed by a scheduler:
    /// `AvailableSlots` below does not read it, and must not start to. How full
    /// a node's cache is says nothing about whether it can take another compile.
    NodeCacheLoad cache {};
};

namespace Detail
{
    /// How many more jobs a resource with `available` bytes left supports.
    ///
    /// Expressed as "on top of what is already running", which is the shape all
    /// three dynamic limits share and the reason it is worth naming once: a machine
    /// reports what is left AFTER its current jobs have taken theirs, so a ceiling
    /// of `available / budget` alone would count the running jobs' own share twice
    /// and shrink a busy machine to nothing the moment it started working.
    /// @param inFlight Jobs already running.
    /// @param available Bytes left of the resource.
    /// @param budgetPerJob Bytes one job is assumed to want.
    /// @return The total job count this resource supports, running ones included.
    [[nodiscard]] constexpr std::uint32_t CeilingFrom(std::uint32_t inFlight,
                                                      std::uint64_t available,
                                                      std::uint64_t budgetPerJob) noexcept
    {
        auto const extra = std::min<std::uint64_t>(available / budgetPerJob, std::numeric_limits<std::uint32_t>::max());
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(std::uint64_t { inFlight } + extra, std::numeric_limits<std::uint32_t>::max()));
    }
} // namespace Detail

/// Which of the four ceilings decided `AvailableSlots`.
///
/// `Registered` is zero, and that is the safety property rather than an ordering
/// convenience: a breakdown nobody filled in reads as "nothing withdrew anything",
/// which is the claim that sends an operator looking at the fleet rather than at
/// one machine.
enum class SlotLimit : std::uint8_t
{
    /// Nothing withdrew anything; the machine offers what it registered with.
    Registered = 0,
    /// Somebody else is using this machine.
    ExternalCpu,
    /// What is left of its memory.
    Memory,
    /// What is left of its scratch filesystem.
    Scratch,
    Last, ///< Not a limit, and has no row: `SlotLimitTable`'s length.
};

/// One row per limit: what it is called, and what an operator does about it.
///
/// The remedy is a column rather than prose somewhere else because the three
/// reasons have *opposite* fixes -- buy a machine, free memory, free a disk -- and
/// a dashboard that named the limit without naming the fix would send an operator
/// shopping for hardware they already own. That is the same argument `PickError`'s
/// three-way split makes one layer up.
struct SlotLimitTraits
{
    SlotLimit limit;         ///< The limit this row describes.
    std::string_view name;   ///< Spelling a report uses.
    std::string_view remedy; ///< What an operator does about it.
};

/// Every limit, in enumerator order.
inline constexpr EnumTable<SlotLimit, SlotLimitTraits> SlotLimitTable { {
    { .limit = SlotLimit::Registered,
      .name = "registered",
      .remedy = "nothing is holding this machine back; it offers what it registered with." },
    { .limit = SlotLimit::ExternalCpu,
      .name = "external-cpu",
      .remedy = "somebody else is using this machine. Its own work is not the fleet's." },
    { .limit = SlotLimit::Memory,
      .name = "memory",
      .remedy = "memory is nearly gone. A compile is budgeted at 1 GiB." },
    { .limit = SlotLimit::Scratch,
      .name = "scratch",
      .remedy = "the scratch filesystem is nearly full. A compile is budgeted at 128 MiB." },
} };

// A limit added to the enum without a row is a build failure rather than a report
// that names whichever row happened to sort first.
static_assert(RowsInEnumeratorOrder(SlotLimitTable, &SlotLimitTraits::limit),
              "SlotLimitTable must hold one row per SlotLimit, in enumerator order");

/// The traits for a limit.
/// @param limit The limit.
/// @return Its row.
[[nodiscard]] constexpr SlotLimitTraits const& TraitsFor(SlotLimit limit) noexcept
{
    return SlotLimitTable[static_cast<std::size_t>(limit)];
}

/// The ceilings behind `AvailableSlots`, unfolded.
///
/// That function returns a `min` of four numbers, and folding them loses the only
/// thing an operator wants to know: a node offering 2 of its 16 slots is a
/// different problem depending on whether somebody is *using* the machine, its
/// memory is gone, or its scratch disk has filled -- and the fix differs in each
/// case. Summing or collapsing them hides all three, exactly as summing the lease
/// refusals hides the difference between an empty fleet and a busy one.
///
/// The three dynamic ceilings are `optional` for the reason `NodeLoad`'s fields
/// are: **absent is not zero**. A machine that could not read its own CPU has no
/// CPU ceiling, which is a different claim from a ceiling of zero -- one says
/// "scheduled on its other properties", the other says "take no work at all".
struct SlotCeilings
{
    /// What `OfferableSlots` gave this worker at registration.
    std::uint32_t registered { 0 };
    /// What is left after the CPU somebody else is using, or absent when the
    /// machine did not report its CPU.
    std::optional<std::uint32_t> byExternalCpu {};
    /// What its remaining memory supports, or absent when it did not say.
    std::optional<std::uint32_t> byMemory {};
    /// What its remaining scratch space supports, or absent when it did not say.
    std::optional<std::uint32_t> byScratch {};
    /// The minimum of whichever applied; what `AvailableSlots` returns.
    std::uint32_t available { 0 };
    /// Which ceiling produced `available`.
    ///
    /// On a tie this names the **first** limit in enumerator order that reached
    /// `available`, and enumerator order is the order the ceilings are applied in.
    /// Stating it makes the answer reproducible rather than dependent on how the
    /// comparison happened to be written.
    SlotLimit binding { SlotLimit::Registered };
};

/// Every ceiling that applies to this worker right now, and which one binds.
///
/// The whole of `AvailableSlots`' arithmetic, spelled apart instead of folded. The
/// three limits are the ones a heartbeat carries:
///
///   - **Somebody else is using the machine.** Host CPU busy, less what this fleet
///     is running here, is capacity that belongs to whoever is using it.
///   - **Memory is nearly gone.** What is left decides how many more jobs fit.
///   - **The scratch filesystem is nearly full.** Likewise, and it is the limit that
///     most often reaches zero on a long-lived build host.
///
/// **`capacity.cache` is not read, and must not start to be**: how full a node's
/// cache is says nothing about whether it can take another compile.
/// @param capacity What the machine is.
/// @param registeredSlots What `OfferableSlots` gave it at registration.
/// @param load What it is doing now.
/// @return Each ceiling, the resulting count, and which limit produced it.
[[nodiscard]] constexpr SlotCeilings SlotCeilingsFor(NodeCapacity const& capacity,
                                                     std::uint32_t registeredSlots,
                                                     NodeLoad const& load) noexcept
{
    SlotCeilings result { .registered = registeredSlots,
                          .byExternalCpu = std::nullopt,
                          .byMemory = std::nullopt,
                          .byScratch = std::nullopt,
                          .available = registeredSlots,
                          .binding = SlotLimit::Registered };

    if (load.cpuBusyPermille.has_value())
    {
        auto const cores = capacity.logicalCores == 0 ? 1U : capacity.logicalCores;
        auto const busyCores = (std::uint64_t { *load.cpuBusyPermille } * cores) / 1000;

        // This fleet's own jobs are subtracted, and that subtraction is what keeps
        // the rule from fighting itself: without it, giving a machine work raises
        // its CPU, which withdraws the capacity that let it take the work, which
        // frees the CPU -- a fleet that oscillates for reasons no operator can see.
        // One core per job is an approximation, and it errs towards believing a
        // machine is less loaded than it is, which is the direction the fallback
        // already covers: an over-full worker refuses and the client compiles
        // locally, where an over-shy one leaves a whole machine unused with nothing
        // reporting why.
        auto const external =
            busyCores <= std::uint64_t { load.inFlight } ? 0U : static_cast<std::uint32_t>(busyCores) - load.inFlight;
        result.byExternalCpu = external >= registeredSlots ? 0U : registeredSlots - external;
        result.available = *result.byExternalCpu;
        if (*result.byExternalCpu < registeredSlots)
            result.binding = SlotLimit::ExternalCpu;
    }

    if (load.availableMemoryBytes.has_value())
    {
        result.byMemory = Detail::CeilingFrom(load.inFlight, *load.availableMemoryBytes, MemoryBudgetPerJobBytes);
        if (*result.byMemory < result.available)
        {
            result.available = *result.byMemory;
            result.binding = SlotLimit::Memory;
        }
    }

    if (load.freeScratchBytes.has_value())
    {
        result.byScratch = Detail::CeilingFrom(load.inFlight, *load.freeScratchBytes, ScratchBudgetPerJobBytes);
        if (*result.byScratch < result.available)
        {
            result.available = *result.byScratch;
            result.binding = SlotLimit::Scratch;
        }
    }

    return result;
}

/// Slots this worker may be given **right now**.
///
/// `OfferableSlots` answers what a machine is worth in general; this answers what it
/// can take at this moment, and the difference is everything a heartbeat carries.
/// Three limits, each of which lowers the count and none of which raises it --
/// `SlotCeilingsFor` above is the same arithmetic with each of them named, and this
/// is the number it arrives at.
///
/// It may legitimately return **zero**, unlike `OfferableSlots`. A registered worker
/// must always be worth picking *eventually*, which is why that one has a floor; a
/// worker whose disk is full right now must be worth picking *never, until that
/// changes*, and a floor here would keep handing it jobs it cannot run.
///
/// @param capacity What the machine is.
/// @param registeredSlots What `OfferableSlots` gave it at registration.
/// @param load What it is doing now.
/// @return Concurrent jobs it may hold right now; may be zero.
[[nodiscard]] constexpr std::uint32_t AvailableSlots(NodeCapacity const& capacity,
                                                     std::uint32_t registeredSlots,
                                                     NodeLoad const& load) noexcept
{
    return SlotCeilingsFor(capacity, registeredSlots, load).available;
}

} // namespace FastCache::Distributed
