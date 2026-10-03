// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CmdLine.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

/// @file ArgumentDenials.hpp
/// The arguments no worker passes to a compiler, whatever its operator allows -- read by BOTH ends.
///
/// **Header-only, beside `CmdLine.hpp`, for the reason `CompileCacheWire.hpp` is header-only**:
/// the launcher does not link `FastCache`, and a table two processes must agree on is one table.
/// The worker consults it inside `IsAcceptableJobArgument`, above its allowlist and above the
/// operator's `--allow-compile-arg`, so no configuration re-admits a row. The LAUNCHER consults it
/// in `Dispatch` before it asks for a lease: an argument every worker refuses by row is one no
/// lease can buy, and asking cost a lease, a round trip and the whole preprocessed translation
/// unit on the wire for a refusal known in advance. The worker still enforces it, because the two
/// ends can be different builds and because the launcher is not the only thing that can reach a
/// worker's port.
///
/// **A refusal by ROW, never by absence.** Absence from the allowlist and a row here were the same
/// answer only while nothing else was consulted; `--allow-compile-arg` (#293) is consulted, and it
/// may extend the allowlist to a flag this build has not heard of -- never to one of these. That is
/// what makes "operator entries EXTEND, never replace" a property of the code rather than a comment.
///
/// **Every row is a PREFIX of the argument with one introducer stripped, and whatever follows is
/// not examined.** A refusal must not be escapable by the shape rule that narrows an allowance:
/// `-fplugin=x` carries no separator and `-fplugin=/tmp/x.so` does, so a shape-checked row would
/// refuse the harmless-looking spelling and pass the one that names a payload.

