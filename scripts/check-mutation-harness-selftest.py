#!/usr/bin/env python3
"""Drive `scripts/mutation-harness.py` against staged trees, one case per outcome.

A guard nobody has watched refuse is not a guard, and a guard nobody has watched
ACCEPT is not known to work (#1031, which sat for two days failing CLOSED and
unconditionally while reading like a bad branch). Both directions are here.

## The case this file exists for

`stale-binary` is #1252's clause 2, which the ticket calls the whole ticket:
**the restore-rebuild rule needs a test that FAILS when the rebuild is deleted.**
A harness whose rebuild can be removed with nothing noticing has the defect by
construction, and a structural claim ("the call is on line N") is not that test.

It is driven by DELETING the post-restore rebuild from a COPY of the harness with
a plain textual edit, asserting the anchor matched exactly once, and requiring the
copy to ABORT naming the stale binary. Its negative control (`stale-binary-ctl`)
runs the UNMODIFIED harness over the same staged tree and requires a clean pass --
without it, "the harness aborts when the rebuild is deleted" and "the harness
aborts on this tree" are one passing test.

**The edit is made by this file directly and NOT by the harness.** Testing the
mutation harness with the mutation harness would let a defect vouch for itself:
the tool's subject here is tools, and the one arrangement that cannot work is the
tool measuring itself.

## Why the staged subject is not C++

The harness must be exercised over a subject that is COMPILED -- one whose binary
can go stale -- and a real compiler in a hygiene check would make this reachable
only where a full build is already running, which is the population it is not
for (#257's argument for `tidy-sweep.sh`'s canary). So the staged tree carries a
two-line "compiler" that derives a binary from a source, and a "test" that reads
the BINARY and never the source. That is the entire property that matters: a
restore without a rebuild leaves the previous mutant in the artefact the verdict
is read from. Everything else about a compiler is irrelevant to it.

Both are Python invoked through `sys.executable`, so this file spawns no shell and
inherits none of the platform's quoting.

Exit 0 pass, 1 fail, 77 prerequisite missing.
"""

import io
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# The harness under test. `FASTCACHED_MUT_HARNESS` points this at a COPY, which is
# what lets the matrix that proves these cases can fail neuter one guard at a time
# without editing the tracked file. It is not a production switch and it weakens
# nothing: the path is PRINTED on every run, because "the mode under test was not
# the mode in use" is how `check-catch-skip-return-code`'s self-test passed six
# green cases while CI failed. A fixture states which path it exercised.
HARNESS = os.environ.get("FASTCACHED_MUT_HARNESS") or \
    os.path.join(HERE, "mutation-harness.py")

# The staged "compiler": a binary DERIVED from the source. It is the whole reason
# a stale binary is observable at all -- delete the rebuild and this never runs,
# so `bin.txt` keeps the previous arm's mutant while `src.txt` is back to normal.
BUILD_PY = """\
import io, os, sys
src = io.open("src.txt", encoding="utf-8").read()
io.open("bin.txt", "w", encoding="utf-8").write(src)
sys.stdout.write("compiling src.txt -> bin.txt\\n")
"""

# The staged "test": reads the BINARY, never the source.
TEST_PY = """\
import io, sys
text = io.open("bin.txt", encoding="utf-8").read()
for line in text.splitlines():
    name, _, state = line.partition("=")
    if state == "broken":
        sys.stdout.write("FAILED: %s\\n" % name)
sys.exit(0)
"""

# The staged "test" for an INTERPRETED subject: reads the source itself, because
# that is what "re-read from disk every run" means. Nothing derives a binary, so
# there is no artefact that can go stale -- which is why such a subject needs no
# rebuild, and equally why its harness cannot demonstrate that it would rebuild
# one that did. That asymmetry is #1252's trap and it is why `build: "none"` has
# to be declared rather than inferred.
TEST_PY_DIRECT = TEST_PY.replace('"bin.txt"', '"src.txt"')


def _write(path, text):
    """Every staged write goes through here with newline="".

    Without it Python translates \n to \r\n on the way out on Windows, and a
    multi-line anchor then matches nothing -- which this file would report as
    ANCHOR-NOT-MATCHED, i.e. as a finding about the harness rather than about
    its own fixture. It cost the first run of this file exactly that.
    """
    io.open(path, "w", encoding="utf-8", newline="").write(text)


SOURCE_OK = "guard_a=ok\nguard_b=ok\n"
SOURCE_RED = "guard_a=broken\nguard_b=ok\n"

