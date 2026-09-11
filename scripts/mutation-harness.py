#!/usr/bin/env python3
"""The tracked mutation harness: neuter a fix, prove the test goes red for THAT
reason and only that reason, put the tree back, and prove the tree is back.

`.agent/rules/testing.md` makes *prove the test can fail* an acceptance step for
much of this repository's work, so this is not a convenience. Until #1252 every
lane wrote its own.

## The census that motivated this, with its pattern, because a count nobody can
## re-derive is what gets a ticket closed against a number that no longer
## describes the tree

Measured 2026-09-11, over every session scratchpad for this project and every
worktree on one developer machine -- a measurement at an INSTANT, so its
conditions are restated here rather than pointed at, and it is not expected to
track anything:

    HARNESS := (a) WRITES a modified source
               AND (b) RUNS something that yields a verdict
               AND (c) RESTORES what it wrote

    (a) .replace( | io.open(..,'w'/'wb') | sed -i | perl -pi | git apply
    (b) ctest | ninja | cmake --build | FastCacheTest | *-tests | run-check.sh
        | check-<name>.sh/.cmake
    (c) git checkout/restore -- | shutil.copy*/move | cp *.bak/.orig/.pristine
        | write-back of a saved buffer | git stash

  54 harnesses. 26 of them carry a `git checkout` restore -- a SHAPE count and
  NOT a defect count. 237 further files satisfy (a) and (b) and restore nothing,
  so clause (c) is doing all of the discriminating work; without it the same
  scan answers 670, having selected every edit script in the fleet.

Three defects turned up in the enumerator itself while it was being written, and
they are worth knowing because this file has the same shape: it matched ITSELF
(a tool whose subject is tools carries its own patterns as literals), it folded
"no restore region could be located" into "the region has no rebuild", and its
shortened display paths collapsed distinct scratchpads onto one prefix so no
listed file could be reopened.

WHY A DEFECTIVE HARNESS IS WORSE THAN NO HARNESS, which is what justifies the
size of this file: it does not fail loudly. It reports that a test did NOT redden
when the fix was removed -- and on that reading the author's correct next move is
to DELETE the test as worthless, since not reddening under mutation is #355's
disqualifying property. So a harness bug destroys the test that would have caught
the next regression, while every signal the author sees says the test is bad.

## The four defects this is built against, each already paid for separately

1. **`git checkout -- <path>` as the restore.** It restores the INDEX, so on
   uncommitted work it silently DELETES the change the harness exists to falsify,
   and every later arm measures the merge base. This file never invokes git. The
   snapshot is a byte copy and the restore is the reverse copy.

2. **Restore without rebuild.** The source goes back, the previous mutation's
   BINARIES stay, and the next baseline runs against a mutant on a tree git calls
   clean. Nothing is lost and nothing looks wrong. Two independent guards, because
   a structural one cannot be shown to bite:
     - the rebuild is spelled twice, after the mutation and after the restore; and
     - the baseline is RE-MEASURED after the restore and must come back to the
       opening control's FAIL set exactly.
   The second is what makes the first TESTABLE. Delete the post-restore rebuild
   and the re-measurement fails, which is
   `check-mutation-harness-selftest.py`'s `stale-binary` case and #1252's
   clause 2 -- the clause the ticket calls the whole ticket. A rule whose
   violation nothing can observe is decoration.

3. **Verdict read from the exit status.** Catch2 spends its exit code on the
   failed-assertion COUNT, so a *caught* mutation scores as "no verdict" and 4
   failures collide with `SKIP_RETURN_CODE 4` (#1128/#1152). The verdict here comes
   from a named FAIL SET parsed out of the OUTPUT. The exit status is recorded as
   evidence and never consulted for the verdict, which makes this structurally
   immune rather than defensively coded.

4. **"And only those" documented but not asserted.** A docstring promising exact
   matching over code that only checks the FAIL set is non-empty holds exactly as
   long as a human does the comparison by eye. `_verdict` compares SETS.

## The subject-kind question, answered as DATA rather than as two tools

#1252 left open whether one harness can serve a compiled and a shell subject. It
can, and the difference is one required field.

A shell subject is re-read from disk every run, so it needs no build -- and a
harness with no build step is INDISTINGUISHABLE, on a green run, from one that
needs a build and forgot it. That is the trap: absence of a defect as a property
of the subject rather than a virtue of the design. Promote such a harness and it
ships silently wrong for every compiled subject, inheriting the bug it was adopted
to prevent.

So the difference is an INPUT and the obligation is STRUCTURAL, in two steps:

  * `subject_kind` is required and closed -- `"compiled"` or `"interpreted"`,
    no default and no third value. It is DECLARED rather than sniffed, because
    the census classifier that guessed from file extensions was fooled by
    mutation PAYLOAD text (see `_load_plan`).
  * `build` is required and undefaulted, and `"build": "none"` is REFUSED for a
    compiled subject outright. No `build_none_reason` buys an exemption there.

The reason string survives only on the interpreted path, where it says why THIS
subject needs no build. That is the repository's own `RefuseWithoutCounter`
argument in another medium -- **deliberately absent must not be spelled like
forgot** -- but a reason is documentation, and *do something* is an obligation.
An obligation belongs in a required input, not in a sentence a reader has to
agree with; a rebuild that a plausible paragraph can switch off is defect 2 one
convincing author away from returning.

## Outcomes are FOUR, because three of them are not failures

`FIRED` (the arm reddened exactly the expected set), `SURVIVED` (it did not
redden), `ANCHOR` (the anchor did not match uniquely, so the mutation was never
applied and the verdict is UNKNOWN -- not a pass and not a fail), and `ABORTED`
(the harness cannot answer: control red, stale binary, a build that named
nothing). Folding `ANCHOR` into either verdict is how a harness reports on a
mutation it never made.

## Watched refusing on a REAL compiled subject, not only on staged trees

`check-mutation-harness-selftest.py` drives staged trees, which is what makes it
a hygiene check that needs no compiler. Staged fixtures prove a tool RUNS; only a
real target catches a wrong one -- `launcher-replay-e2e.sh` is this repository's
standing example. So this was also driven against `src/FastCache/Net/ReadSlot.hpp`
with real ninja and real ctest, removing the `assert` in `ClaimReadSlot` so the
must-die `read-slot-guard-canary` SURVIVES:

    control : GREEN
      arm: the read-slot guard's assert is removed, so the canary SURVIVES
        run (mutant): exit=8  fails=['read-slot-guard-canary']       -> FIRED
        run (post-restore baseline): exit=0  fails=none
    control : GREEN (closing)

Two things that only a real subject shows. `ctest` exited **8** -- a number that
carries no verdict at all -- while the FAIL set was exact, which is defect 3 in
the wild rather than in a fixture. And with the post-restore rebuild DELETED from
a copy, the same plan gives:

    run (post-restore baseline): exit=8  fails=['read-slot-guard-canary']
    ABORTED: ... The source is back on disk, so the difference is a STALE BINARY

That is #1252's defect 2 exactly: the source restored, the tree clean to `git`,
and the binary still the mutant. A staged "compiler" cannot produce real ninja's
`no work to do`, which is why `build_names` is checked against a real build too.

Usage:
    python3 scripts/mutation-harness.py --config <plan.json> [--arm NAME] [-v]

The plan format is documented in `_load_plan`. A worked one, from the run above:

    {
      "root": "<the worktree>",
      "subject": "src/FastCache/Net/ReadSlot.hpp",
      "build": ["ninja", "-C", "out/build/reg", "read-slot-guard-canary"],
      "run": ["ctest", "--test-dir", "out/build/reg",
              "-R", "^read-slot-guard-canary$"],
      "fail_pattern": "^\\\\s*\\\\d+ - (\\\\S+) \\\\(",
      "build_names": ["read-slot-guard-canary"],
      "arms": [{"name": "...", "old": "<anchor>", "new": "    (void) slot;",
                "expect_fail": ["read-slot-guard-canary"]}]
    }
"""

