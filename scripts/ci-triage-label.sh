#!/usr/bin/env bash
#
# Should `status/needs-triage` be on this issue? (#1563)
#
# THE DEFECT. The label had three writers -- `issue-triage.yml`, two sites in
# `build.yml` and two in `merge-group-report.yml` -- and NO remover. Nothing in the
# repository ever took it off, so it accumulated until it marked 97 of 119 open issues
# and a query for it answered "almost everything", which is the same as answering
# nothing. A label that only ever goes on is not a state, it is a timestamp.
#
# WHAT "TRIAGED" MEANS, and it is the label's own definition rather than a convenience:
# an issue is triaged when it carries BOTH a `type/` label and an `area/` label. A
# remover checking only `type/` would quietly redefine the word -- and `type/` is the
# one CI can derive, so that version would clear the label on exactly the issues nobody
# had looked at.
#
# THE PART THAT IS NOT OBVIOUS: who applied the labels matters. The three auto-filers
# open issues carrying `type/bug area/build status/needs-triage` in one call. By the
# definition above those issues are already "triaged" the instant they are created, so
# a remover keyed on the labels ALONE would strip `status/needs-triage` from every
# CI-filed issue within seconds -- defeating the three writers this change is
# explicitly meant to keep. A CI-filed issue genuinely is untriaged: a machine guessed
# `type/bug` and `area/build` from the fact that a build broke, which is not a person
# having read it.
#
# So the rule is: labels a MACHINE applied do not count as triage. `Actor` is the
# discriminator, and it is a fact only the event carries -- which is why this decision
# is a function of (labels, actorType) and not of the issue alone.
#
# AND WHICH LABEL ARRIVED, for the same reason one step along. The workflow fires on
# every `labeled`, so `priority/high` added to an issue that already carries a type and
# an area would otherwise read as the moment triage completed. The label SET is
# identical before and after such an event, so nothing in it can distinguish them; the
# event has to. So the decision is a function of (event, labels, actorType, labelName),
# and every one of the four is a fact the issue alone does not carry.
#
# The DECISION is what is tested. `--decide` is pure: it opens no network, reads no
# event, and takes everything it needs as arguments. The acquisition around it lives in
# `issue-triage.yml` and is three `gh` calls.
set -o errexit
set -o nounset
set -o pipefail

TriageLabel="status/needs-triage"

