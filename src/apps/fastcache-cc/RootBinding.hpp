// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "PathResolve.hpp"
#include "RootAliases.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Errors/ProtocolError.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace FastCache::Cc
{

/// # An object that names its producer's checkout is not portable, and only the OBJECT can say so
///
/// A cache key is portable across checkouts by design: `<SRCROOT>` stands in for the
/// source root in every argument, the preprocessed text carries no line markers, and
/// a direct-mode manifest keys on the tokenized source path. That is the whole point
/// of the launcher -- and it is exactly why a key cannot see a compile whose OBJECT
/// names the producing checkout in data the program reads:
///
/// - **`__FILE__` under direct mode.** The preprocessor expands it, so the ordinary
///   key's raw preprocessed text does differ between checkouts -- but direct mode
///   short-circuits before any preprocessing, and the manifest key is built from the
///   tokenized source path and the relativized arguments. Two checkouts reach one
///   manifest, it validates, and it points at the producer's object.
/// - **`std::source_location` and `__builtin_FILE`, on EVERY path.** The COMPILER
///   fills those in from the presumed file name, after preprocessing; `/EP` and
///   `-E -P` suppress the line markers that would carry it, deliberately. So the text
///   `ComputeKey` hashes contains no file name at all, and two checkouts share the
///   key with direct mode off as well.
///
/// Measured on `cl` 14.51 over two byte-identical checkouts: the second one printed
/// the first one's path for both, served byte-identical objects (issue record in
/// `.agent/rules/compile-cache.md`, "An object that names its producer's roots").
///
/// No command-line rule can predict it: `__FILE__`, `source_location`, a `#line`, a
/// `/FC`, an `assert` (whose message is the WIDE `__FILE__` on MSVC), or a path-valued
/// macro nobody anticipated all reach the same place. The object is the one ground
/// truth for every mechanism at once, so it is asked.
///
/// ## The mechanism
///
/// At STORE time the launcher -- which holds both the fresh object and the layout --
/// scans the object's program-visible sections for the parts of its own checkout
/// (`ScanCheckout`): the source root, the build tree and the working directory. An
/// object naming none is stored exactly as before, under the portable key, and keeps
/// every cross-checkout hit it ever had. An object naming one is ROOT-BOUND: it is
/// stored under `ComputeRootBoundKey`, which folds the parts it NAMES into the portable
/// key, and the portable key gets a MARKER value in its place that lists those parts. A
/// consumer that fetches the portable key and meets the marker follows it with ITS OWN
/// values for exactly those parts (`ResolveFetchedObject`), which for another checkout
/// is a key nothing stored -- a MISS, whose compile stores that checkout's own
/// root-bound copy. It fails closed and heals itself, and a checkout meeting its own
/// marker HITs its own object.
///
/// Direct mode needs no second mechanism: a manifest records the PORTABLE key, and
/// following it lands on the marker exactly as the preprocessed path does. A dispatched
/// compile needs none either, because its object is written to the build's own output
/// path and stored by the same code as a local one, scanned against THIS client's roots
/// -- a worker compiles `#line`-carrying text naming the client's paths, so its
/// `source_location` names them too.
///
/// ## The marker's format, and why no generation moved
///
/// A marker is an ordinary current-generation `CompileValue` whose object blob is a
/// fixed prefix, a version and the named parts BY NAME (`EncodeRootBoundMarker`), and
/// which carries NO text region. The
/// alternative was a new field in the value, which is a `CompileValueVersion` bump, a
/// conformance row and a `GenerationBumps` entry; it buys nothing here, because:
///
/// - **Every server already stores it correctly.** A value with no text region passes
///   `CanonicalStoredValue` unchanged on every server of this generation, and a server
///   of any other generation refuses it by its leading byte -- the ordinary
///   `ForeignGeneration` refusal, never a verbatim store.
/// - **No launcher that cannot read it can reach it.** Markers live only under keys
///   derived with `objkey-v7`, which this change introduced; a launcher without this
///   code derives `objkey-v6` or earlier and never computes one.
/// - **It cannot be mistaken for an object.** Every stored OBJECT carries two text
///   regions (stdout and stderr, pushed unconditionally), and a marker carries none, so
///   the recogniser asks both and a real object whose bytes happened to spell the magic
///   would still not be one. And no consumer here can materialize a fetched value
///   without passing through `ResolveFetchedObject`, which is the one place that asks.
/// - **A marker this build cannot read is refused, never served.** Anything carrying the
///   prefix is a marker; one whose version or part names this build does not know --
///   `v1`, written by a build that bound every object to the whole checkout -- is an
///   undecodable value, which is compiled and stored over like any other.
///
/// ## Which way the scan fails, and why it errs BROAD
///
/// A spelling this scan does not recognise makes a root-bound object look portable,
/// which serves it into another checkout: **wrong, and looks right** -- the class this
/// launcher exists to never produce. A spelling it over-recognises costs one TU its
/// cross-checkout sharing: a miss. So every choice below takes the broad side:
///
/// - **Both separators, and a RUN of them.** `\` and `/` are one character, and `\\`
///   (a stringized literal, a `#line` directive's escaping) matches as one.
/// - **Case-insensitive, on every layout.** `cl /FC` has lower-cased paths, and macOS's
///   default volume is case-insensitive too.
/// - **Any non-ASCII run matches any non-ASCII run.** A root carrying `ü` is spelled as
///   two UTF-8 bytes, one code-page byte (`cl` converts `__FILE__` to the EXECUTION
///   character set without `/utf-8`) or one UTF-16 unit; each is one wildcard here.
/// - **Narrow, UTF-16LE and UTF-32LE, at every alignment.** `L"" __FILE__` and
///   `assert` on MSVC are UTF-16; `wchar_t` is 32 bits on Linux and macOS. Every
///   alignment rather than only the natural one, which also covers the big-endian
///   spellings for every ASCII character.
/// - **Both roots AND the working directory, each in two spellings.** Absolute as
///   exported (a relative export is joined onto the working directory first -- a relative
///   spelling names no checkout, and `.` would match nearly any object) and as the
///   filesystem resolves it, because a compiler writes whichever spelling it was handed.
///   The working directory is scanned because it is what `cl /FC` absolutizes a relative
///   path against, whatever the roots say: a build whose roots do not cover it otherwise
///   stores an object naming the checkout as portable. Where it IS a root -- the ordinary
///   layouts -- scanning it again costs nothing.
/// - **Everything but debug records.** What is excluded is named, never what is
///   included -- a section nobody anticipated is scanned.
/// - **What cannot be read as bytes is BOUND.** An LTO object (LLVM bitcode, GCC's
///   compressed IR, `cl /GL`'s anonymous object), clang's COVERAGE MAP (`__llvm_cov*`,
///   `.lcovmap$`/`.lcovfun$`, `__LLVM_COV,` -- a zlib stream behind a header, holding the
///   compilation directory and the absolute source; measured on clang 22 under
///   `-fcoverage-mapping`, where the root appears nowhere in plain bytes), device images,
///   and ANY section that is compressed -- flagged so, or opening with a zlib or zstd
///   header -- hide their strings; guessing "portable" about them is the
///   wrong-looking-right side.
/// - **A `?` in the object matches any single character of a root.** `cl` without
///   `/utf-8` writes `?` for a root character its code page cannot hold.
/// - **A format the walk cannot lay out is scanned WHOLE**, header, symbols and all.
///
/// What it deliberately does NOT scan are debug records -- `.debug$S`, `.debug$T`,
/// `.chks64`, DWARF. They name the producing checkout on every compile with debug info,
/// which is #203's accepted cost: counting them would end cross-checkout sharing for
/// every debug build, the alternative this design exists to avoid.
///
/// ## The bound key identifies the CHECKOUT, never the spelling
///
/// `ComputeRootBoundKey` folds each part ABSOLUTE and RESOLVED. It once folded the roots as
/// exported, and a relative export (`.` and `build`) is the same string in every checkout:
/// the marker led a second checkout straight to the first one's bound copy -- measured end
/// to end, `cl /FC`, the second checkout printing the first one's path under
/// `(root-bound: served from key=...)`. Folding the resolved absolute paths makes the key a
/// property of where the compile ran.
///
/// ## ... and only the parts the object NAMES
///
/// It once folded all three parts whatever the object named, and that ended sharing it had
/// no reason to end: a test object's `__FILE__` names the SOURCE ROOT only, and a second
/// build directory of the same checkout -- the same sources, compiled again -- missed on
/// every one of them. Measured by `launcher-replay-e2e` on Linux: 32 misses in the warm
/// build, every one a `*_test.cpp` whose object names no build directory at all, because
/// `-fdebug-prefix-map` maps it away. This is the compile-cache rule that only
/// machine-independent inputs are hashed, applied to the binding: the key folds what the
/// object is OBSERVED to name, so an object naming only the source root hits from any build
/// directory of that checkout, and one naming the build tree or the working directory misses
/// from another -- as it must, since it spells that directory. It still fails CLOSED for an
/// object that names another checkout, and one this scan cannot read is bound to every part.
///
/// The fetch cannot see what the object names before it has the object, so the MARKER
/// carries the set, and a consumer folds its own values for exactly those parts. Two
/// spellings of one directory -- a working directory that IS the build tree -- name both
/// parts, which only folds one more value that agrees in every build that shares it.
///
/// Residuals, recorded rather than rediscovered:
///
/// - A root spelled through an alias the key never MAPPED and that is not its 8.3 short
///   form -- a second `subst` drive the compile line never used, say -- is not found. An
///   alias the key did map is scanned for (`RootAliasList`), which is what keeps the key
///   and the scan from disagreeing about one: the key cannot share an entry across it
///   without the scan knowing it.
/// - A relative path argument that climbs OUT of every root and the working directory
///   (`-I..\inc` from `<checkout>\build` with the source root narrower than the
///   checkout) is absolutized by `cl /FC` to a directory this scan was not given. It errs
///   the unsafe way; the direction is recorded, and resolving each relative argument
///   lexically would close it.

class RootReconciler;

/// The name a marker spells @p part by -- the one table of them.
/// @param part A part.
/// @return Its name, `[a-z-]` only.
[[nodiscard]] std::string_view CheckoutPartName(CheckoutPart part) noexcept;

/// A set of checkout parts.
class CheckoutParts
{
  public:
    /// @return The set of every part: what an object this scan cannot read is bound to.
    [[nodiscard]] static CheckoutParts All() noexcept;

    /// @param part A part. @return This set with @p part added.
    [[nodiscard]] CheckoutParts With(CheckoutPart part) const noexcept;

    /// @param part A part. @return Whether the set holds it.
    [[nodiscard]] bool Contains(CheckoutPart part) const noexcept;

    /// @return Whether the set holds nothing.
    [[nodiscard]] bool Empty() const noexcept;

    /// @return The names of the parts in the set, in enumerator order, comma-separated.
    [[nodiscard]] std::string Names() const;

    /// @return Whether both sets hold the same parts.
    [[nodiscard]] bool operator==(CheckoutParts const&) const noexcept = default;

  private:
    std::uint8_t _bits {};
};

/// Whether an object names the producing checkout in program-visible bytes.
///
/// Private: never transmitted or persisted, so no enumerator carries an explicit value.
enum class RootBinding : std::uint8_t
{
    /// Names neither root anywhere a linked program can see: portable across checkouts.
    Portable,
    /// Names a root, or could not be read well enough to know it does not.
    Bound,
};

/// What scanning one object found.
struct RootBindingScan
{
    /// The verdict. Defaults to `Bound` so a scan nobody filled in keys apart.
    RootBinding binding { RootBinding::Bound };
    /// Why, for a `Bound` object: which root, in which section and encoding, or which
    /// part of the object could not be read. Empty for `Portable`.
    std::string evidence;
    /// Which parts of the checkout the object names -- every part for an object this scan
    /// cannot read, none for a `Portable` one. Filled by `ScanCheckout`; `ScanForRoots`,
    /// which is handed bare spellings, leaves it empty.
    CheckoutParts parts;
};

/// What a root-bound miss says -- the reason `--show-stats` tallies it under AND the
/// qualifier on its `MISS` trace line, one text for both.
///
/// It names the parts the marker names and says one of them differs here, and nothing
/// more, because nothing more is known: a consumer folds its OWN values for those parts
/// and never sees the producer's, so it cannot tell another checkout from another build
/// directory of this one. A split by the parts named claimed exactly that -- an object
/// whose `__builtin_FILE()` sits in the build tree names all three parts, and a second
/// build directory of the SAME checkout was told "another checkout's source tree".
/// Fixed text per part set, so the tally has at most a row per set rather than per compile.
/// @param parts The parts the marker named.
/// @return The description.
[[nodiscard]] std::string BoundMissReason(CheckoutParts parts);

/// The `MISS` trace line's qualifier for a root-bound miss: `BoundMissReason`, marked.
/// @param parts The parts the marker named.
/// @return ` (root-bound: <reason>)`.
[[nodiscard]] std::string BoundMissQualifier(CheckoutParts parts);

/// Where ONE compile ran, as the root binding reads it: both roots and the working
/// directory, each ABSOLUTE and in two spellings.
///
/// Built once per compile by `BindCheckout` and handed to the scan, the bound key, the
/// store plan and the fetch alike, so none of them can disagree about which checkout this is.
struct CheckoutRoots
{
    PathCanon::Layout exported;           ///< The roots as exported, made absolute.
    PathCanon::Layout resolved;           ///< The same, as the filesystem resolves them.
    std::string workingDirectory;         ///< The compile's working directory, absolute.
    std::string resolvedWorkingDirectory; ///< The same, as the filesystem resolves it.
    /// The same again, as a compiler spawned here spells it: `$PWD` where that names this
    /// directory (`CompilerWorkingDirectory`), which on POSIX is how a driver that
    /// absolutizes a path against its directory writes it. SCANNED for and never keyed:
    /// the key already folds `resolvedWorkingDirectory`, which this resolves to. Empty, or
    /// equal to `workingDirectory`, adds nothing.
    std::string compilerWorkingDirectory;
    /// Every alias spelling of a part this compile accepts (`RootReconciler::Aliases`),
    /// read when the scan runs rather than copied here: the reconciler keeps adding to it
    /// while the compile runs, and the scan at the STORE must see every alias the key
    /// mapped. Null scans for none, which only a test that builds a checkout by hand does.
    RootAliasList const* aliases { nullptr };
};

/// A root made absolute against the working directory, lexically.
/// @param root A root as exported; may be relative.
/// @param workingDirectory The compile's working directory, absolute.
/// @return @p root verbatim when it already names a location (or is drive-relative, which
///         nothing lexical can place); otherwise joined onto @p workingDirectory in its
///         separator and normalized. Empty stays empty.
[[nodiscard]] std::string AbsoluteAgainst(std::string_view root, std::string_view workingDirectory);

/// Bind this compile's checkout: every path absolute, then resolved -- and each part's
/// 8.3 short form seeded into @p reconciler's alias list, which the checkout then reads.
///
/// Takes the RECONCILER rather than its roots and resolver, because the alias list is the
/// reconciler's: binding a checkout that reads some other list is exactly the divergence
/// between key and scan that list exists to end.
/// @param reconciler This compile's reconciler: its as-given roots, its resolver, and the
///        alias list it records into.
/// @param workingDirectory The compile's working directory, ABSOLUTE: the process's own,
///        never one re-spelled in the layout's vocabulary, which under a relative export is
///        relative itself (`.` in every checkout).
/// @param compilerWorkingDirectory The same, as a compiler spawned here spells it --
///        `CompilerWorkingDirectory(workingDirectory)`. Scanned for, never keyed.
/// @return The checkout; it reads @p reconciler's alias list, so it must not outlive it.
[[nodiscard]] CheckoutRoots BindCheckout(RootReconciler& reconciler,
                                         std::string_view workingDirectory,
                                         std::string_view compilerWorkingDirectory);

/// The spellings to scan for: both roots and the working directory, each as made absolute
/// and as resolved, the working directory as a compiler spawned here spells it, and every
/// alias in the checkout's alias list.
///
/// Empty ones are dropped and duplicates collapsed, so an unaliased build whose working
/// directory is its build tree scans each path once.
/// @param checkout This compile's checkout.
/// @return The distinct, non-empty spellings.
[[nodiscard]] std::vector<std::string> RootSpellings(CheckoutRoots const& checkout);

/// Decide whether @p image names any of @p roots in bytes a linked program can see.
/// @param image The object file.
/// @param roots The root spellings, from `RootSpellings`.
/// @return The verdict and its evidence; `parts` is left empty.
[[nodiscard]] RootBindingScan ScanForRoots(std::span<std::byte const> image, std::span<std::string const> roots);

/// Decide which parts of @p checkout @p image names in bytes a linked program can see.
/// @param image The object file.
/// @param checkout This compile's checkout; every spelling of every part is looked for.
/// @return The verdict, its evidence, and the parts the object names.
[[nodiscard]] RootBindingScan ScanCheckout(std::span<std::byte const> image, CheckoutRoots const& checkout);

/// The key a root-bound object is stored under: the portable key with the parts of the
/// checkout the object NAMES folded in, each RESOLVED and absolute -- so only a compile
/// whose values agree for exactly those parts derives it. See "... and only the parts the
/// object NAMES" above.
/// @param portableKey The object key, as `ComputeKey` derived it.
/// @param checkout This compile's checkout.
/// @param parts The parts the object names, from the scan or from the marker.
/// @return A key of `KeyDigest::HexLength` characters.
[[nodiscard]] std::string ComputeRootBoundKey(std::string_view portableKey,
                                              CheckoutRoots const& checkout,
                                              CheckoutParts parts);

/// The encoded value a root-bound object leaves under its portable key.
/// @param parts The parts the object names, which every consumer folds.
/// @return A current-generation compile value; see this file's header for its shape.
[[nodiscard]] std::vector<std::byte> EncodeRootBoundMarker(CheckoutParts parts);

/// What a decoded value is, as far as the root binding is concerned.
///
/// Private: never transmitted or persisted, so no enumerator carries an explicit value.
enum class MarkerKind : std::uint8_t
{
    /// Not a marker: an object.
    Object,
    /// A marker this build reads; `parts` says which parts to fold.
    Marker,
    /// A marker this build cannot read -- another version, or a part it does not know.
    UnreadableMarker,
};

/// A value read as a marker.
struct MarkerReading
{
    MarkerKind kind { MarkerKind::Object }; ///< What the value is.
    CheckoutParts parts;                    ///< The parts, for a readable marker.
};

/// @param value A decoded compile value.
/// @return Whether it is an object, a marker (with its parts) or a marker this build
///         cannot read.
[[nodiscard]] MarkerReading ReadRootBoundMarker(CompileValue const& value);

/// Where one compile's object goes, decided from the object itself.
struct StorePlan
{
    /// The key the object is stored under.
    std::string objectKey;
    /// The key the marker is stored under, when the object is root-bound -- the portable
    /// key, which a consumer fetches first.
    std::optional<std::string> markerKey;
    /// What the scan found, for the verbose line.
    RootBindingScan scan;
};

/// Decide where @p object is stored.
/// @param portableKey The object key, as `ComputeKey` derived it.
/// @param object The object the compile produced -- locally or on a worker.
/// @param checkout This compile's checkout: what is scanned for, and what a bound key folds.
/// @param metMarker The parts of the marker this compile's own fetch met at @p portableKey,
///        if it met one -- `FetchedObject::markerParts`, whatever the bound key then held:
///        absent, undecodable, of another generation or stale. Then the object is stored
///        BOUND whatever the scan says, to those parts when the scan found none: the marker
///        is evidence that an object under this key names a root, and a portable store
///        would overwrite it for every checkout. Defence in depth -- no configuration
///        measured reaches it.
/// @return The plan; a bound plan's `scan.parts` is what its marker must carry.
[[nodiscard]] StorePlan PlanStore(std::string const& portableKey,
                                  std::span<std::byte const> object,
                                  CheckoutRoots const& checkout,
                                  std::optional<CheckoutParts> metMarker);

/// The portable key held the marker, and this build's bound key holds nothing: the
/// object is bound to another checkout's roots, or this checkout's own copy was evicted.
/// Either way a MISS, whose compile stores this checkout's copy.
struct BoundAbsence
{
};

/// What a fetch of an object key turned out to hold, once any marker is followed.
struct FetchedObject
{
    /// An object to serve; the bound key's absence; or why the value cannot be used --
    /// another generation, damaged bytes, or a marker where an object must be. An
    /// unusable value is compiled and STORED over, never compiled plainly, for the reason
    /// `CacheDecision.hpp` gives.
    ///
    /// A variant rather than a state beside two optional payloads, so no reader can take
    /// the payload of one state while holding another.
    std::variant<CompileValue, BoundAbsence, ProtocolError> answer;
    /// The key the answer came from: the portable key, or the bound one after a marker.
    std::string servedKey;
    /// Whether the portable key held the marker.
    bool followedMarker { false };
    /// The parts the followed marker named; engaged exactly when `followedMarker`, and
    /// what the STORE after a miss is handed (`PlanStore`) whichever answer it came with.
    std::optional<CheckoutParts> markerParts;
};

/// Fetches the value under one key, or nothing when there is none or the exchange failed.
using ValueFetch = std::function<std::optional<std::vector<std::byte>>(std::string const& key)>;

/// Turn the bytes fetched under an object key into something to serve, following a
/// marker to this build's bound key.
///
/// The ONE place a fetched object value is classified, so no path can materialize a
/// marker as an object file: both the preprocessed path and direct mode come through
/// here. A marker is followed at most once -- a marker under a bound key is refused as
/// undecodable rather than chased -- and one this build cannot read is refused as well.
/// @param payload What the portable key held.
/// @param portableKey The key it was fetched under.
/// @param checkout This compile's checkout.
/// @param fetch Fetches the bound key when a marker is met.
/// @return What to serve, or why nothing is served.
[[nodiscard]] FetchedObject ResolveFetchedObject(std::span<std::byte const> payload,
                                                 std::string const& portableKey,
                                                 CheckoutRoots const& checkout,
                                                 ValueFetch const& fetch);

} // namespace FastCache::Cc