import argparse
import io
import json
import os
import re
import shutil
import subprocess
import sys

PRISTINE_SUFFIX = ".mutpristine"

FIRED = "FIRED"
SURVIVED = "SURVIVED"
ANCHOR = "ANCHOR-NOT-MATCHED"
ABORTED = "ABORTED"


class Abort(Exception):
    """The harness cannot answer. Never a verdict about the subject."""


def _run(cmd, cwd):
    """Run a command, returning (exit status, combined output).

    The status is EVIDENCE. Nothing in this file derives a verdict from it --
    see defect 3 in the module docstring.
    """
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def _load_plan(path):
    """Read and VALIDATE a mutation plan.

    The table is an input rather than a literal, because a harness whose table is
    hardcoded is copied rather than reused -- which is how this repository came to
    have 54 of them.

    Required keys:
      root          directory every relative path below is resolved against
      subject       the file to mutate
      subject_kind  "compiled" or "interpreted" -- DECLARED, never sniffed
      build         argv list; or "none" plus `build_none_reason`, and only
                    when `subject_kind` is "interpreted"
      run           argv list producing the verdict output
      fail_pattern  regex with ONE capture group naming a failing item
      arms          [{name, old, new, expect_fail: [...]}]
    Optional:
      build_names   regexes the build output must match after a real mutation, so
                    a build that did nothing ("ninja: no work to do") is caught
                    rather than believed. That is a SECOND, independent detector
                    of defect 2, and it is not theoretical here: ninja over DrvFs
                    has been observed missing an edit outright.

    Every absence is an error. There are no defaults, because a default is how a
    field that matters gets omitted in silence.

    ## Why `subject_kind` is declared and not inferred

    A compiled subject has a BINARY that can go stale; an interpreted one is
    re-read from disk every run and has nothing to rebuild. That difference
    decides whether a rebuild is mandatory, so it must be an input rather than
    something this file guesses.

    Guessing was tried and is unsound. A census classifier that read the file
    extensions a harness mentions put `laneD588-mutate.py` in the compiled set
    because it names `src/Deep/Base.hpp` -- inside a mutation PAYLOAD it writes,
    while its real subject is `scripts/tidy-sweep.sh`. **A classifier fooled by
    the text a harness mutates must not be the thing that decides whether a
    rebuild is required.**

    So the obligation is structural rather than advisory: `"compiled"` with
    `"build": "none"` is REFUSED, and no reason string can buy an exemption. The
    one inference kept is a REFUSAL in the safe direction -- a subject whose own
    path ends in a compiled extension may not be declared interpreted -- which
    reads the subject PATH and never any payload.
    """
    with io.open(path, encoding="utf-8") as fh:
        plan = json.load(fh)

    for key in ("root", "subject", "subject_kind", "build", "run",
                "fail_pattern", "arms"):
        if key not in plan:
            raise Abort("plan is missing the required key %r. There is no default "
                        "for it: a default is how a field that matters gets "
                        "omitted in silence." % key)

    if plan["subject_kind"] not in ("compiled", "interpreted"):
        raise Abort('subject_kind must be "compiled" or "interpreted", not %r. It '
                    'decides whether a rebuild is mandatory, so there is no third '
                    'value and no default.' % (plan["subject_kind"],))

    # The safe-direction inference, on the subject's own PATH. Declaring a .cpp
    # interpreted would silently switch off the rebuild for a subject that has a
    # binary; the opposite mistake only costs a redundant build, so it is allowed.
    if (plan["subject_kind"] == "interpreted"
            and os.path.splitext(plan["subject"])[1].lower()
            in (".cpp", ".hpp", ".cc", ".cxx", ".h")):
        raise Abort(
            'subject %r is declared "interpreted", but its extension says it is '
            "compiled -- and an interpreted subject is exempt from the rebuild. If "
            "that exemption were granted here, every arm after the first would "
            "measure a stale binary." % plan["subject"])

    if plan["build"] == "none":
        if plan["subject_kind"] == "compiled":
            raise Abort(
                'build is "none" for a COMPILED subject. Its binary outlives the '
                "restore, so a rebuild is not optional and no `build_none_reason` "
                "buys an exemption -- that is the whole of #1252's defect 2. An "
                "obligation to DO something belongs in a required input, not in a "
                "sentence a reader has to agree with.")
        if not plan.get("build_none_reason"):
            raise Abort(
                'build is "none" with no `build_none_reason`. A subject that '
                'needs no build must SAY WHY it needs none -- an interpreted '
                'subject is re-read from disk every run, and on a green run that '
                'is indistinguishable from a compiled subject whose rebuild was '
                'forgotten. Deliberately absent must not be spelled like forgot.')
    elif not isinstance(plan["build"], list) or not plan["build"]:
        raise Abort('build must be a non-empty argv list, or the literal "none" '
                    'with a `build_none_reason`.')

    if not plan["arms"]:
        raise Abort("the plan has no arms, so a green run would vouch for nothing")

    for i, arm in enumerate(plan["arms"]):
        for key in ("name", "old", "new", "expect_fail"):
            if key not in arm:
                raise Abort("arm %d is missing %r" % (i, key))
        if arm["old"] == arm["new"]:
            raise Abort("arm %r mutates nothing: `old` and `new` are identical, so "
                        "it would report SURVIVED for a mutation never made"
                        % arm["name"])
        if not arm["expect_fail"]:
            raise Abort(
                "arm %r expects an EMPTY fail set. An arm that expects nothing to "
                "redden cannot distinguish a working test from a deleted one -- "
                "assert what DISTINGUISHES (#355)." % arm["name"])

    if re.compile(plan["fail_pattern"]).groups != 1:
        raise Abort("fail_pattern must have exactly ONE capture group, naming the "
                    "failing item. Without a group there is a count and no set, "
                    "and a count cannot be compared for exactness.")
    return plan