# Decide what to do with the label.
#
# @param 1 Event: `opened` or `labeled`.
# @param 2 Comma-separated label names currently on the issue.
# @param 3 The actor's type: `Bot` or `User`.
# @param 4 On `labeled`, the label that was just ADDED
#          (`github.event.label.name`); empty on `opened`.
# @return Prints `add`, `remove` or `none`.
#
# The fourth parameter is REQUIRED and undefaulted, which `set -o nounset` enforces.
# A default would be `""`, and `""` is the spelling of *this event added no label* --
# so a caller that forgot to pass it would get `none` on every `labeled` event, which
# is a working-looking workflow that never removes anything. The obligation here is DO
# SOMETHING rather than SAY WHY, so it is the shell's type system and not a scan.
Decide() {
    local event="$1" labels=",$2," actorType="$3" labelName="$4"
    local hasType=0 hasArea=0 hasTriage=0

    case "$labels" in *,type/*) hasType=1 ;; esac
    case "$labels" in *,area/*) hasArea=1 ;; esac
    case "$labels" in *,"${TriageLabel}",*) hasTriage=1 ;; esac

    # A machine's labels are not triage, whichever event carried them.
    if [ "$actorType" = "Bot" ]; then
        case "$event" in
            opened) [ "$hasTriage" -eq 1 ] && echo "none" || echo "add" ;;
            # Never remove on a bot's labelling: that is the auto-filer adding
            # `type/bug area/build` beside the very label this would take off.
            *) echo "none" ;;
        esac
        return 0
    fi

    # WHICH label arrived decides whether this event is evidence of triage.
    #
    # The workflow fires on EVERY `labeled`, and most labels have nothing to do with
    # triage: `priority/high` on an issue that already carries a type and an area
    # would otherwise be read as the moment triage completed, and the removal would
    # be attributed to whoever happened to touch the issue next. The label set does
    # not distinguish these -- it is the same set before and after -- so the EVENT
    # has to, exactly as the actor does one clause up.
    #
    # Anchored as a PREFIX, so `prototype/spike` is not a type and `subarea/x` is not
    # an area, matching how the label set itself is read.
    if [ "$event" = "labeled" ]; then
        case "$labelName" in
            type/*|area/*) ;;
            *) echo "none"; return 0 ;;
        esac
    fi

    if [ "$hasType" -eq 1 ] && [ "$hasArea" -eq 1 ]; then
        # Triaged. On `opened` that means a human typed it while filing, so the
        # label is never applied rather than applied and undone -- an issue that
        # flickers through `needs-triage` shows up in whatever query ran in between.
        [ "$hasTriage" -eq 1 ] && echo "remove" || echo "none"
        return 0
    fi

    case "$event" in
        opened) [ "$hasTriage" -eq 1 ] && echo "none" || echo "add" ;;
        *)      echo "none" ;;
    esac
}

SelfTest() {
    local ran=0 failed=0 got
    Expect() {
        local what="$1" want="$2"; shift 2
        ran=$(( ran + 1 ))
        got="$(Decide "$@")"
        if [ "$got" = "$want" ]; then
            echo "ok: ${what} -> ${got}"
        else
            echo "TRIAGE LABEL SELF-TEST FAILED: ${what}: expected '${want}', got '${got}'" >&2
            failed=$(( failed + 1 ))
        fi
    }

    # --- opened -----------------------------------------------------------
    Expect "a bare issue a human filed" add opened "" User ""
    Expect "a human filed it with a type only" add opened "type/bug" User ""
    Expect "a human filed it with an area only" add opened "area/ci" User ""
    # The reproduction case from the ticket: filing with both still got stamped.
    Expect "a human filed it already triaged" none opened "type/bug,area/ci" User ""
    Expect "already triaged AND already stamped" remove opened "type/bug,area/ci,status/needs-triage" User ""

    # --- the auto-filers, which must keep their label ---------------------
    #
    # This is the case that makes the actor a parameter at all. By labels alone
    # these are indistinguishable from a human's triaged issue.
    Expect "CI filed it with type, area and the label" none opened "type/bug,area/build,status/needs-triage" Bot ""
    Expect "CI labelled it afterwards" none labeled "type/bug,area/build,status/needs-triage" Bot "area/build"
    Expect "CI filed it without the label" add opened "type/bug,area/build" Bot ""

    # --- labeled ----------------------------------------------------------
    Expect "a human completed the triage" remove labeled "type/bug,area/ci,status/needs-triage" User "area/ci"
    Expect "a human added only a type" none labeled "type/bug,status/needs-triage" User "type/bug"
    Expect "a human added only an area" none labeled "area/ci,status/needs-triage" User "area/ci"
    # Nothing to remove is not the same as removing nothing; it must not call gh.
    Expect "triaged but never stamped" none labeled "type/bug,area/ci" User "area/ci"
    Expect "a label that is neither" none labeled "status/blocked" User "status/blocked"

    # --- WHICH label arrived ----------------------------------------------
    #
    # Every case in this group holds the SAME label set -- triaged and stamped --
    # and differs only in the label the event carried. The set cannot tell them
    # apart, which is the whole reason the event has to.
    Triaged="type/bug,area/ci,status/needs-triage"
    Expect "an unrelated label on an already-triaged issue" none labeled "$Triaged" User "priority/high"
    Expect "a status label on an already-triaged issue" none labeled "$Triaged" User "status/blocked"
    # The anchor, both halves. A name merely ENDING in `type/` or `area/` is not
    # one, exactly as in the label-set reading above.
    Expect "the added label merely ends in type/" none labeled "$Triaged" User "prototype/spike"
    Expect "the added label merely ends in area/" none labeled "$Triaged" User "subarea/x"
    # And the two controls, without which every case above passes under a gate that
    # refuses EVERYTHING -- which is the failure mode of a rule written as a filter.
    Expect "the control: a real type label completed it" remove labeled "$Triaged" User "type/bug"
    Expect "the control: a real area label completed it" remove labeled "$Triaged" User "area/ci"

    # --- the substring traps ----------------------------------------------
    #
    # `,type/*` is anchored on the comma so a label merely CONTAINING the text does
    # not satisfy it. Without the anchor `prototype/x` reads as a type.
    # `opened`, because that is the event where the two answers differ: an unanchored
    # match would read `prototype/spike` as a type, call the issue triaged and return
    # `none`. On `labeled` both readings answer `none` and the case proves nothing --
    # which is what the first spelling of this pair did.
    Expect "a label ending in type/ is not a type" add opened "prototype/spike,area/ci" User ""
    Expect "a label ending in area/ is not an area" add opened "type/bug,subarea/x" User ""
    # And the control: with REAL type and area labels the same shape answers `none`,
    # so the two cases above are the anchor working rather than the check refusing
    # everything.
    Expect "the control: real type and area labels" none opened "type/bug,area/ci" User ""

    echo "ci-triage-label --self-test: ${ran} case(s) ran, ${failed} failed"
    [ "$failed" -eq 0 ]
}

Mode="decide"
Event=""
Labels=""
ActorType="User"
# Empty is the honest default HERE and not in `Decide`: on `opened` no label arrived,
# and a caller that omits `--label` on a `labeled` event is describing an event that
# carried none, which the decision reads as "not evidence of triage" and answers
# `none`. The door is allowed a default; the decision is not.
LabelName=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --self-test)  Mode="selftest"; shift ;;
        --event)      Event="$2"; shift 2 ;;
        --labels)     Labels="$2"; shift 2 ;;
        --actor-type) ActorType="$2"; shift 2 ;;
        --label)      LabelName="$2"; shift 2 ;;
        *) echo "usage: $0 --event <opened|labeled> --labels <a,b> --actor-type <User|Bot> [--label <name>] | --self-test" >&2; exit 2 ;;
    esac
done

if [ "$Mode" = "selftest" ]; then
    SelfTest
    exit $?
fi

[ -n "$Event" ] || { echo "ci-triage-label: --event is required" >&2; exit 2; }
Decide "$Event" "$Labels" "$ActorType" "$LabelName"
