// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "KeyDigest.hpp"

#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

/// Ties a COMPILE reply to the COMPILE request that asked for it.
///
/// ## The rule that decides what is covered
///
/// **A field belongs here exactly when the CLIENT knows it before sending *and* the
/// runner observes it at the point of execution.** Everything below follows from
/// that one constraint, and it is stated rather than the field list because it will
/// still be true when the field list has changed. A value only the worker knows
/// cannot be verified by the client; a value only the client knows cannot be
/// reported on by the worker. Anything failing either half is not a correlation.
///
/// ## Why this exists
///
/// Nothing else tied a reply to its request
/// ([#280](https://github.com/LASTRADA-Software/fastcached/issues/280)). The cache key
/// covers the inputs, the fingerprint covers the toolchain and the lease covers the
/// authorization -- all of them upstream of the reply. The client sent a job and
/// accepted whatever object came back on that connection, so any defect crossing two
/// jobs produced a build that succeeded with the wrong object and no way to notice.
///
/// ## What it detects, and the half it cannot see
///
/// It detects a reply routed to the wrong waiter, and a runner that fed the driver
/// something other than what it was handed -- the second only because the digest is
/// filled by the runner from what it actually used, never recomputed from the request
/// at the wire layer. A digest taken from the decoded request would satisfy every test
/// that swaps two replies and would still catch nothing that mattered, because at that
/// layer both requests are pristine and the crossing happens below it.
///
/// It does **not** detect a runner that fed the right bytes and then read back the
/// wrong object file. There the metadata is honest and only the object is foreign, and
/// no input-side digest can see that. That is
/// [#279](https://github.com/LASTRADA-Software/fastcached/issues/279)'s failure, and
/// #279's exclusive scratch claim is what closes it. **#279 secures the output side and
/// this secures the input side; neither alone is sufficient.** Do not write that this
/// detects crossed objects without that qualification -- read as unconditional, it
/// makes the scratch claim look redundant, and removing it is how this codebase's worst
/// regressions have happened.
///
/// ## Not a security control
///
/// `KeyDigest` is `MurmurHash3` and this carries no key, so it is integrity against
/// ACCIDENT and nothing more: a worker that can return a wrong object can return a
/// wrong digest just as easily. A signed statement about what a worker compiled would
/// be a different mechanism with a different threat model; this is not one.
///
/// ## Framing
///
/// Folded through `KeyDigest` rather than hand-rolled, which is what keeps the
/// length-prefixed injective encoding to one author. Issue #63 is the record of why
/// that rule exists: NUL-terminated pieces let two unrelated inputs digest identically.
/// The schema tag is the domain separator, so a correlation digest can never equal an
/// `objkey-v*` or `manifest-v*` blob.
/// The inputs a correlation covers, named rather than ordered.
///
/// A struct because the covered set GROWS -- that is what the rule at the top of this
/// header is for -- and every field but one is a `std::string_view`. Six positional
/// parameters, four of them adjacent and same-typed, is a signature where a transposed
/// pair compiles, digests, and then makes both ends refuse every honest reply while
/// looking like a fleet that has stopped working. Designated initializers at the three
/// call sites make that unspellable, and adding the next covered field stops being an
/// edit to four argument lists.
///
/// Views throughout: it is built at the call site and consumed before the statement
/// ends, and everything it names outlives it there.
struct CorrelatedCompile
{
    /// The translation unit as written to scratch, post-envelope.
    std::string_view preprocessed;
    /// The argument vector the runner passed through to the driver -- the client's
    /// slice only, never the compiler path or the scratch paths the worker appends
    /// around it.
    std::span<std::string const> args;
    std::string_view fingerprint; ///< The toolchain the client named.
    std::string_view sourceName;  ///< The base name as the client sent it, before sanitizing.
    /// The client's own compile directory as it sent it, before validating; empty when
    /// the client maps nothing.
    std::string_view compileDir;
    /// What it asked that directory to read as; empty when the client maps nothing.
    std::string_view compileDirReplacement;
};

/// @param compile What this correlation covers.
/// @return The correlation, as 32 lowercase hex characters.
[[nodiscard]] inline std::string CompileCorrelation(CorrelatedCompile const& compile)
{
    // `fingerprint` is covered because two jobs identical in source, args and name
    // but built for different toolchains have different correct objects, and crossing
    // them is otherwise invisible. The worker's RESOLVED compiler path is not covered:
    // the client cannot know it, so covering it would make this unverifiable.
    //
    // `sourceName` is covered raw rather than through `SafeSourceName` because it
    // reaches the object's `.file` symbol -- seven bytes on clang-cl, per
    // `CompileRequest`'s own note -- so two jobs differing only in name have
    // different correct objects. Raw, so the client is not made a second author of the
    // sanitization rule; that makes this FINER than strictly required, which is safe,
    // where coarser would be a hole.
    //
    // The compilation-directory pair is covered for the same reason, and it satisfies
    // the rule at the top of this header exactly: the client knows both before sending,
    // and the runner observes both -- they become the two halves of the
    // `-fdebug-prefix-map` arguments on the line that is spawned. Two jobs differing
    // only there have different correct objects, and the difference is
    // `DW_AT_comp_dir`, which is what #506 is about; crossing them would otherwise be
    // the very thing that ticket closes, reappearing one layer down. Raw, like
    // `sourceName`, so the client is not made a second author of the worker's
    // validation rule.
    KeyDigest digest { "compile-corr-v2" };
    digest.Field(compile.fingerprint);
    digest.Field(compile.sourceName);
    digest.Field(compile.compileDir);
    digest.Field(compile.compileDirReplacement);
    for (auto const& arg: compile.args)
        digest.Item(arg);
    digest.Field(compile.preprocessed);
    return digest.ToHex();
}

} // namespace FastCache::Cc