def _fail_set(pattern, output):
    """The named FAIL set, from the OUTPUT. Never from an exit status."""
    return set(m.group(1).strip() for m in re.finditer(pattern, output, re.M))


class Harness:
    def __init__(self, plan, verbose=False):
        self.plan = plan
        self.verbose = verbose
        self.root = os.path.abspath(plan["root"])
        self.subject = os.path.join(self.root, plan["subject"])
        self.pristine = self.subject + PRISTINE_SUFFIX
        self.pattern = plan["fail_pattern"]
        self.baseline = None

    def log(self, msg):
        sys.stdout.write("%s\n" % msg)
        sys.stdout.flush()

    # -- the tree ---------------------------------------------------------

    def snapshot(self):
        """Take the pristine copy, refusing to run over a stray one.

        A leftover `.mutpristine` means a previous run was killed between the copy
        and the restore, so the subject on disk may ALREADY be a mutant. Adopting
        it would make every later arm measure a mutated baseline; overwriting it
        would destroy the only copy of the original. Refuse and say so.
        """
        if os.path.exists(self.pristine):
            raise Abort(
                "a stray %s is already there, so a previous run was killed between "
                "the copy and the restore and the subject on disk may already be a "
                "mutant. Compare the two by hand and remove the backup; this "
                "harness will not guess which is the original."
                % os.path.basename(self.pristine))
        shutil.copyfile(self.subject, self.pristine)

    def restore(self):
        """Put the source back BY COPY. Never `git checkout --` (defect 1)."""
        shutil.copyfile(self.pristine, self.subject)

    def write_mutant(self, arm):
        """Apply one arm, asserting the anchor matched EXACTLY once.

        `count == 1` and not `in`: "missing" and "not unique" are different
        failures and both must fire. A non-unique anchor silently mutates the
        first occurrence and reports about the wrong line.
        """
        with io.open(self.pristine, encoding="utf-8", newline="") as fh:
            original = fh.read()
        n = original.count(arm["old"])
        if n != 1:
            return n
        with io.open(self.subject, "w", encoding="utf-8", newline="") as fh:
            fh.write(original.replace(arm["old"], arm["new"], 1))
        return 1

    # -- the two rebuilds -------------------------------------------------

    def build(self, why, expect_work):
        """Rebuild, and check the build actually DID something.

        `expect_work` is True after a real change to the subject. A build that
        reports no work after the source changed has not seen the change, and
        everything measured afterwards describes the previous binary -- which is
        defect 2 arriving through the build tool rather than through a missing
        call.
        """
        if self.plan["build"] == "none":
            if self.verbose:
                self.log("    build: none (%s)" % self.plan["build_none_reason"])
            return
        status, output = _run(self.plan["build"], self.root)
        if self.verbose:
            self.log("    build (%s): exit=%d" % (why, status))
        if status != 0:
            raise Abort("the build failed while %s.\n%s" % (why, output[-4000:]))
        names = self.plan.get("build_names") or []
        if expect_work and names:
            missing = [n for n in names if not re.search(n, output, re.M)]
            if missing:
                raise Abort(
                    "the build after %s named none of %r in its output, so it did "
                    "not rebuild the affected object and every reading after this "
                    "point describes the PREVIOUS binary.\n%s"
                    % (why, missing, output[-2000:]))

    # -- measurement ------------------------------------------------------

    def measure(self, why):
        status, output = _run(self.plan["run"], self.root)
        fails = _fail_set(self.pattern, output)
        if self.verbose:
            self.log("    run (%s): exit=%d  fails=%s"
                     % (why, status, sorted(fails) or "none"))
        return fails, output

    def control(self):
        """The opening control. A red one ABORTS: no arm may be read against a
        poisoned baseline, because every arm would then report FIRED for free."""
        self.build("the opening control", expect_work=False)
        fails, output = self.measure("opening control")
        if fails:
            raise Abort("the opening control is RED (%s), so no arm can be read "
                        "against it -- every one of them would report FIRED for "
                        "free.\n%s" % (sorted(fails), output[-2000:]))
        self.baseline = fails
        return fails

    def recheck_baseline(self):
        """Re-measure after the restore.

        THIS is what makes the restore-rebuild rule observable. With the
        post-restore rebuild in place the subject's binary is the original again
        and this comes back to the opening control's set. Delete that rebuild and
        the previous arm's mutant is still linked in, so this fires -- which is
        exactly the failing test #1252 clause 2 demands.
        """
        fails, output = self.measure("post-restore baseline")
        if fails != self.baseline:
            raise Abort(
                "the post-restore baseline is %s where the opening control was %s. "
                "The source is back on disk, so the difference is a STALE BINARY: "
                "the restore was not followed by a rebuild, or the rebuild did not "
                "see the change. Everything measured after this point would "
                "describe a mutant on a tree git calls clean.\n%s"
                % (sorted(fails), sorted(self.baseline), output[-2000:]))

    # -- the verdict ------------------------------------------------------

    @staticmethod
    def _verdict(expected, observed):
        """Exact SET comparison -- #1252 defect 4, asserted here and not in prose.

        A superset is not a pass: an arm that reddens its own case AND four
        unrelated ones has shown that something is broken, not that THIS test
        catches THIS defect. That is the whole of "and only those".
        """
        expected = set(expected)
        if observed == expected:
            return FIRED, ""
        if not observed:
            return SURVIVED, "expected %s, nothing reddened" % sorted(expected)
        return SURVIVED, ("expected exactly %s, got %s (missing %s, unexpected %s)"
                          % (sorted(expected), sorted(observed),
                             sorted(expected - observed),
                             sorted(observed - expected)))

    def run_arm(self, arm):
        self.log("  arm: %s" % arm["name"])
        matched = self.write_mutant(arm)
        if matched != 1:
            # Not a pass and not a fail. The mutation was never applied, so there
            # is no verdict to report -- folding this into either one is how a
            # harness reports on a mutation it did not make.
            self.log("    %s: the anchor matched %d times, expected exactly 1"
                     % (ANCHOR, matched))
            return ANCHOR, matched

        try:
            self.build("the mutation", expect_work=True)
            observed, output = self.measure("mutant")
            verdict, detail = self._verdict(arm["expect_fail"], observed)
            self.log("    %s%s" % (verdict, (": " + detail) if detail else ""))
            if verdict == SURVIVED and self.verbose:
                self.log(output[-2000:])
        finally:
            # The restore runs whatever the arm did, INCLUDING on an Abort raised
            # inside it -- a harness that leaves a mutant on disk when it gives up
            # has done more damage than the defect it was looking for.
            self.restore()
            self.build("the restore", expect_work=True)

        self.recheck_baseline()
        return verdict, observed

    def run(self, only=None):
        self.log("subject : %s" % self.plan["subject"])
        self.log("build   : %s" % (self.plan["build"] if self.plan["build"] != "none"
                                   else 'none -- ' + self.plan["build_none_reason"]))
        self.snapshot()
        results = []
        try:
            self.control()
            self.log("control : GREEN")
            for arm in self.plan["arms"]:
                if only and arm["name"] != only:
                    continue
                results.append((arm["name"],) + tuple(self.run_arm(arm)[:1]))
            # The closing control. The opening one says the tree was sound when we
            # started; this one says it still is, which is the only way a run can
            # vouch for the tree it leaves behind.
            fails, _ = self.measure("closing control")
            if fails != self.baseline:
                raise Abort("the closing control is %s where the opening one was "
                            "%s" % (sorted(fails), sorted(self.baseline)))
            self.log("control : GREEN (closing)")
        finally:
            if os.path.exists(self.pristine):
                self.restore()
                os.remove(self.pristine)
        return results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--config", required=True, help="path to the mutation plan")
    ap.add_argument("--arm", help="run only the named arm")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    try:
        plan = _load_plan(args.config)
        results = Harness(plan, args.verbose).run(args.arm)
    except Abort as exc:
        sys.stdout.write("\n%s: %s\n" % (ABORTED, exc))
        return 3
    except OSError as exc:
        sys.stdout.write("\n%s: %s\n" % (ABORTED, exc))
        return 3

    survived = [n for n, v in results if v == SURVIVED]
    unknown = [n for n, v in results if v == ANCHOR]
    sys.stdout.write("\n%d arm(s): %d fired, %d survived, %d anchor-not-matched\n"
                     % (len(results), len(results) - len(survived) - len(unknown),
                        len(survived), len(unknown)))
    if unknown:
        # A distinct status, because "we could not tell" must not be reported with
        # the same number as "the test is fine".
        sys.stdout.write("anchor did not match: %s\n" % ", ".join(unknown))
        return 4
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