# The post-restore rebuild, verbatim from the harness. Deleting THIS is the
# mutation the whole file is built around.
REBUILD_LINE = '            self.build("the restore", expect_work=True)\n'


def stage(tmp, source=SOURCE_OK, arms=None, build_names=None, build="python"):
    """Write a complete staged tree and return the path to its plan."""
    root = tempfile.mkdtemp(dir=tmp)
    _write(os.path.join(root, "src.txt"), source)
    _write(os.path.join(root, "build.py"), BUILD_PY)
    _write(os.path.join(root, "test.py"),
           TEST_PY_DIRECT if build == "none" else TEST_PY)
    plan = {
        "root": root,
        "subject": "src.txt",
        # Declared, never sniffed. The staged subject is `src.txt`, so no
        # extension-based inference can reach it either way -- which is the point:
        # the harness must be told, and this fixture tells it.
        "subject_kind": "interpreted" if build == "none" else "compiled",
        "build": ([sys.executable, os.path.join(root, "build.py")]
                  if build == "python" else build),
        "run": [sys.executable, os.path.join(root, "test.py")],
        "fail_pattern": r"^FAILED: (\S+)$",
        "arms": arms if arms is not None else [{
            "name": "guard_a is neutered",
            "old": "guard_a=ok",
            "new": "guard_a=broken",
            "expect_fail": ["guard_a"],
        }],
    }
    if build_names is not None:
        plan["build_names"] = build_names
    if build == "none":
        plan["build_none_reason"] = "staged interpreted subject, re-read every run"
    path = os.path.join(root, "plan.json")
    _write(path, json.dumps(plan, indent=2))
    return path


