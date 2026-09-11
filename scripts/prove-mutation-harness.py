#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
"""Prove every case in check-mutation-harness-selftest.py can FAIL, and that each
failure is THAT case and ONLY that case.

Deliberately NOT written with mutation-harness.py. Using the harness to prove the
harness would let a defect vouch for itself, and the subject here is the harness.
This is a plain matrix: neuter one guard in a COPY of the harness, run the whole
self-test against that copy, record which cases redden.

Each row states the cases it EXPECTS to redden. A row whose observed set differs
from its expected set is reported -- a mutation reddening MORE than its own case
is as much a finding as one reddening less, because it means the cases are not
independent and a future failure cannot be attributed.
"""
import io, os, re, shutil, subprocess, sys, tempfile

W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
H = os.path.join(W, "scripts", "mutation-harness.py")
S = os.path.join(W, "scripts", "check-mutation-harness-selftest.py")

MUTATIONS = [
    # Predicted {"stale-binary-ctl"} and that was WRONG, recorded rather than
    # tuned away: `neutered_harness` asserts its anchor matches exactly once, so
    # with the line already gone the self-test REFUSES TO RUN and says the harness
    # was edited without it being updated. That is the better outcome -- a case
    # silently exercising the unmodified harness is the false green this file
    # exists to prevent -- but it is not the one I wrote down first.
    ("post-restore rebuild deleted",
     '            self.build("the restore", expect_work=True)\n', "",
     "REFUSE"),

    ("verdict accepts a SUPERSET (defect 4)",
     "        if observed == expected:", "        if observed >= expected and observed:",
     {"exact-set-not-superset"}),

    ("anchor check accepts a non-unique match",
     "        if n != 1:", "        if n < 1:",
     {"anchor-not-unique"}),

    # The first spelling of this mutation commented out a `raise` and left its
    # continuation lines orphaned, so the harness did not PARSE and all eleven
    # cases reddened -- which reads as "the cases are not independent" and is in
    # fact the mutation not doing what it said. Hence the compile guard below: a
    # mutant that does not parse is a broken mutation, never a finding.
    ("a red opening control no longer aborts",
     '        if fails:\n            raise Abort("the opening control is RED',
     '        if False:\n            raise Abort("the opening control is RED',
     {"control-red"}),

    ("the build-did-no-work check is dropped",
     "            missing = [n for n in names if not re.search(n, output, re.M)]",
     "            missing = []  # [n for n in names if not re.search(n, output, re.M)]",
     {"build-did-no-work"}),

    ("`build: none` no longer needs a reason",
     "        if not plan.get(\"build_none_reason\"):",
     "        if False:",
     {"build-required-not-defaulted"}),

    # Anchor repointed after `subject_kind` joined this tuple. It was caught by
    # the matrix's own `count == 1` assertion reporting SETUP BROKEN rather than
    # by review -- which is the assertion doing its job: a stale anchor that
    # matched nothing would otherwise have reported this guard as untested while
    # every other row stayed green.
    ("`build` gains a default (the key becomes optional)",
     '"root", "subject", "subject_kind", "build", "run",',
     '"root", "subject", "subject_kind", "run",',
     {"build-required-not-defaulted"}),

    ("verdict read from the EXIT STATUS (defect 3)",
     "        return fails, output",
     "        return (fails if status else set()), output",
     # `build-did-no-work` was in this set and should not have been: it ABORTS
     # inside the build, before any measurement runs, so where the verdict is
     # read from cannot reach it. My prediction was wrong and the case is more
     # independent than I claimed.
     {"arm-fires", "control-red", "exact-set-not-superset", "stale-binary",
      "stale-binary-ctl", "interpreted-subject-needs-no-build"}),

    # The three structural guards the manager asked for: the rebuild obligation
    # must not be carried by a reason string.
    ("a COMPILED subject may skip the build after all",
     '        if plan["subject_kind"] == "compiled":\n            raise Abort(',
     '        if False:\n            raise Abort(',
     {"compiled-subject-cannot-skip-the-build"}),

    ("subject_kind is no longer required",
     '    for key in ("root", "subject", "subject_kind", "build", "run",',
     '    for key in ("root", "subject", "build", "run",',
     {"subject-kind-is-required-and-closed"}),

    ("subject_kind accepts any value",
     '    if plan["subject_kind"] not in ("compiled", "interpreted"):',
     '    if False:',
     {"subject-kind-is-required-and-closed"}),

    ("a compiled extension may be declared interpreted",
     '    if (plan["subject_kind"] == "interpreted"\n',
     '    if (False\n',
     {"a-compiled-extension-may-not-be-declared-interpreted"}),

    ("the post-restore baseline is not re-measured",
     "        self.recheck_baseline()\n", "",
     {"stale-binary"}),

    ("restore switched to `git checkout --` (defect 1)",
     "        shutil.copyfile(self.pristine, self.subject)",
     "        subprocess.run(['git','checkout','--',self.subject],cwd=self.root)",
     # Three predicted reds that correctly did NOT happen, each for its own
     # reason: `arm-survives` mutates only trailing whitespace, so a failed
     # restore leaves a behaviourally identical file and the baseline still
     # matches; `build-did-no-work` aborts before the restore is reached; and
     # `stale-binary` runs a copy that ALSO lacks the post-restore rebuild, so it
     # aborts on the stale binary either way. Predicted 7, observed 4, and the
     # four are right.
     {"stale-binary-ctl", "arm-fires", "exact-set-not-superset",
      "interpreted-subject-needs-no-build"}),
]


