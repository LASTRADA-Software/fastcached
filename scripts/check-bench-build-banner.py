#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Watches `fastcache-bench` state the build its figures came from (#1439).

A timing is a quantity under conditions, and the condition that decides whether it is a
cost at all is the build. `fastcache-bench` used to print nothing about that, and a Debug
bench prints a plausible number -- so while reviewing #1420 an MSVC `cl-release` log was
read as "a Debug-build signature", and the only tell was the build directory's name.

`BuildBannerListener.cpp` fixes that with one Catch2 listener. What this check is for is
the part the C++ cases cannot reach: the listener is actually REGISTERED, it reaches the
real stderr, it marks every figure, and the XML document `bench/inproc_bench.py` parses is
still a document.

## Why the streams are asserted SEPARATELY

`bench/inproc_bench.py` parses this binary's STDOUT with ElementTree. Measured on Catch2
3.6.0: under `--reporter xml` the reporter redirects the streams a test case writes, so
`std::cout`/`std::cerr` from inside a case are captured into `<StdOut>`/`<StdErr>` and the
document still parses -- but C stdio and `std::println` are NOT redirected, and a write
from inside a `BENCHMARK` body lands inside an open start tag and breaks the parse. So
"stdout parses" is a weaker property than it looks, and this check asserts NO MIXED
CONTENT: a stray write between elements leaves a document that parses perfectly and
carries text where only elements belong.

It runs the benchmark BODIES, at one sample each, rather than `--skip-benchmarks`, which
runs no body at all -- the hazard is what a body writes.

## One defect, one red

This binary holds the banner's own unit cases too (`[buildbanner]`, run by
`ctest -R bench-build-banner`), and the run below executes them along with everything else.
A failure there is that test's finding: reported here as well it would make one defect two
reds naming neither, so those cases' verdict is left to it and the stream rules are judged
anyway. A failure anywhere ELSE stops this check, because the streams it reads are the
streams those cases write. The exit status is never the verdict either way -- Catch2 spends
it on the failed-assertion count -- so completeness is read from the document parsing at
all, which means reaching its own closing tag.

## The instrument is proved before the subject is read

A clean tree has no mixed content, so on every run this check first hands its detector a
planted document that DOES and requires it to be found, and a clean one and requires it
not to be. A detector that had quietly stopped matching would otherwise report the same
clean line as a correct one. Those two controls are the reason a green run here means
something; if either fails, the check refuses naming ITSELF and says nothing about the
binary.

Usage:
    python3 scripts/check-bench-build-banner.py --binary <path to fastcache-bench>

