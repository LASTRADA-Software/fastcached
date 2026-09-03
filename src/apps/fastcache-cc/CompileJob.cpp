// SPDX-License-Identifier: Apache-2.0
#include "CmdLine.hpp"
#include "CompileCorrelation.hpp"
#include "CompileJob.hpp"
#include "FileBytes.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    /// Extensions a job's source name may imply, and nothing else.
    ///
    /// A fixed table rather than "take whatever is after the last dot": the value
    /// reaches a compiler's command line, and an extension is one of the few places
    /// a driver will happily accept something surprising. `.S` is here because
    /// preprocessed assembly is a real translation unit a build system produces.
    constexpr std::array<std::string_view, 7> KnownExtensions {
        ".cpp", ".cc", ".cxx", ".c", ".m", ".mm", ".S",
    };

    /// What a name falls back to when the client's cannot be used.
    constexpr std::string_view DefaultStem = "tu";
    constexpr std::string_view DefaultExtension = ".cpp";

    /// The longest stem a client may ask for.
    ///
    /// A real source name is far shorter; the cap is here because the string comes
    /// off a socket and a path has a limit on every platform this runs on.
    constexpr std::size_t MaxStemLength = 64;

    /// Names Windows resolves to a DEVICE rather than to a file, with or without an
    /// extension: `CON.cpp` opens the console.
    ///
    /// Compared case-insensitively against the stem, which is why the table carries
    /// one spelling each. A worker on POSIX is unaffected and checks anyway -- the
    /// scratch file is written by whichever host is doing the compiling, and a rule
    /// that holds only on the host that happens to be running the test is the kind
    /// this repository keeps finding the hard way.
    constexpr std::array<std::string_view, 22> ReservedDeviceNames {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };

    /// Whether a stem may be used as a file name inside the scratch directory.
    /// @param stem The name without its extension.
    /// @return True when it is safe to create.
    [[nodiscard]] bool IsSafeStem(std::string_view stem)
    {
        if (stem.empty() || stem.size() > MaxStemLength || stem.front() == '.')
            return false;
        constexpr std::string_view Allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-";
        if (!std::ranges::all_of(stem, [&](char c) { return Allowed.contains(c); }))
            return false;
        // Folded through PathCanon's byte rule rather than `std::tolower`, which is
        // locale-dependent: under a Turkish locale `std::tolower('I')` is not `i`,
        // so `LPT1` would be reserved on one worker and allowed on the next.
        std::string folded { stem };
        std::ranges::transform(folded, folded.begin(), [](char c) { return PathCanon::AsciiLower(c); });
        return std::ranges::find(ReservedDeviceNames, folded) == ReservedDeviceNames.end();
    }

} // namespace

namespace
{
    /// What may follow an allowlist row's spelling inside the same argument.
    enum class ArgValue : std::uint8_t
    {
        /// Nothing: the argument is the spelling and no more. Every bare switch.
        Bare,
        /// A longer flag name or a fused value may follow, and whatever follows must
        /// carry no path separator.
        ///
        /// This is the OLD shape rule, kept and composed *inside* a row rather than
        /// deleted wholesale. It is what makes a prefix row safe to write at all: a
        /// row-level constraint kills a whole class of value, where a carve-out per
        /// dangerous spelling kills one member per incident. It is not sufficient on
        /// its own -- `-fpass-plugin=x.so` carries no separator either, which is why
        /// the `-f` space is enumerated rather than prefixed -- so it is defence in
        /// depth beneath the allowlist, never a substitute for it.
        NoPathSeparator,
    };

    /// Whether a row admits an argument or carves one back out of a prefix that
    /// admits it.
    enum class ArgRule : std::uint8_t
    {
        Allow, ///< Accept the argument.
        /// Refuse it, overriding any `Allow` prefix it also matches. Checked before
        /// every `Allow`, so a carve-out cannot be out-voted and row order never
        /// decides an answer.
        Deny,
    };

    /// One entry in the allowlist of accepted argument shapes.
    ///
    /// The spelling is stored **without its introducer**, and matched against the
    /// argument with one introducer stripped -- the same thing `MatchPathValueFlag`
    /// does, and for the same reason. Every MSVC driver accepts `-` for every option
    /// it spells with `/`, and `clang-cl` is family `Msvc` while routinely being
    /// handed GNU spellings (`-Wall`, `-fno-exceptions`, `-O2`). Storing `/O2` and
    /// `-O2` as two rows would have refused half of those and turned every such build
    /// into a silent local fallback; one introducer-less row covers both.
    ///
    /// **A GNU-spelled row is `Any`, and that is not sloppiness.** `DriverFamily`
    /// cannot tell `cl` from `clang-cl` -- both are `Msvc` -- while `clang-cl` accepts
    /// essentially every GNU-spelled clang option (`-march=`, `-std=c++23`, `-g`,
    /// `-flto`, `-fstrict-aliasing`). Scoping those rows `Gnu` therefore refuses them
    /// on the one driver that does take them, silently, which is the failure this
    /// table must not add. Widening them costs nothing, because a client and a worker
    /// are bound to the same compiler by the FINGERPRINT: a line `cl` would merely
    /// warn about and ignore is one the client's own `cl` also ignored, so both ends
    /// still produce the same object. Rows spelled only by MSVC stay `Msvc`, because
    /// no GNU driver has them at all.
    struct AllowedArg
    {
        std::string_view spelling;                    ///< The flag, without its leading `-` or `/`.
        DriverFamily families { DriverFamily::None }; ///< Which driver families accept this spelling.
        ArgValue value { ArgValue::Bare };            ///< What may follow it.
        ArgRule rule { ArgRule::Allow };              ///< Whether the row admits or carves out.
    };