def cases_failing(harness_path):
    """Run the self-test against a harness copy; return the set of red cases."""
    env = dict(os.environ, FASTCACHED_MUT_HARNESS=harness_path)
    p = subprocess.run([sys.executable, S], capture_output=True, text=True,
                       errors="replace", env=env)
    out = (p.stdout or "") + (p.stderr or "")
    if "case(s) ran" not in out:
        return None, out          # the self-test did not conclude
    return set(re.findall(r"^(\S+)\s+FAILED$", out, re.M)), out


def main():
    base = io.open(H, encoding="utf-8", newline="").read()
    tmp = tempfile.mkdtemp(prefix="prove1252-")

    clean, out = cases_failing(H)
    print("BASELINE (unmutated harness): %s"
          % ("green" if clean == set() else "RED %s" % sorted(clean or [])))
    if clean != set():
        print(out[-3000:])
        return 1

    bad = 0
    for name, old, new, expected in MUTATIONS:
        n = base.count(old)
        if n != 1:
            print("%-52s SETUP BROKEN: anchor matched %d, expected 1" % (name, n))
            bad += 1
            continue
        mutant = base.replace(old, new, 1)
        # A mutant that does not PARSE reddens every case, which reads as a
        # devastating finding about case independence and is nothing of the kind.
        # Cost one such reading before this guard existed.
        try:
            compile(mutant, "<mutant>", "exec")
        except SyntaxError as exc:
            print("%-52s BROKEN MUTATION: does not parse (%s line %s)"
                  % (name, exc.msg, exc.lineno))
            bad += 1
            continue
        path = os.path.join(tmp, "h%d.py" % abs(hash(name)))
        io.open(path, "w", encoding="utf-8", newline="").write(mutant)
        got, out = cases_failing(path)
        if expected == "REFUSE":
            ok = got is None and "SELFTEST BROKEN" in out
            print("%-52s %s (refused to run: %s)"
                  % (name, "ok  " if ok else "DIFF", "yes" if ok else "no"))
            if not ok:
                print(out[-1200:])
                bad += 1
            continue
        if got is None:
            print("%-52s SELFTEST DID NOT CONCLUDE" % name)
            print(out[-1500:])
            bad += 1
            continue
        mark = "ok " if got == expected else "DIFF"
        print("%-52s %s red=%s" % (name, mark, sorted(got) or "NONE"))
        if got != expected:
            print("      expected %s" % sorted(expected))
            print("      missing  %s" % sorted(expected - got))
            print("      extra    %s" % sorted(got - expected))
            bad += 1

    shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d mutation(s), %d disagreeing with their stated expectation"
          % (len(MUTATIONS), bad))
    return 1 if bad else 0


sys.exit(main())