Exit:
    0   the binary states its build, marks every figure, and its XML is a document
    1   it does not -- every finding is printed
    2   this check could not answer (the instrument's own controls failed)
    77  the binary is not there, so nothing was run: a prerequisite, not a verdict
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from pathlib import Path

#: The banner's first line. `RenderBuildBanner` in `src/apps/fastcache-bench/BuildBanner.cpp`
#: spells it; a rename there is meant to fail here, because this string is the only thing
#: that says the listener ran at all.
BANNER_FIRST_LINE = "fastcache-bench: the build these figures come from"

#: The verdict line, whatever the verdict is. The three leading characters are the emphasis
#: run `MarkerTable` carries per standing, so this pattern matches all three without pinning
#: which build this host produced -- the point of the ticket is that all three are possible.
VERDICT_LINE = re.compile(r"^[-!?]{3} these figures are (.+?) -- ", re.MULTILINE)

#: One line per figure, naming the mean. `[` is part of it: the marker is what tells a
#: reader whether the number beside it may be quoted.
FIGURE_LINE = re.compile(r"^fastcache-bench figure \[[^\]]+\]: .+: mean = ", re.MULTILINE)

#: What the banner must say about Catch2's two columns, because confusing them is half of
#: #1439. Two separate needles rather than a sentence: the wording is allowed to change and
#: the two column names are not.
COLUMN_NEEDLES = ("mean", "est run time")

#: The tag on the banner's own unit cases, which `ctest -R bench-build-banner` runs. Named
#: here so this check can leave their verdict to that test instead of reporting it twice.
BANNER_CASE_TAG = "[buildbanner]"

#: Arguments that run every case in the binary, including the hidden `[!benchmark]` ones,
#: with each benchmark body executed exactly once.
#:
#: `*` rather than no filter: a `[!benchmark]` case is HIDDEN, and Catch2 excludes hidden
#: cases when no test spec is given -- measured, an unfiltered run of this binary executes
#: 2 of its cases and leaves 5 of them, and every line they write, unexercised.
RUN_ARGUMENTS = [
    "*",
    "--reporter",
    "xml",
    "--benchmark-samples",
    "1",
    "--benchmark-no-analysis",
    "--benchmark-warmup-time",
    "0",
]

#: A document carrying mixed content: text sitting where only elements belong, which is what
#: a stray write between two elements produces. The instrument control below requires the
#: detector to find BOTH sites.
PLANTED_MIXED_DOCUMENT = """<?xml version="1.0"?>
<Catch2TestRun name="planted">stray text before an element
  <TestCase name="a case">
    <BenchmarkResults name="a row"><mean value="1"/>stray text after an element
    </BenchmarkResults>
  </TestCase>
</Catch2TestRun>
"""

#: The same document with the two stray writes removed. Required to come back clean, because
#: a detector that flagged everything would satisfy the refusing control on its own.
PLANTED_CLEAN_DOCUMENT = """<?xml version="1.0"?>
<Catch2TestRun name="planted">
  <TestCase name="a case">
    <BenchmarkResults name="a row"><mean value="1"/>
    </BenchmarkResults>
  </TestCase>
</Catch2TestRun>
"""


def mixed_content_sites(root: ElementTree.Element) -> list[str]:
    """Return one description per element holding both child elements and non-blank text.

    That is the definition of mixed content, and it is exactly the shape a stray write
    between two elements leaves behind. An element with text and NO children is ordinary --
    `<StdErr>` is one -- so it is not a site.

    :param root: the parsed document's root element
    :return: a description per site, empty when the document holds none
    """
    sites: list[str] = []
    for element in root.iter():
        children = list(element)
        if not children:
            continue
        if element.text and element.text.strip():
            sites.append(f"<{element.tag}> holds text before its first child: {element.text.strip()[:80]!r}")
        for child in children:
            if child.tail and child.tail.strip():
                sites.append(f"<{element.tag}> holds text after <{child.tag}>: {child.tail.strip()[:80]!r}")
    return sites


def prove_the_detector() -> list[str]:
    """Run the detector against a planted document and a clean one.

    Both directions, because each alone is satisfied by a broken detector: one that matched
    nothing would pass the clean document, and one that matched everything would pass the
    planted one.

    :return: a description per control that did not behave, empty when both did
    """
    failures: list[str] = []

    planted = mixed_content_sites(ElementTree.fromstring(PLANTED_MIXED_DOCUMENT))
    if len(planted) != 2:
        failures.append(
            f"the mixed-content detector found {len(planted)} of the 2 sites planted in this check's own "
            f"fixture, so it cannot be believed about the binary's document: {planted}"
        )

    clean = mixed_content_sites(ElementTree.fromstring(PLANTED_CLEAN_DOCUMENT))
    if clean:
        failures.append(
            f"the mixed-content detector reported {len(clean)} site(s) in a document that has none, so a "
            f"refusal below would say nothing about the binary: {clean}"
        )

    return failures


def check_run(binary: Path) -> list[str]:
    """Run the binary once and report everything about that run that is wrong.

    :param binary: the `fastcache-bench` executable
    :return: a description per finding, empty when the run satisfies every rule
    """
    completed = subprocess.run(
        [str(binary), *RUN_ARGUMENTS], capture_output=True, text=True, check=False
    )

    findings: list[str] = []
    out, err = completed.stdout, completed.stderr

    # --- The document is still a document, which is also the completeness reading ---------
    #
    # Parsed FIRST, and the exit status is never the verdict. Catch2 spends its exit code on
    # the failed-assertion count, so a number there says nothing on its own -- while a
    # document that parses has reached its own closing tag, which is what says the run
    # finished. A crash therefore shows up here, as a document that does not parse.
    try:
        root = ElementTree.fromstring(out)
    except ElementTree.ParseError as error:
        return [
            f"stdout does not parse as XML ({error}), so `bench/inproc_bench.py` cannot read this binary "
            f"and nothing below is a statement about the banner. The binary exited "
            f"{completed.returncode}; its stdout starts {out[:300]!r} and the last of its stderr is "
            f"{err.strip()[-300:]!r}"
        ]

    # A failing case is a finding for the OTHER test, not for this one -- and which one it
    # is decides whether this check may go on. `bench-build-banner` runs the `[buildbanner]`
    # cases and is where a broken renderer is diagnosed; refusing here as well would make
    # one defect two reds naming neither, which is the attribution this repository has paid
    # for before. A failure anywhere ELSE is this check's business, because the streams it
    # judges are the streams those cases write.
    failed = [
        case
        for case in root.findall(".//TestCase")
        if (case.find("OverallResult") is not None
            and case.find("OverallResult").get("success") == "false")
    ]
    elsewhere = [case for case in failed if BANNER_CASE_TAG not in case.get("tags", "")]
    if elsewhere:
        return [
            f"{len(elsewhere)} case(s) outside {BANNER_CASE_TAG} failed, so the streams below are not what "
            f"this run is about: {[case.get('name') for case in elsewhere][:5]}"
        ]
    if failed:
        # Not a finding. Said out loud, because a check that silently tolerated a failing
        # case would be reporting a clean stream over a broken binary with nothing saying so.
        print(
            f"note: {len(failed)} {BANNER_CASE_TAG} case(s) failed in this run. That is "
            f"`bench-build-banner`'s finding, not this check's; the stream rules below were still judged.",
            file=sys.stderr,
        )

    # --- The banner reaches the real stderr, and only that -------------------------------
    if BANNER_FIRST_LINE not in err:
        findings.append(
            f"stderr does not carry the banner. `testRunStarting` runs outside any case, so it reaches the "
            f"real stderr; nothing here says {BANNER_FIRST_LINE!r}, which means the listener is not "
            f"registered in this build. stderr was {err.strip()[:400]!r}"
        )
    if BANNER_FIRST_LINE in out:
        findings.append(
            "stdout carries the banner. `bench/inproc_bench.py` parses stdout as XML, and the banner is "
            "written from outside any test case, so under a capturing reporter it cannot be redirected into "
            "the document -- it must not be on that stream at all."
        )

    verdicts = VERDICT_LINE.findall(err)
    if not verdicts:
        findings.append(
            "stderr carries no verdict line. The banner has to end in what this build makes of its own "
            "figures; without it a reader has the facts and not the conclusion."
        )

    for needle in COLUMN_NEEDLES:
        if needle not in err:
            findings.append(
                f"the banner does not name {needle!r}. #1420's misreading was a COLUMN as much as a build, "
                f"so the banner says which of Catch2's two numbers is the per-operation cost."
            )

    for site in mixed_content_sites(root):
        findings.append(f"the XML document carries mixed content, which a stray write leaves behind: {site}")

    # --- stdout is the reporter's stream, all of it ---------------------------------------
    #
    # The bench-wide half of the rule, and the one that is self-enforcing: `<StdOut>` exists
    # in this document only when a case wrote to stdout, so requiring it to be empty refuses
    # a `std::cout` added to any bench file tomorrow. Without this, "every bench-authored
    # line goes to stderr" would be a sentence in the files that already obey it.
    for element in root.findall(".//StdOut"):
        if element.text and element.text.strip():
            findings.append(
                f"a case wrote to stdout, which belongs to the Catch2 reporter: "
                f"{element.text.strip()[:120]!r}. Every line a bench file writes of its own goes to stderr -- "
                f"stdout is safe only through iostreams AND only under a capturing reporter, while stderr is "
                f"safe for any API, so the rule needs no exception for how a line was written."
            )

    # --- Every figure is marked ----------------------------------------------------------
    benchmarks = root.findall(".//BenchmarkResults")
    if not benchmarks:
        findings.append(
            "the document holds no <BenchmarkResults>, so no benchmark body ran and the figure lines below "
            "are unexercised rather than absent. That is this check measuring nothing."
        )
        return findings

    # `benchmarkEnded` runs INSIDE the case, so under this reporter its lines are captured
    # into that case's <StdErr> rather than reaching the real stderr. Both places are read,
    # because which one they land in is the reporter's business and not this rule's.
    captured = "\n".join(element.text or "" for element in root.findall(".//StdErr"))
    figures = len(FIGURE_LINE.findall(captured)) + len(FIGURE_LINE.findall(err))
    if figures != len(benchmarks):
        findings.append(
            f"{len(benchmarks)} benchmark(s) reported and {figures} figure line(s) were written. Every "
            f"figure carries its build's standing or the marking is decoration: a reader who meets an "
            f"unmarked number has no way to know it is one."
        )

    return findings


def main() -> int:
    """Check the binary named on the command line.

    :return: the process exit status, as the module docstring describes it
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="the fastcache-bench executable to run")
    arguments = parser.parse_args()

    if not arguments.binary.exists():
        print(
            f"SKIP: {arguments.binary} does not exist, so the banner was not checked. Build the "
            f"`fastcache-bench` target (FASTCACHED_BUILD_BENCHMARKS=ON) and run this again.",
            file=sys.stderr,
        )
        return 77

    instrument = prove_the_detector()
    if instrument:
        print("check-bench-build-banner COULD NOT ANSWER -- its own controls failed:", file=sys.stderr)
        for failure in instrument:
            print(f"  * {failure}", file=sys.stderr)
        return 2

    findings = check_run(arguments.binary)
    if findings:
        print(f"check-bench-build-banner FAILED: {len(findings)} finding(s)", file=sys.stderr)
        for finding in findings:
            print(f"  * {finding}", file=sys.stderr)
        print(
            "\nThe rule: fastcache-bench states the build its figures came from, before any case runs, on "
            "stderr, and marks every figure with what that build makes of it. "
            "src/apps/fastcache-bench/BuildBanner.hpp carries the reasoning.",
            file=sys.stderr,
        )
        return 1

    print(
        f"ok: fastcache-bench states its build on stderr, marks every figure, and its XML document parses "
        f"with no mixed content (2 instrument controls passed first)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
