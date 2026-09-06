#!/usr/bin/env bash
# Qualify a flake by keeping a TALLY, because the natural tool does not.
#
#   scripts/flake-rate.sh --runs 200 -- ctest --preset clang-debug -R SomeTest
#   scripts/flake-rate.sh --until-fail --runs 50 -- ./out/build/clang-debug/target/FastCacheTest "[flaky]"
#
# ## Why this exists
#
# `ctest --repeat until-fail:N` reports the LAST iteration's result, so a test
# that fails a small fraction of the time is reported `100% tests passed` whenever
# the final run happens to pass. Measured: a ~1% flake survived SIX consecutive
# full-suite runs qualified that way, every one printing `100% tests passed`, and
# a loop that counted every outcome found it on the first try -- `ran=200 pass=198
# fail=2` (#735).
#
# It is the `| tail` trap with a loop counter instead of a pipe: the wrong reading
# and the right one agree on every input EXCEPT a flake, which is precisely what
# you were using it to look for. And it is self-concealing in the worst direction
# -- a tool used to HUNT flakes that reports the last iteration will, on average,
# report success in proportion to how rare the flake is, so the rarer the defect
# the more confident the false clean.
#
# ## Two things this keeps
#
#   * `--until-fail` still stops at the first failure. That half of ctest's mode
#     is useful and the complaint was never about it: once you have a reproduction
#     there is nothing to learn from the remaining runs.
#   * A claim of stability ALWAYS carries its N. "Did not reproduce" does not
#     disprove a 1% rate, and a tool that printed only a verdict would invite
#     exactly that inference. Every line below states how many runs it is about.
#
# bash 3.2: macOS ships a 2007 /bin/bash.
set -uo pipefail

Runs=100
UntilFail=0
Quiet=0

Usage() {
    cat >&2 <<'USAGE'
usage: flake-rate.sh [--runs N] [--until-fail] [--quiet] -- <command> [args...]

  --runs N      how many times to run the command (default 100)
  --until-fail  stop at the first failure; a full pass still reports its N
  --quiet       suppress the command's own output
USAGE
}

if [ "${1:-}" = "--self-test" ]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    cases=0
    Expect() {  # $1 label, $2 expected substring, $3.. command
        local label="$1" want="$2"; shift 2
        cases=$((cases + 1))
        local out; out=$("$@" 2>&1 || true)
        case "$out" in
            *"$want"*) echo "  ok: $label" ;;
            *) echo "  FAIL: $label -- wanted [$want] in:"; printf '%s\n' "$out"; exit 1 ;;
        esac
    }

    # A command that fails on run 2 of every 5, so a tally and a last-read DISAGREE.
    printf '#!/usr/bin/env bash\nn=$(cat "%s/n" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "%s/n"\n[ $((n %% 5)) -eq 2 ] && exit 1 || exit 0\n' "$tmp" "$tmp" > "$tmp/flaky"
    chmod +x "$tmp/flaky"

    rm -f "$tmp/n"
    Expect "a flake is counted, not last-read" "ran=10 pass=8 fail=2" \
        bash "$0" --runs 10 --quiet -- "$tmp/flaky"

    rm -f "$tmp/n"
    Expect "--until-fail stops at the first failure" "FAILED at run 2 of 10" \
        bash "$0" --until-fail --runs 10 --quiet -- "$tmp/flaky"

    # And says so: an early stop is not a rate, and a reader must not read it as one.
    rm -f "$tmp/n"
    Expect "an early stop refuses to be read as a rate" "stopped early, so this is not a rate" \
        bash "$0" --until-fail --runs 10 --quiet -- "$tmp/flaky"

    # The clean line carries its N, which is the whole complaint about the tool
    # this replaces.
    Expect "a clean run states how many runs it is about" "no failure in 7 run(s)" \
        bash "$0" --runs 7 --quiet -- true

    Expect "a bad --runs is refused rather than defaulted" "must be a positive integer" \
        bash "$0" --runs zero -- true
    Expect "no command is refused" "no command given" bash "$0" --runs 3 --

    echo "flake-rate self-test: $cases case(s) ran, all as expected"
    exit 0
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --runs) Runs="${2:?--runs needs a number}"; shift 2 ;;
        --runs=*) Runs="${1#--runs=}"; shift ;;
        --until-fail) UntilFail=1; shift ;;
        --quiet) Quiet=1; shift ;;
        --) shift; break ;;
        -h | --help) Usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; Usage; exit 2 ;;
    esac
done

case "$Runs" in
    '' | *[!0-9]*) echo "--runs must be a positive integer, got '${Runs}'" >&2; exit 2 ;;
esac
[ "$Runs" -gt 0 ] || { echo "--runs must be greater than zero" >&2; exit 2; }
[ "$#" -gt 0 ] || { echo "no command given" >&2; Usage; exit 2; }

pass=0
fail=0
ran=0
firstFailure=0

while [ "$ran" -lt "$Runs" ]; do
    ran=$((ran + 1))
    if [ "$Quiet" -eq 1 ]; then
        "$@" >/dev/null 2>&1
    else
        "$@"
    fi
    status=$?
    if [ "$status" -eq 0 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        [ "$firstFailure" -eq 0 ] && firstFailure=$ran
        if [ "$UntilFail" -eq 1 ]; then
            echo "flake-rate: FAILED at run ${ran} of ${Runs} (exit ${status})"
            echo "flake-rate: ran=${ran} pass=${pass} fail=${fail} -- stopped early, so this is not a rate"
            exit 1
        fi
    fi
done

# The N is on every line, including the clean one. That is the whole point.
echo "flake-rate: ran=${ran} pass=${pass} fail=${fail}"
if [ "$fail" -eq 0 ]; then
    echo "flake-rate: no failure in ${ran} run(s) -- which does not disprove a rate below about 1-in-${ran}"
    exit 0
fi
echo "flake-rate: first failure at run ${firstFailure} of ${ran}"
exit 1