    /// The flag shapes a distributed compile legitimately carries.
    ///
    /// The accepted set `IsAcceptableJobArgument` describes: code generation,
    /// language, preprocessor-define and diagnostic options -- everything that still
    /// means something once the headers are inlined and the macros expanded. It is
    /// deliberately broader than what `RemoteCompileArgs` emits today, because too
    /// narrow a table is a silent local fallback while too broad a one is the hole
    /// this file exists to close.
    ///
    /// **The `-f` space is enumerated, never prefixed, and that is the load-bearing
    /// decision here.** A blanket `-f` prefix with a carve-out for `-fplugin=` looks
    /// like an allowlist and behaves like a denylist over the largest and most
    /// volatile flag family GCC and Clang have: a new code-loading `-f*` in a future
    /// release would be admitted *by default* until somebody noticed. That is not
    /// hypothetical -- `-fmodule-mapper=|program args` makes GCC spawn a subprocess
    /// (this tree already declares it a side-artefact flag), and `-fpass-plugin=x.so`
    /// is Clang's new pass-manager plugin loader. Neither begins with `-fplugin`, and
    /// neither carries a path separator, so neither a carve-out on that spelling nor
    /// the shape rule beneath it would have stopped them. Enumeration makes the whole
    /// class fail *closed*: an `-f` flag not listed below is refused, and the cost is
    /// one local compile.
    ///
    /// The prefixes that remain are ones whose non-listed members are a **closed**
    /// set, named as `Deny` rows beside them: `-W` (whose only non-warning members
    /// are the three sub-tool passers `-Wa,`/`-Wl,`/`-Wp,`) and `-m` (whose only
    /// pass-through is `-mllvm`, which takes its value as a separate argument that
    /// must itself survive this table). Every other prefix row's spelling ends at the
    /// option's own value separator, so the row names exactly one option and only its
    /// value is open.
    ///
    /// Absent by construction, and therefore refused: anything path-valued (`-I`,
    /// `-isystem`, `-include`, `-B`, `--sysroot`, `-specs=`, MSVC `/Fo`, `/FI`,
    /// `@response`), every sub-tool pass-through (`-Xclang`, `-Xassembler`,
    /// `-Xlinker`, MSVC `/link`), and every plugin loader (`-fplugin=`,
    /// `-fpass-plugin=`, `/analyze:plugin`).
    /// The extent is spelled out rather than deduced: `std::array`'s deduction guide
    /// folds a `is_same_v` pack over every element, and at this many rows that fold
    /// exceeds Clang's 256-deep expression nesting limit and fails to compile. A
    /// stated extent only diagnoses rows being ADDED, so the `static_assert` below
    /// the table closes the other direction.
    constexpr std::array<AllowedArg, 344> AllowedArgs {
        // -- optimization ------------------------------------------------------
        AllowedArg { .spelling = "O", .families = DriverFamily::Any },
        AllowedArg { .spelling = "O0", .families = DriverFamily::Any },
        AllowedArg { .spelling = "O1", .families = DriverFamily::Any },
        AllowedArg { .spelling = "O2", .families = DriverFamily::Any },
        AllowedArg { .spelling = "O3", .families = DriverFamily::Any },
        AllowedArg { .spelling = "Ofast", .families = DriverFamily::Any },
        AllowedArg { .spelling = "Og", .families = DriverFamily::Any },
        AllowedArg { .spelling = "Os", .families = DriverFamily::Any },
        AllowedArg { .spelling = "Oz", .families = DriverFamily::Any },
        AllowedArg { .spelling = "Od", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Oi", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Oi-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Ot", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Ox", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Oy", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Oy-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Ob0", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Ob1", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Ob2", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Ob3", .families = DriverFamily::Msvc },

        // -- language standard -------------------------------------------------
        AllowedArg { .spelling = "std=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "std:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "ansi", .families = DriverFamily::Any },
        AllowedArg { .spelling = "pedantic", .families = DriverFamily::Any },
        AllowedArg { .spelling = "pedantic-errors", .families = DriverFamily::Any },
        AllowedArg { .spelling = "permissive", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "permissive-", .families = DriverFamily::Msvc },

        // -- preprocessor defines. The value is a macro definition and may be
        // anything but a path, which the shape rule enforces.
        AllowedArg { .spelling = "D", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "U", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "undef", .families = DriverFamily::Any },
        AllowedArg { .spelling = "u", .families = DriverFamily::Msvc },

        // -- warnings. `-W`/`/W` is a prefix because the warning namespace is
        // unbounded; its only non-warning members are the three sub-tool passers
        // denied below, which is a closed set -- there is no fourth sub-tool a GNU
        // driver forwards options to.
        AllowedArg { .spelling = "W", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "Wa,", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator, .rule = ArgRule::Deny },
        AllowedArg {
            .spelling = "Wl,", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator, .rule = ArgRule::Deny },
        AllowedArg {
            .spelling = "Wp,", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator, .rule = ArgRule::Deny },
        // `-w`/`/w` suppresses warnings and is BARE on both families. It was briefly a
        // prefix here so `/wd4996` would match, and that admitted GNU `-wrapper` --
        // the very flag this ticket is about, let back in by a one-letter prefix on
        // the other family's spelling. The MSVC warning-selector prefixes below are
        // therefore MSVC-only and spelled out.
        AllowedArg { .spelling = "w", .families = DriverFamily::Any },
        AllowedArg { .spelling = "wd", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "we", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "wo", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "w1", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "w2", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "w3", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "w4", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },

        // -- machine / architecture. `-m` is a prefix because the ISA feature space
        // is unbounded and grows every release; its only pass-through is `-mllvm`,
        // denied below. `-mllvm` takes its value as a SEPARATE argument, which must
        // itself survive this table, and no LLVM option spelling appears in it.
        AllowedArg { .spelling = "m", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "mllvm", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator, .rule = ArgRule::Deny },
        AllowedArg { .spelling = "arch:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "favor:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },

        // -- debug information. Enumerated rather than prefixed: `-gsplit-dwarf`
        // writes a `.dwo` beside the object, and only the object comes back.
        AllowedArg { .spelling = "g", .families = DriverFamily::Any },
        AllowedArg { .spelling = "g0", .families = DriverFamily::Any },
        AllowedArg { .spelling = "g1", .families = DriverFamily::Any },
        AllowedArg { .spelling = "g2", .families = DriverFamily::Any },
        AllowedArg { .spelling = "g3", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ggdb", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ggdb3", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gdwarf-2", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gdwarf-3", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gdwarf-4", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gdwarf-5", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gline-tables-only", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gline-directives-only", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gcolumn-info", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gno-column-info", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gstrict-dwarf", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gno-strict-dwarf", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gpubnames", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ggnu-pubnames", .families = DriverFamily::Any },
        AllowedArg { .spelling = "gcodeview", .families = DriverFamily::Any },
        AllowedArg { .spelling = "p", .families = DriverFamily::Any },
        AllowedArg { .spelling = "pg", .families = DriverFamily::Any },

        // -- GNU feature flags, ENUMERATED. See the note above: `-f` is never a
        // prefix, because a new code-loading `-f*` must fail closed.
        AllowedArg { .spelling = "fPIC", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fpic", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fPIE", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fpie", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-pic", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-pie", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fexceptions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-exceptions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fnon-call-exceptions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "frtti", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-rtti", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fthreadsafe-statics", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-threadsafe-statics", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstrict-aliasing", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-strict-aliasing", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstrict-overflow", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-strict-overflow", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstrict-enums", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-strict-enums", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fomit-frame-pointer", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-omit-frame-pointer", .families = DriverFamily::Any },
        AllowedArg { .spelling = "finline-functions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-inline-functions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-inline", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fkeep-inline-functions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "funroll-loops", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-unroll-loops", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ffast-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-fast-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fmath-errno", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-math-errno", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ffinite-math-only", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-finite-math-only", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fsigned-zeros", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-signed-zeros", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ftrapping-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-trapping-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "frounding-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-rounding-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "freciprocal-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-reciprocal-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fassociative-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-associative-math", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fwrapv", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-wrapv", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ftrapv", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstack-protector", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstack-protector-all", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstack-protector-strong", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-stack-protector", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstack-clash-protection", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-stack-clash-protection", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fvisibility-inlines-hidden", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-visibility-inlines-hidden", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdata-sections", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-data-sections", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ffunction-sections", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-function-sections", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fcommon", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-common", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fshort-enums", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-short-enums", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fshort-wchar", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fsigned-char", .families = DriverFamily::Any },
        AllowedArg { .spelling = "funsigned-char", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fchar8_t", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-char8_t", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fconcepts", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fcoroutines", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-coroutines", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fopenmp", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-openmp", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fopenmp-simd", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fsemantic-interposition", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-semantic-interposition", .families = DriverFamily::Any },
        AllowedArg { .spelling = "flto", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-lto", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ffat-lto-objects", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-fat-lto-objects", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fuse-cxa-atexit", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-use-cxa-atexit", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fsized-deallocation", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-sized-deallocation", .families = DriverFamily::Any },
        AllowedArg { .spelling = "faligned-allocation", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-aligned-allocation", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fasynchronous-unwind-tables", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-asynchronous-unwind-tables", .families = DriverFamily::Any },
        AllowedArg { .spelling = "funwind-tables", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-unwind-tables", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdelete-null-pointer-checks", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-delete-null-pointer-checks", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fms-extensions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-ms-extensions", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fms-compatibility", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-ms-compatibility", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdelayed-template-parsing", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-delayed-template-parsing", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdeclspec", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-declspec", .families = DriverFamily::Any },
        AllowedArg { .spelling = "felide-constructors", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-elide-constructors", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-implicit-templates", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fgnu-unique", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-gnu-unique", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fplt", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-plt", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fident", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-ident", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fverbose-asm", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-verbose-asm", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fbuiltin", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-builtin", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-builtin-", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fpermissive", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fhosted", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ffreestanding", .families = DriverFamily::Any },
        AllowedArg { .spelling = "foperator-names", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-operator-names", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-access-control", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fnew-ttp-matching", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fexperimental-library", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fmerge-all-constants", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-merge-all-constants", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fjump-tables", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-jump-tables", .families = DriverFamily::Any },
        AllowedArg { .spelling = "ftree-vectorize", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-tree-vectorize", .families = DriverFamily::Any },
        AllowedArg { .spelling = "foptimize-sibling-calls", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-optimize-sibling-calls", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-lifetime-dse", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstrict-vtable-pointers", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-strict-vtable-pointers", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-devirtualize", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdollars-in-identifiers", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-dollars-in-identifiers", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-assume-sane-operator-new", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-autolink", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-rtti-data", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-c++-static-destructors", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fc++-static-destructors", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-stack-check", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fstack-check", .families = DriverFamily::Any },
        // Diagnostics rendering. Cosmetic, and none names a file.
        AllowedArg { .spelling = "fdiagnostics-color", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-diagnostics-color", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdiagnostics-absolute-paths", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdiagnostics-show-option", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-diagnostics-show-option", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdiagnostics-show-template-tree", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-diagnostics-fixit-info", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fdiagnostics-plain-output", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fcaret-diagnostics", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-caret-diagnostics", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fcolor-diagnostics", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-color-diagnostics", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fansi-escape-codes", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fshow-column", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-show-column", .families = DriverFamily::Any },
        AllowedArg { .spelling = "fno-canonical-system-headers", .families = DriverFamily::Any },
        // Valued `-f` options. Each row's spelling ends at the option's own `=`, so
        // the row names exactly one option and only its value is open.
        AllowedArg { .spelling = "fvisibility=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "fvisibility-inlines-hidden=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "ffp-contract=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "ffp-model=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fcf-protection=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fsanitize=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fno-sanitize=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fsanitize-recover=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "fno-sanitize-recover=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fsanitize-trap=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fno-sanitize-trap=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fexcess-precision=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fabi-version=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fabi-compat-version=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "fms-compatibility-version=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fmessage-length=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fmax-errors=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "ferror-limit=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "ftemplate-depth=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "ftemplate-backtrace-limit=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fconstexpr-depth=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fconstexpr-steps=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "fconstexpr-backtrace-limit=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fbracket-depth=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "fmacro-backtrace-limit=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fdiagnostics-color=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fdiagnostics-format=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fexec-charset=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "finput-charset=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fwide-exec-charset=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fstrict-flex-arrays=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "falign-functions=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "falign-loops=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "fpatchable-function-entry=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fopenmp-version=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "flto=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fvect-cost-model=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },
        AllowedArg {
            .spelling = "fstack-protector-guard=", .families = DriverFamily::Any, .value = ArgValue::NoPathSeparator },

        // -- MSVC code generation ----------------------------------------------
        AllowedArg { .spelling = "EH", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "MD", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "MDd", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "MT", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "MTd", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "MP", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "LD", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "LDd", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GR", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GR-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GS", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GS-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gs", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "Gy", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gy-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gw", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gw-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GF", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GF-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GA", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gd", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gr", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gv", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gz", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GT", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GL", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "GL-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Gm-", .families = DriverFamily::Msvc },
        // `/Z7` and no other debug-info selector. `/Zi` and `/ZI` make MSVC write a
        // PDB BESIDE the object, and only the object comes back -- the same rule that
        // keeps `-gsplit-dwarf` out of the GNU debug rows above, and the same one
        // `RemoteCompileArgs` applies at the other end (`MsvcSharedPdb`), which
        // refuses the whole command line rather than dispatching it. Admitting them
        // here would let a client that skipped that check receive an object whose
        // debug info names a PDB nothing hands back: wrong output rather than a
        // visible fallback, which is the worse of the two answers.
        AllowedArg { .spelling = "Z7", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Za", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Ze", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Zl", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Zg", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Zo", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Zo-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Zs", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Zp", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "Zc:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "RTC", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "fp:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "guard:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "diagnostics:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "errorReport:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "openmp", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "openmp-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "openmp:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "source-charset:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "execution-charset:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "constexpr:", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "vd", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "vm", .families = DriverFamily::Msvc, .value = ArgValue::NoPathSeparator },
        AllowedArg { .spelling = "Qspectre", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Qspectre-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Qpar", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Qpar-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Qfast_transcendentals", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "Qimprecise_fwaits", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "QIntel-jcc-erratum", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "utf-8", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "validate-charset", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "validate-charset-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "nologo", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "bigobj", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "sdl", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "sdl-", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "homeparams", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "hotpatch", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "await", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "J", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "X", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "FC", .families = DriverFamily::Msvc },
        AllowedArg { .spelling = "FS", .families = DriverFamily::Msvc },

        // Clang's "do not warn about arguments this compile did not use", which is a
        // DIAGNOSTIC switch and reaches nothing. `Any` rather than `Gnu` because
        // clang-cl is family `Msvc` and takes it too, and because this project's own
        // `PEDANTIC_COMPILER` presets add it to every clang build -- so a `Gnu`-only
        // row would make the fleet refuse every job it dispatches to itself, visibly
        // in a counter and nowhere else.
        AllowedArg { .spelling = "Qunused-arguments", .families = DriverFamily::Any },

        // -- the compile-only marker. The worker appends its own, but a client that
        // also sent one costs nothing and refusing it would be a pure fallback.
        AllowedArg { .spelling = "c", .families = DriverFamily::Any },
        AllowedArg { .spelling = "pthread", .families = DriverFamily::Any },
        AllowedArg { .spelling = "pipe", .families = DriverFamily::Any },
        AllowedArg { .spelling = "trigraphs", .families = DriverFamily::Any },
        AllowedArg { .spelling = "nostdinc", .families = DriverFamily::Any },
        AllowedArg { .spelling = "nostdinc++", .families = DriverFamily::Any },
    };

    // The other half of the stated extent. Adding a row past it is a compile error
    // already; REMOVING one is not -- the array pads itself with value-initialized
    // rows whose `families` is `None`, which match nothing and shrink the accepted
    // set without a diagnostic. An empty spelling is exactly what such a row has.
    static_assert(std::ranges::none_of(AllowedArgs, [](AllowedArg const& row) { return row.spelling.empty(); }),
                  "AllowedArgs' stated extent must equal its row count -- an empty row is a padded one");

    /// Whether an allowlist row matches an argument.
    /// @param row The row.
    /// @param body The argument with one introducer stripped, known non-empty.
    /// @param family The worker's driver family.
    /// @return True when the row applies and its shape matches.
    [[nodiscard]] bool ArgRowMatches(AllowedArg const& row, std::string_view body, DriverFamily family)
    {
        if (!Overlaps(row.families, family))
            return false;
        if (row.value == ArgValue::Bare)
            return body == row.spelling;
        if (!body.starts_with(row.spelling))
            return false;
        // The shape rule, composed inside the prefix rather than deleted with it.
        return !body.contains('/') && !body.contains('\\');
    }

} // namespace

JobError JobError::RejectedArgumentNaming(std::string_view argument)
{
    // Long enough to identify any real flag and far too short to be a payload. A
    // refused argument is a flag, and a client that sent a megabyte of them does not
    // get a megabyte back through this worker's reply.
    constexpr std::size_t MaxNamedArgument = 96;

    std::string named;
    named.reserve(std::min(argument.size(), MaxNamedArgument));
    for (auto const byte: argument.substr(0, MaxNamedArgument))
        // Printable ASCII only. Everything else -- control characters, terminal
        // escapes, and every non-ASCII byte -- becomes one `?`, which makes the result
        // valid UTF-8 whatever arrived and keeps an escape sequence out of the log
        // this lands in.
        named.push_back(byte >= 0x20 && byte <= 0x7E ? byte : '?');
    if (argument.size() > MaxNamedArgument)
        named += "...";

    return JobError { .reason = JobRefusal::RejectedArgument,
                      .detail = std::format("argument {} is not on this worker's accepted-flag list for its "
                                            "driver family",
                                            named) };
}

bool IsAcceptableJobArgument(std::string_view arg, DriverSpec const& driver)
{
    if (arg.empty())
        return true; // an empty argument names nothing and reaches no file
    if (arg.starts_with('@'))
        return false; // a response file names a path with no separator in it

    // Asked of the MAINTAINED table rather than restated as rows here: a flag that
    // makes the compile write a second artefact is refused, because only the object
    // comes back. `-fmodule-mapper=|program args` is on it and makes GCC spawn a
    // subprocess, so this check is load-bearing rather than tidy.
    if (ProducesSideArtefact(arg, driver.family))
        return false;

    // The language the client states for a preprocessed input, read out of the
    // driver's OWN table rather than copied. A language added to `preprocessedInput`
    // upstream would otherwise make every such job a silent `RejectedArgument`. These
    // spellings include bare value tokens (`c++-cpp-output`) that carry no introducer,
    // so they are matched before the introducer rule below.
    for (auto const& spelling: driver.preprocessedInput)
        if (std::ranges::contains(spelling.flags, arg))
            return true;

    // The target pin, from the one seam that spells it. Restating `--target=` here
    // would drift from `TargetPinPrefixFor` and refuse every dispatched clang job.
    if (auto const prefix = TargetPinPrefixFor(driver.targetDiscovery); !prefix.empty() && arg.size() > prefix.size()
                                                                        && arg.starts_with(prefix) && !arg.contains('/')
                                                                        && !arg.contains('\\'))
        return true;

    // One introducer is stripped before the table is consulted, so a row covers `/O2`
    // and `-O2` alike. An argument that introduces no option at all matches nothing:
    // it is a bare word, which on a compiler's command line is an input file.
    auto const introducers = IntroducersOf(driver.family);
    if (introducers.empty() || !introducers.contains(arg.front()))
        return false;
    auto const body = arg.substr(1);

    // One pass, with a matching `Deny` answering immediately: a carve-out cannot be
    // out-voted by an `Allow` prefix it also matches, and row order never decides an
    // answer. The shape test is the expensive half, so it is asked once per row
    // rather than once per rule.
    bool allowed = false;
    for (AllowedArg const& row: AllowedArgs)
    {
        if (!ArgRowMatches(row, body, driver.family))
            continue;
        if (row.rule == ArgRule::Deny)
            return false;
        allowed = true;
    }
    return allowed;
}

std::string SafeSourceName(std::string_view sourceName)
{
    // One component. A colon counts as a separator here even on POSIX: `C:x` is a
    // path on a Windows worker and an ordinary file name nowhere that matters, and
    // the cost of being wrong is a name, while the cost of being right is nothing.
    auto const separator = sourceName.find_last_of("/\\:");
    auto const name = separator == std::string_view::npos ? sourceName : sourceName.substr(separator + 1);

    auto const dot = name.find_last_of('.');
    auto const stem = dot == std::string_view::npos ? name : name.substr(0, dot);
    auto const extension = dot == std::string_view::npos ? std::string_view {} : name.substr(dot);

    auto const safeExtension =
        std::ranges::find(KnownExtensions, extension) != KnownExtensions.end() ? extension : DefaultExtension;

    if (!IsSafeStem(stem))
        return std::string { DefaultStem } + std::string { safeExtension };
    return std::string { stem } + std::string { safeExtension };
}

std::expected<std::vector<std::string>, JobError> WorkerPrefixMapRules(std::string_view workerDirectory,
                                                                       std::string_view clientDirectory,
                                                                       std::string_view replacement,
                                                                       DriverFamily family)
{
    // No directory is the client saying it maps nothing, and it is the ONE case that
    // must not become a refusal or a worker-chosen default: a worker that mapped anyway
    // would hand a build that asked for nothing an object naming a directory neither
    // machine has. No rules, and no error.
    //
    // An empty REPLACEMENT is not that case. `-fdebug-prefix-map=<builddir>=` maps a
    // root to nothing and is a standard reproducible-build spelling, so the directory
    // alone is what says whether a mapping is in force. The reverse -- a replacement
    // with no directory -- is genuinely half a rule and is refused: it would map
    // everything.
    if (clientDirectory.empty() && replacement.empty())
        return std::vector<std::string> {};
    if (clientDirectory.empty())
        return std::unexpected(JobError { .reason = JobRefusal::RejectedArgument,
                                          .detail = "a compilation-directory replacement needs the directory it "
                                                    "replaces" });

    // Bounded, and two different numbers because they are two different things: a
    // replacement is a relative path a build tree produces, a directory is an absolute
    // one. A single bound refuses a 300-byte directory, which is ordinary.
    constexpr std::size_t MaxReplacement = 256;
    constexpr std::size_t MaxDirectory = 4096;
    if (replacement.size() > MaxReplacement)
        return std::unexpected(JobError::RejectedArgumentNaming(replacement));
    if (clientDirectory.size() > MaxDirectory)
        return std::unexpected(JobError::RejectedArgumentNaming(clientDirectory));

    // Read off the table BEFORE anything is validated against it, so which families
    // accept the flag, how it is spelled and what separates `<from>` from `<to>` stay
    // one fact -- and so the separator check below can ask the ROW rather than rely on
    // an alphabet that happens to omit today's separator. A family with no row is a
    // worker that cannot honour the request at all.
    auto const row = std::ranges::find_if(PathValueFlags(), [family](PathValueFlag const& candidate) {
        return candidate.role == PathValueRole::PrefixMap && Overlaps(candidate.families, family);
    });
    if (row == PathValueFlags().end())
        return std::unexpected(
            JobError { .reason = JobRefusal::SpawnFailed,
                       .detail = "this worker's driver family has no path-mapping switch, so a dispatched object "
                                 "cannot record the compilation directory the client asked for" });

    // The shape rule, spelled the way `IsSafeStem` above spells its own: an explicit
    // alphabet read from a capturing lambda, and no `<cctype>`. `std::isalnum` is
    // locale-dependent, which is the exact hazard the note under `IsSafeStem` records
    // for `std::tolower` -- a rule that answers differently on two workers is how one
    // machine refuses what the next accepts.
    //
    // Two things are why this is not `IsSafeStem` itself. Bytes at or above 0x80 are
    // ALLOWED, because none of these ever becomes a path here and a build directory
    // with a non-ASCII component is ordinary; and `/`, `\`, `:` are allowed, which a
    // name that becomes a file must refuse. `:` because a Windows absolute path begins
    // `C:\` and a GNU-layout driver on Windows is an ordinary client -- without it,
    // such a client's own directory was refused before the driver family was even
    // consulted, so the refusal named the wrong end of the fleet.
    //
    // The row's own separator is excluded HERE rather than left to the alphabet, so
    // the guard cannot fail open when the table grows: `CmdLine.hpp` names the row it
    // expects next -- `-fprofile-prefix-map`, a `<path>:<something>` spelling -- and
    // `:` is in that alphabet. gcc cuts `<from>=<to>` at the last separator and clang
    // at the first, so a value carrying one is a rule the two drivers read differently
    // (measured: `/tmp/l506b/eq=sign` mapped to `.` gives `.` on gcc 14 and
    // `sign=.=sign` on clang 20 -- a WRONG compilation directory, not a missing one).
    constexpr std::string_view Allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./\\:_-+~";
    auto const unspellable = [&](char c) {
        return c == row->valueTailSeparator || (static_cast<unsigned char>(c) < 0x80 && !Allowed.contains(c));
    };

    // ALL THREE values, and the two halves are attributed differently on purpose: a
    // value the client sent is the CLIENT's fault, this worker's own directory is the
    // WORKER's. Blaming a client for a property of the machine it was sent to sends an
    // operator to the wrong end of the fleet.
    if (std::ranges::any_of(clientDirectory, unspellable))
        return std::unexpected(JobError::RejectedArgumentNaming(clientDirectory));
    if (std::ranges::any_of(replacement, unspellable))
        return std::unexpected(JobError::RejectedArgumentNaming(replacement));
    if (workerDirectory.empty() || workerDirectory.size() > MaxDirectory
        || std::ranges::any_of(workerDirectory, unspellable))
        return std::unexpected(
            JobError { .reason = JobRefusal::SpawnFailed,
                       .detail = "this worker's own compile directory cannot be spelled inside a mapping rule, so no "
                                 "unambiguous rule exists" });

    auto const ruleFor = [&](std::string_view directory) {
        // The separator appears twice -- once joining the flag to its value, once
        // inside that value -- and this spells both from the row's own
        // `valueTailSeparator`. That is one character for every row the table can
        // currently hold, and it is an assumption rather than a fact, so
        // `CompileJob_test` pins the join DIRECTLY. Re-parsing what this emits does not
        // pin it: `StripJoinSeparator` accepts every character in `JoinSeparators` by
        // design, so a rule joined with `:` parses back with the same head and the same
        // tail. All five of that guard's assertions passed with this line broken.
        return std::format(
            "{}{}{}{}{}", row->spelling, row->valueTailSeparator, directory, row->valueTailSeparator, replacement);
    };

    // **The worker's own rule is DROPPED when it would also match the client's
    // directory**, and that is not a nicety -- it is the difference between a mapped
    // object and a corrupted one.
    //
    // A prefix-map rule appends the unmatched tail, so `<from>` = `/` rewrites
    // `/home/ci/build` to `.home/ci/build` and `/usr/include/...` to `.usr/include/...`
    // -- every absolute path in the object. And `/` is the production value: the shipped
    // `fastcache-compile-node.service` sets no `WorkingDirectory=`, so systemd starts
    // the node in `/`, and `PosixDaemonHost` calls `chdir("/")` on the daemonize path.
    //
    // Measured on gcc 14.2.0, node in `/`, both rules present with this one last:
    // `DW_AT_comp_dir` came back `.tmp/l506d/client` and every system header read
    // `.usr/include/...`. That is a WRONG object under a correct key -- strictly worse
    // than the unmapped directory #506 is about.
    //
    // Dropping it rather than refusing the job is deliberate. The client's rule still
    // lands, so the gcc case -- where the compile adopts the CLIENT's directory from the
    // preprocessed text -- is fully mapped: measured, `comp_dir` is `.`. What is left
    // is clang on such a node, whose object keeps the worker's directory, which is
    // exactly the pre-#506 state rather than a new defect. Refusing instead would cost
    // every dispatched compile on every node installed from the shipped unit, to buy a
    // case that is no worse than it was.
    //
    // The real repair is a `WorkingDirectory=` in the unit, which is packaging.
    if (clientDirectory.starts_with(workerDirectory))
        return std::vector<std::string> { ruleFor(clientDirectory) };

    // BOTH, to the same replacement. Which directory the object records is the driver's
    // answer rather than the fleet's -- gcc under `-g` puts the CLIENT's into the
    // preprocessed text and the compile adopts it, clang leaves this worker's showing --
    // so mapping both gives one answer either way. The worker's own goes LAST, and by
    // the guard above it cannot contain the client's directory, so the order is free.
    return std::vector<std::string> { ruleFor(clientDirectory), ruleFor(workerDirectory) };
}

CompileJobRunner::CompileJobRunner(IProcessRunner& runner,
                                   std::filesystem::path scratchRoot,
                                   std::map<std::string, std::string> toolchains,
                                   ToolchainSurvey survey):
    _runner { runner },
    _scratchRoot { std::move(scratchRoot) },
    _toolchains { std::move(toolchains) },
    _survey { survey }
{
}

std::vector<std::string> CompileJobRunner::Fingerprints() const
{
    std::shared_lock const guard { _toolchainsMutex };
    std::vector<std::string> out;
    out.reserve(_toolchains.size());
    for (auto const& [fingerprint, compiler]: _toolchains)
        out.push_back(fingerprint);
    return out;
}

void CompileJobRunner::ReplaceToolchains(std::map<std::string, std::string> toolchains)
{
    std::unique_lock const guard { _toolchainsMutex };
    _toolchains = std::move(toolchains);
    _survey = ToolchainSurvey::Completed();
}

std::expected<CompileOutcome, JobError> CompileJobRunner::Run(CompileJob const& job)
{
    // The compiler comes from THIS worker's configuration, keyed by the fingerprint
    // the client named. The client never names a program. A fingerprint this worker
    // does not have is refused rather than served with a default -- the scheduler
    // should not have sent it, and a client that reached this port directly did not
    // go through scheduling at all.
    //
    // Copied OUT of the map, under the lock, rather than kept as an iterator. The
    // map can be replaced while this job runs -- a node re-surveys when a compiler
    // is patched underneath it (#238) -- and the two uses below are far downstream,
    // after the scratch directory is created and the whole preprocessed source is
    // written. An iterator held across that is a dangling read on the line that
    // decides which program executes. A job already admitted therefore finishes
    // against the compiler it looked up, which is also the honest answer: it is what
    // the client was told it would get.
    std::string compiler;
    {
        std::shared_lock const guard { _toolchainsMutex };
        // Asked BEFORE the lookup, not after it fails, and under the same lock: the
        // two are one fact. A worker still walking its include trees has an empty map
        // for a reason that has nothing to do with the fingerprint it was handed, and
        // reporting `UnknownFingerprint` there tells an operator the fleet is matching
        // the wrong machines when the answer is "this one is still starting" (#365).
        if (!_survey.HasCompleted())
            return std::unexpected(JobError { .reason = JobRefusal::ToolchainSurveyInFlight, .detail = {} });
        auto const found = _toolchains.find(job.fingerprint);
        if (found == _toolchains.end())
            return std::unexpected(JobError { .reason = JobRefusal::UnknownFingerprint, .detail = {} });
        compiler = found->second;
    }

    // Derived from the worker's OWN configured compiler, never from anything the
    // client sent -- the same rule that governs which program runs. Used both to vet
    // the client's arguments against this family's allowlist and to spell the output
    // flag far below, so it is derived once here.
    auto const& driver = DriverOf(ClassifyCompiler(compiler));
    auto const family = driver.family;

    // A compiler this worker cannot classify is a WORKER fault, and it is refused as
    // the job rather than per argument. Two things go wrong otherwise. The allowlist
    // consults `IntroducersOf(None)`, which is empty, so every argument is refused --
    // reported as `RejectedArgument` and counted under it, sending an operator to look
    // at the fleet's *flags* when the answer is this node's `--toolchain`. And a job
    // with an EMPTY argument list has nothing to refuse, so it would sail past the
    // loop and spawn a driver whose command-line dialect this worker does not know.
    // `SpawnFailed` is the honest existing answer -- "this worker is broken, compile
    // it elsewhere" -- and it carries the wire code and the counter that say so.
    if (family == DriverFamily::None)
        return std::unexpected(JobError { .reason = JobRefusal::SpawnFailed,
                                          .detail = "this worker's configured compiler matches no known driver "
                                                    "family, so its command line cannot be built safely" });

    // Checked again here, on the receiving side, and against an ALLOWLIST -- see
    // `IsAcceptableJobArgument`. The client's filter protects an honest client from
    // dispatching something that would not work; this one protects the worker from a
    // client that is not honest. Trusting the client's check would mean the worker is
    // secured by code running on the caller's machine -- and the client's check is a
    // shape rule that admits every program-invoking option carrying no path
    // separator, which is exactly the surface this must not expose.
    if (auto const offender =
            std::ranges::find_if(job.args, [&](std::string const& arg) { return !IsAcceptableJobArgument(arg, driver); });
        offender != job.args.end())
        return std::unexpected(JobError::RejectedArgumentNaming(*offender));

    // Every path below is the worker's. Nothing the client sent decides where a byte
    // lands -- not the source name, not the object name, not the directory.
    std::error_code ec;
    auto const scratch = _scratchRoot / std::format("job-{}", _nextJob.fetch_add(1, std::memory_order_relaxed));
    std::filesystem::create_directories(scratch, ec);
    if (ec)
        return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });

    // Removed however this returns, including on a refusal below: a worker that
    // leaked a directory per job would fill its disk in a long-running build.
    class ScratchGuard
    {
      public:
        explicit ScratchGuard(std::filesystem::path path) noexcept:
            _path { std::move(path) }
        {
        }
        ~ScratchGuard()
        {
            std::error_code ignored;
            std::filesystem::remove_all(_path, ignored);
        }
        ScratchGuard(ScratchGuard const&) = delete;
        ScratchGuard& operator=(ScratchGuard const&) = delete;
        ScratchGuard(ScratchGuard&&) = delete;
        ScratchGuard& operator=(ScratchGuard&&) = delete;

      private:
        std::filesystem::path _path;
    };
    ScratchGuard const guard { scratch };

    auto const source = scratch / SafeSourceName(job.sourceName);
    auto const object = scratch / "tu.o";
    {
        std::ofstream out { source, std::ios::binary };
        if (!out)
            return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });
        out.write(job.preprocessed.data(), static_cast<std::streamsize>(job.preprocessed.size()));
        if (!out.good())
            return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });
    }

    std::vector<std::string> argv;
    argv.reserve(job.args.size() + 7);
    argv.push_back(compiler);
    argv.insert(argv.end(), job.args.begin(), job.args.end());
    // AFTER the client's arguments, because both GNU drivers honour the LAST matching
    // rule -- so a build that carried a prefix-map of its own could not override the
    // rules this exists to add. Before `-c` and the paths below, which is cosmetic;
    // flag order against the input is not significant to either driver.
    //
    // `current_path()` is this worker's own fact and is asked for at the leaf that
    // already does the filesystem work: the compiler inherits THIS process's directory,
    // because `IProcessRunner` spawns with no directory of its own and every path here
    // is absolute. It is one of the TWO directories a dispatched object can record; the
    // other is the client's, which gcc puts inside the preprocessed text under `-g`.
    // See `WorkerPrefixMapRule` for the measurements and for why both are mapped.
    if (!job.compileDir.empty() || !job.compileDirReplacement.empty())
    {
        // The `error_code` overload, so a filesystem that cannot answer is a refusal
        // rather than an exception thrown on a worker thread. Asked only when a mapping
        // was requested: it is a syscall, and a fleet that maps nothing must not pay it
        // once per job.
        //
        // `SpawnFailed` on the precedent above -- "this worker is broken, compile it
        // elsewhere" -- which is the client's cue to compile locally and get the object
        // it actually wanted. Silently skipping the rules would instead return an object
        // whose compilation directory disagrees with a locally built one under the same
        // key, which is #506 itself.
        std::error_code cwdError;
        auto const compileDirectory = std::filesystem::current_path(cwdError);
        if (cwdError)
            return std::unexpected(JobError { .reason = JobRefusal::SpawnFailed,
                                              .detail = "this worker cannot read its own working directory, so a "
                                                        "dispatched object cannot record the compilation directory "
                                                        "the client asked for" });

        auto rules = WorkerPrefixMapRules(compileDirectory.string(), job.compileDir, job.compileDirReplacement, family);
        if (!rules.has_value())
            // Whichever fault it was, named where it was decided: the refusal knows
            // which of the two values it refused, where a caller reconstructing that
            // from emptiness would name the wrong half.
            return std::unexpected(rules.error());
        argv.insert(argv.end(), std::make_move_iterator(rules->begin()), std::make_move_iterator(rules->end()));
    }
    // The compile action and the output are the worker's to name, which is why the
    // client's `RemoteCompileArgs` dropped both rather than passing them through.
    //
    // The OUTPUT flag comes from the driver family, because `-o` is not universal:
    // `cl` does not accept it. Hard-coding it meant MSVC quietly wrote `tu.obj`
    // beside the source, exited 0, and this worker then found nothing at the path
    // it had asked for and refused the job as ScratchUnavailable -- so
    // distribution never worked on Windows, and said "storage write failed" about
    // it. `-c` needs no such treatment: MSVC drivers accept `-` for every option,
    // so it means the same to both families.
    //
    // `family` was derived once at lookup from the worker's OWN configured compiler,
    // never from anything the client sent -- the same rule that governs which program
    // runs, and which vetted the arguments above.
    argv.emplace_back("-c");
    argv.push_back(source.string());
    // Fused, which both families accept and which is the only form MSVC documents
    // for `/Fo`.
    argv.push_back(std::string { ObjectOutputPrefixFor(family) } + object.string());

    // Recorded HERE, from the vector that is about to be spawned and the text that
    // was just written to scratch -- not in `WorkerProtocol` from the decoded request.
    // That placement is the whole mechanism: at the wire layer two crossed requests are
    // both still pristine, so a digest taken there agrees with whatever it is compared
    // against. Taken from `argv`'s own client slice rather than from `job.args` again,
    // so anything that ever edits the vector between here and the spawn is followed
    // rather than described. See `CompileCorrelation` (#280).
    auto const correlation =
        CompileCorrelation(CorrelatedCompile { .preprocessed = job.preprocessed,
                                               .args = std::span<std::string const> { argv }.subspan(1, job.args.size()),
                                               .fingerprint = job.fingerprint,
                                               .sourceName = job.sourceName,
                                               .compileDir = job.compileDir,
                                               .compileDirReplacement = job.compileDirReplacement });

    auto run = _runner.RunCaptureSplit(argv);
    if (run.exitCode == NotSpawned)
        // The compiler could not be spawned at all. Deliberately NOT reported as a
        // failed compile: the client must be able to tell "this worker is broken"
        // from "your code does not compile", because only the second is its answer.
        return std::unexpected(JobError { .reason = JobRefusal::SpawnFailed, .detail = {} });

    CompileOutcome outcome {
        .exitCode = run.exitCode,
        .object = {},
        .stdoutText = std::move(run.out),
        .stderrText = std::move(run.err),
        .correlation = correlation,
    };
    if (run.exitCode == 0)
    {
        auto bytes = ReadFileBytes(object);
        if (!bytes.has_value())
            // The compiler said it succeeded and produced nothing readable. Refused
            // rather than returned as an empty object, which the client would write
            // to disk and cache.
            return std::unexpected(JobError { .reason = JobRefusal::ScratchUnavailable, .detail = {} });
        outcome.object = *std::move(bytes);
    }
    return outcome;
}

} // namespace FastCache::Cc