def run_harness(plan, harness=HARNESS):
    p = subprocess.run([sys.executable, harness, "--config", plan],
                       capture_output=True, text=True, errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def neutered_harness(tmp):
    """A copy of the harness with the post-restore rebuild DELETED.

    `count == 1`, never `in`: "the line is gone" and "the line is not unique" are
    different failures, and an edit script that silently matched nothing would
    make this whole case pass while testing the unmodified harness -- a false
    green in the file written to prevent false greens.
    """
    text = io.open(HARNESS, encoding="utf-8", newline="").read()
    n = text.count(REBUILD_LINE)
    if n != 1:
        raise SystemExit(
            "SELFTEST BROKEN: the post-restore rebuild line matched %d times in "
            "%s, expected exactly 1. The harness was edited without this file "
            "being updated, and every stale-binary case below would have "
            "exercised the UNMODIFIED harness while reporting green." % (n, HARNESS))
    path = os.path.join(tmp, "harness-no-restore-rebuild.py")
    io.open(path, "w", encoding="utf-8", newline="").write(text.replace(REBUILD_LINE, "", 1))
    return path


CASES = []


def case(name):
    def deco(fn):
        CASES.append((name, fn))
        return fn
    return deco


@case("arm-fires")
def _(tmp):
    """The ordinary success: the arm reddens exactly the expected set."""
    status, out = run_harness(stage(tmp))
    return (status == 0 and "FIRED" in out and "1 fired" in out,
            "exit=%d\n%s" % (status, out))


@case("arm-survives")
def _(tmp):
    """A mutation nothing catches. The test under mutation is worthless and the
    harness must SAY so rather than passing."""
    plan = stage(tmp, arms=[{
        "name": "a change no test observes",
        "old": "guard_b=ok",
        "new": "guard_b=ok  ",
        "expect_fail": ["guard_b"],
    }])
    status, out = run_harness(plan)
    return (status == 1 and "SURVIVED" in out,
            "exit=%d\n%s" % (status, out))


@case("anchor-not-matched")
def _(tmp):
    """A third outcome, distinct from both verdicts: the mutation was never
    applied, so there is nothing to report about the test."""
    plan = stage(tmp, arms=[{
        "name": "an anchor that is not there",
        "old": "guard_z=ok",
        "new": "guard_z=broken",
        "expect_fail": ["guard_z"],
    }])
    status, out = run_harness(plan)
    return (status == 4 and "ANCHOR-NOT-MATCHED" in out,
            "exit=%d\n%s" % (status, out))


@case("anchor-not-unique")
def _(tmp):
    """The other half of `count == 1`. A duplicated anchor mutates the first
    occurrence and reports about the wrong line, so it is refused too."""
    plan = stage(tmp, source="dup=ok\ndup=ok\n", arms=[{
        "name": "a duplicated anchor",
        "old": "dup=ok",
        "new": "dup=broken",
        "expect_fail": ["dup"],
    }])
    status, out = run_harness(plan)
    return (status == 4 and "matched 2 times" in out,
            "exit=%d\n%s" % (status, out))


@case("control-red")
def _(tmp):
    """A poisoned baseline. Every arm would report FIRED for free, so no arm may
    be read at all."""
    status, out = run_harness(stage(tmp, source=SOURCE_RED))
    return (status == 3 and "opening control is RED" in out,
            "exit=%d\n%s" % (status, out))


@case("exact-set-not-superset")
def _(tmp):
    """"And only those", asserted. An arm reddening its own case AND an unrelated
    one has shown that something is broken, not that THIS test catches THIS
    defect -- so it is SURVIVED, not FIRED."""
    plan = stage(tmp, arms=[{
        "name": "neuters two guards, declares one",
        "old": "guard_a=ok\nguard_b=ok",
        "new": "guard_a=broken\nguard_b=broken",
        "expect_fail": ["guard_a"],
    }])
    status, out = run_harness(plan)
    return (status == 1 and "SURVIVED" in out and "unexpected ['guard_b']" in out,
            "exit=%d\n%s" % (status, out))


@case("build-did-no-work")
def _(tmp):
    """A build that ran and rebuilt nothing. Everything after it describes the
    previous binary -- defect 2 arriving through the build tool rather than
    through a missing call. Observed for real: ninja over DrvFs missing an edit."""
    plan = stage(tmp, build_names=[r"^compiling NOTHING-LIKE-THIS"])
    status, out = run_harness(plan)
    return (status == 3 and "named none of" in out,
            "exit=%d\n%s" % (status, out))


@case("build-required-not-defaulted")
def _(tmp):
    """A plan that omits `build` is an ERROR, and "no build needed" is a positive
    declaration carrying a reason. This is the whole answer to #1252's open
    subject-kind question, so it is asserted rather than described."""
    plan_path = stage(tmp)
    plan = json.load(io.open(plan_path, encoding="utf-8"))
    del plan["build"]
    io.open(plan_path, "w", encoding="utf-8").write(json.dumps(plan))
    status, out = run_harness(plan_path)
    ok_missing = status == 3 and "missing the required key 'build'" in out

    # The missing-reason rule now lives on the INTERPRETED path only: a compiled
    # subject is refused before any reason is read, by the stronger rule that
    # `compiled-subject-cannot-skip-the-build` covers. Left on a compiled plan
    # this would pass on the WRONG refusal -- a case asserting something both
    # branches produce, which is exactly what #355 disqualifies.
    plan["subject_kind"] = "interpreted"
    plan["build"] = "none"          # declared, but with no reason given
    _write(plan_path, json.dumps(plan))
    status2, out2 = run_harness(plan_path)
    ok_noreason = status2 == 3 and "must SAY WHY" in out2
    return (ok_missing and ok_noreason,
            "omitted: exit=%d\n%s\nno reason: exit=%d\n%s"
            % (status, out, status2, out2))


@case("compiled-subject-cannot-skip-the-build")
def _(tmp):
    """The rebuild obligation is STRUCTURAL for a compiled subject.

    `"build": "none"` on a compiled subject is refused outright, and no
    `build_none_reason` buys an exemption -- an obligation to DO something belongs
    in a required input, not in a sentence a reader has to agree with. Without
    this, defect 2 is one plausible-sounding reason string away from returning."""
    plan_path = stage(tmp)
    plan = json.load(io.open(plan_path, encoding="utf-8"))
    plan["build"] = "none"
    plan["build_none_reason"] = "a reason that sounds entirely convincing"
    _write(plan_path, json.dumps(plan))
    status, out = run_harness(plan_path)
    return (status == 3 and "COMPILED subject" in out,
            "exit=%d\n%s" % (status, out))


@case("subject-kind-is-required-and-closed")
def _(tmp):
    """Omitted, and given a third value. Both refused: it decides whether a
    rebuild is mandatory, so there is no default and no other value."""
    plan_path = stage(tmp)
    plan = json.load(io.open(plan_path, encoding="utf-8"))
    del plan["subject_kind"]
    _write(plan_path, json.dumps(plan))
    s1, o1 = run_harness(plan_path)
    plan["subject_kind"] = "probably-compiled"
    _write(plan_path, json.dumps(plan))
    s2, o2 = run_harness(plan_path)
    return (s1 == 3 and "missing the required key 'subject_kind'" in o1
            and s2 == 3 and 'must be "compiled" or "interpreted"' in o2,
            "omitted: exit=%d\n%s\nthird value: exit=%d\n%s" % (s1, o1, s2, o2))


@case("a-compiled-extension-may-not-be-declared-interpreted")
def _(tmp):
    """The one inference kept, and only in the SAFE direction.

    Declaring a `.hpp` interpreted would switch off the rebuild for a subject that
    has a binary. It reads the subject's own PATH and never a payload -- the
    census classifier that read payload text put a shell-subject harness in the
    compiled set because of a `.hpp` inside a string it writes."""
    plan_path = stage(tmp)
    plan = json.load(io.open(plan_path, encoding="utf-8"))
    plan["subject"] = "ReadSlot.hpp"
    plan["subject_kind"] = "interpreted"
    plan["build"] = "none"
    plan["build_none_reason"] = "claims it needs no build"
    _write(plan_path, json.dumps(plan))
    status, out = run_harness(plan_path)
    return (status == 3 and "its extension says it is compiled" in out,
            "exit=%d\n%s" % (status, out))


@case("interpreted-subject-needs-no-build")
def _(tmp):
    """The positive direction of the case above: a declared `"build": "none"`
    with a reason runs normally. Without this, the rule reads as "interpreted
    subjects are refused"."""
    status, out = run_harness(stage(tmp, build="none"))
    return (status == 0 and "FIRED" in out, "exit=%d\n%s" % (status, out))


@case("stale-binary-ctl")
def _(tmp):
    """NEGATIVE CONTROL for the case below, and it is not decoration: without it,
    "aborts when the rebuild is deleted" and "aborts on this tree" are the same
    passing test. Two arms, so the second one is READ against the baseline the
    first one restored -- one arm cannot exhibit a stale binary, because nothing
    is measured after the restore."""
    plan = stage(tmp, arms=[
        {"name": "first", "old": "guard_a=ok", "new": "guard_a=broken",
         "expect_fail": ["guard_a"]},
        {"name": "second", "old": "guard_b=ok", "new": "guard_b=broken",
         "expect_fail": ["guard_b"]},
    ])
    status, out = run_harness(plan)
    return (status == 0 and out.count("FIRED") == 2,
            "exit=%d\n%s" % (status, out))


@case("stale-binary")
def _(tmp):
    """#1252 clause 2, the clause the ticket calls the whole ticket.

    Delete the post-restore rebuild and the harness must NOTICE. The first arm's
    mutant stays in `bin.txt` while `src.txt` is restored, so the second arm's
    baseline is measured against a mutant on a tree that looks clean -- and the
    post-restore re-measurement is what sees it.

    This case FAILING is what a harness with defect 2 looks like."""
    plan = stage(tmp, arms=[
        {"name": "first", "old": "guard_a=ok", "new": "guard_a=broken",
         "expect_fail": ["guard_a"]},
        {"name": "second", "old": "guard_b=ok", "new": "guard_b=broken",
         "expect_fail": ["guard_b"]},
    ])
    status, out = run_harness(plan, neutered_harness(tmp))
    return (status == 3 and "STALE BINARY" in out,
            "exit=%d\n%s" % (status, out))


def main():
    if not os.path.exists(HARNESS):
        sys.stdout.write("SKIP: %s is not there\n" % HARNESS)
        return 77

    sys.stdout.write("harness under test: %s%s\n\n"
                     % (HARNESS, "  (FASTCACHED_MUT_HARNESS override)"
                        if os.environ.get("FASTCACHED_MUT_HARNESS") else ""))
    tmp = tempfile.mkdtemp(prefix="mut-harness-selftest-")
    failures = []
    try:
        for name, fn in CASES:
            try:
                ok, detail = fn(tmp)
            except SystemExit:
                raise
            except Exception as exc:                      # noqa: BLE001
                ok, detail = False, "raised %s: %s" % (type(exc).__name__, exc)
            sys.stdout.write("%-34s %s\n" % (name, "ok" if ok else "FAILED"))
            if not ok:
                failures.append((name, detail))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # State how many cases RAN. A self-test that stops early must not look like
    # one that judged something -- #1252's own siblings have shipped that.
    sys.stdout.write("\n%d case(s) ran, %d failed\n" % (len(CASES), len(failures)))
    for name, detail in failures:
        sys.stdout.write("\n=== %s ===\n%s\n" % (name, detail))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