namespace FastCache::Cc
{

/// One argument family no worker passes to a compiler.
struct DeniedArgument
{
    std::string_view spelling; ///< A prefix of the argument, without its leading `-` or `/`.
    DriverFamily families;     ///< Which driver families spell it this way.
};

/// Every argument family a dispatched compile may not carry, by row.
inline constexpr std::array DeniedArguments {
    // -- the sub-tool passers inside an ALLOWED prefix. `-W` and `-m` are allowlist prefixes
    // because the warning and ISA spaces are unbounded; these are their only members that
    // are not a warning or an ISA feature -- the three sub-tool passers, and `-mllvm`,
    // whose value is a SEPARATE argument that must itself survive the allowlist.
    DeniedArgument { .spelling = "Wa,", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "Wl,", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "Wp,", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "mllvm", .families = DriverFamily::Any },

    // -- the program-invoking and code-loading options #240 is about
    DeniedArgument { .spelling = "wrapper", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "fplugin", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "fpass-plugin", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "fmodule-mapper", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "plugin", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "specs", .families = DriverFamily::Any },
    // The sub-tool pass-throughs. `-Xclang -load x.so` is two arguments and only
    // the second names the payload, so BOTH halves are refused: a rule that
    // stopped one of them would be a rule an operator could complete by allowing
    // the other.
    DeniedArgument { .spelling = "Xclang", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "Xassembler", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "Xlinker", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "Xpreprocessor", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "load", .families = DriverFamily::Any },
    // MSVC's own two.
    DeniedArgument { .spelling = "link", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "analyze", .families = DriverFamily::Msvc },

    // -- the path-valued options, which name a file on the WORKER
    //
    // A preprocessed translation unit has its headers inlined, so none of these
    // can be a legitimate part of a dispatched compile: what they would reach is
    // this machine's filesystem, not the client's build. The operator match is
    // additionally shape-checked, so a value CARRYING a separator is refused
    // whatever it is spelled -- these rows are what closes the relative spelling
    // (`-I.`, `/Foout.obj`) that the shape rule cannot see.
    DeniedArgument { .spelling = "B", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "I", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "include", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "imacros", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "idirafter", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "iquote", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "isystem", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "isysroot", .families = DriverFamily::Any },
    // clang-cl's system include directory, dropped by the launcher like `/external:I`.
    DeniedArgument { .spelling = "imsvc", .families = DriverFamily::Msvc },
    // `--sysroot=` reaches here with ONE introducer stripped, so the row keeps the
    // second dash. Spelling it `sysroot` would match nothing and read as coverage.
    DeniedArgument { .spelling = "-sysroot", .families = DriverFamily::Any },
    // The external-header directories, as a ROW rather than by absence, so
    // `--allow-compile-arg` cannot re-admit them: `/external:I` is an include path like
    // `/I`, and `/external:env:<var>` reads include paths out of THIS machine's
    // environment. Neither is needed -- see the `external:W` row above.
    DeniedArgument { .spelling = "external:I", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "external:env:", .families = DriverFamily::Msvc },
    // MSVC's file options, enumerated rather than denied as a blanket `F`: `/FC`
    // and `/FS` are ordinary and allowed above, and a prefix row would refuse them.
    DeniedArgument { .spelling = "AI", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "FA", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "FI", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "FR", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fa", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fd", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fe", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fi", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fm", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fo", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fp", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fr", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "Fx", .families = DriverFamily::Msvc },
    // The precompiled-header family. `ProducesSideArtefact` already refuses `/Yc`
    // because it WRITES one, and `cl`'s `/Yu` because its object is tied to one;
    // these refuse the rest of the family, which reads one off this machine.
    DeniedArgument { .spelling = "Y", .families = DriverFamily::Msvc },

    // -- the flags that make the compile write a SECOND artefact
    //
    // `ProducesSideArtefact` is the maintained table for this class and is asked
    // first, but it answers about the ones a compile is REFUSED for outright; these
    // are the ones this table refused by not listing them, each with a sentence in
    // `CompileJob_test.cpp` explaining why. Not a security class -- nothing here
    // runs a program -- and worse in the way this repository cares about most: only
    // the object comes back, so admitting one produces an object that names a file
    // its client never receives, under a correct key, and is then shared.
    DeniedArgument { .spelling = "gsplit-dwarf", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "gstabs", .families = DriverFamily::Any },
    // The whole profile family, in both directions: `-fprofile-generate` writes a
    // `.gcda` and `-fprofile-use=` reads one, both at paths the driver derives
    // rather than at anything on the command line.
    DeniedArgument { .spelling = "fprofile", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "fcoverage", .families = DriverFamily::Any },
    DeniedArgument { .spelling = "ftest-coverage", .families = DriverFamily::Any },
    // MSVC's separate-PDB debug formats. `/Z7` is allowed above and is the whole
    // reason these two are not: it puts the debug information IN the object, which
    // is the only place a hit can reproduce it. `RemoteCompileArgs` refuses a
    // command line carrying one (`MsvcSharedPdb`), and this is the worker's own
    // answer for a client that never asked it.
    DeniedArgument { .spelling = "Zi", .families = DriverFamily::Msvc },
    DeniedArgument { .spelling = "ZI", .families = DriverFamily::Msvc },
};

static_assert(std::ranges::none_of(DeniedArguments, [](DeniedArgument const& row) { return row.spelling.empty(); }),
              "an empty DeniedArguments spelling would refuse every argument of its family");

/// An argument as a refusal may NAME it: capped, and reduced to printable ASCII.
///
/// One reduction for both ends, because both name an argument a build wrote in text a person
/// reads -- the worker in its reply, which lands in the client's log, and the launcher in its own
/// log when it refuses up front. Long enough to identify any real flag and far too short to be a
/// payload; every byte outside printable ASCII -- control characters, terminal escapes, anything
/// non-ASCII -- becomes one `?`, so the result is valid UTF-8 whatever arrived and carries no
/// escape sequence into a terminal.
/// @param argument The argument as it arrived.
/// @return What may be printed in its place.
[[nodiscard]] inline std::string PrintableArgument(std::string_view argument)
{
    constexpr std::size_t MaxNamedArgument = 96;
    constexpr char FirstPrintable = 0x20;
    constexpr char LastPrintable = 0x7E;

    std::string named;
    named.reserve(std::min(argument.size(), MaxNamedArgument));
    for (auto const byte: argument.substr(0, MaxNamedArgument))
        named.push_back(byte >= FirstPrintable && byte <= LastPrintable ? byte : '?');
    if (argument.size() > MaxNamedArgument)
        named += "...";
    return named;
}

/// The row refusing @p arg for a driver of @p family, if one does.
///
/// One introducer is stripped before the rows are asked, exactly as the allowlist strips it, so a
/// row covers `/link` and `-link` alike. An argument that introduces no option is a bare word --
/// an input file on a compiler's command line -- and no row speaks for it.
/// @param arg One argument, as it would be passed to the compiler.
/// @param family The driver family that would receive it.
/// @return The refusing row, or nullptr when no row refuses it.
[[nodiscard]] inline DeniedArgument const* FindDeniedArgument(std::string_view arg, DriverFamily family) noexcept
{
    auto const introducers = IntroducersOf(family);
    if (arg.empty() || introducers.empty() || !introducers.contains(arg.front()))
        return nullptr;
    auto const body = arg.substr(1);
    // A loop rather than a named `find_if` iterator, for the reason `MatchLanguageSelector` gives:
    // no single spelling of the iterator's type satisfies `readability-qualified-auto` everywhere.
    for (DeniedArgument const& row: DeniedArguments)
        if (Overlaps(row.families, family) && body.starts_with(row.spelling))
            return &row;
    return nullptr;
}

} // namespace FastCache::Cc
