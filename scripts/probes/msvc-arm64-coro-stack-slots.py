#!/usr/bin/env python3
"""List the coroutine resume functions that may read stale stack on MSVC ARM64.

This is the measurement behind the MSVC ARM64 coroutine entry in
`.agent/rules/build-and-toolchain.md` ("Language and ABI pitfalls"). It is committed
so the audit can be re-run rather than re-argued. It is a PROBE, not a check: nothing
runs it, and the entry says why.

The shape, as `HandleMs` had it before #1545, in MSVC 19.44's ARM64 listing:

    add   x8,x23,#0x110        ; the call's result goes to the resume function's STACK
    bl    ParseSetFlags
    ...
    add   sp,sp,#0x4D0         ; epilogue, then a tail jump: the coroutine has suspended
    br    x8
    ...
|$__resumable_state_12|        ; a later ACTIVATION of the resume function starts here
    ...
    ldrb  w10,[x23,#0x114]     ; so this reads a slot nothing in this activation wrote

`x23` is the register the prologue copies `sp` into after `sub sp,sp,#N`, and `x8` is
AArch64's indirect-result register. A local that lives across a suspension belongs in
the coroutine frame, which the resume function reaches through another register.

What it does: every activation of a resume function starts at its first instruction and
jumps through the prologue's state table to a `$__resumable_state_N` label, and ends at a
suspension (an sp restore, then `br`) or a return. It builds that control-flow graph,
jump tables included, and reports a read of a slot some call's result was returned into
(through x8) wherever a path from the function's start reaches the read without writing
those bytes in the same activation.

Usage -- the listings come from `/FAs` with a directory per target, on an ARM64 build:

    target_compile_options(<target> PRIVATE /FAs "/Fa<dir>/<target>/")
    python3 scripts/probes/msvc-arm64-coro-stack-slots.py <dir>
    python3 scripts/probes/msvc-arm64-coro-stack-slots.py --self-test

Exit status: 0 no candidate, 1 candidates listed, 2 nothing was analysed (no listing,
or no resume function in any) -- which is not a clean result, and is not reported as one.
A resume function it cannot model (an unrecognised prologue, a jump table it cannot
resolve) is counted and named on stderr, never silently passed.

What it cannot see, in both directions:
- it looks only at results passed through x8. A value the compiler keeps on the stack
  some other way is not looked for at all, so no candidate is not a proof;
- the result's size is not in the listing, so a read up to `ResultWindow` bytes past the
  slot counts as reading it -- a false candidate when a neighbouring slot is read before
  it is written, which a compiler does not normally do;
- a callee may write through a pointer into the stack area, and the probe treats a taken
  address as writing `AddressTakenWindow` bytes -- which can HIDE a stale read.

Recorded result, cl 19.44.35228 ARM64 `cl-release`, windows-11-arm: see the entry, which
carries the runs and the counts; they are not repeated here.
"""

import re
import sys
from pathlib import Path

PROC = re.compile(r'^\|?(\S+?)\|?\s+PROC\b')
ENDP = re.compile(r'^\s*(?:\|?\S+?\|?\s+)?ENDP\b')
LABEL = re.compile(r'^\|([^|]+)\|\s*$')
INSTRUCTION = re.compile(r'^\t([a-z][a-z0-9.]*)\b\s*(.*)$')
PROLOG_END = 'resumable_prolog_label'
SUB_SP = re.compile(r'^sp,sp,#(0x[0-9A-Fa-f]+|\d+)')
FROM_SP = re.compile(r'^(x\d+|fp),sp$')
X8_FROM = re.compile(r'^x8,(x\d+|fp|sp)(?:,#(0x[0-9A-Fa-f]+|\d+))?$')
ADDRESS_OF = re.compile(r'^x\d+,(x\d+|fp|sp),#(0x[0-9A-Fa-f]+|\d+)$')
MEMORY = re.compile(r'^(.*?)\[(x\d+|fp|sp)(?:,#(-?0x[0-9A-Fa-f]+|-?\d+))?\](!?)')
TARGET = re.compile(r'\|([^|]+)\|\s*$')
SOURCE = re.compile(r'^; (\d+)\s+:\s?(.*)$')

Conditional = {'beq', 'bne', 'bhi', 'bls', 'bgt', 'ble', 'bge', 'blt', 'bhs', 'blo', 'bcs',
               'bcc', 'bmi', 'bpl', 'bvs', 'bvc', 'cbz', 'cbnz', 'tbz', 'tbnz'}
NoSuccessor = {'ret', 'brk', 'svc'}

# Funclets the compiler names after the resume function they belong to.
Funclets = ('?dtor$', '?catch$')

# How far past a slot's start a read still counts as reading the call's result. The
# result's size is not in the listing; 256 bytes covers every aggregate seen so far.
ResultWindow = 256

# A slot whose ADDRESS is taken is treated as written over this many bytes: a callee may
# write through the pointer, and the probe cannot see how far.
AddressTakenWindow = 256


class Unmodelled(Exception):
    """A function this probe cannot model; it is reported, never passed."""


def number(text):
    return int(text, 0) if text else 0


def access_width(op, operands):
    first = operands.split(',')[0].strip()
    pair = op.startswith(('ldp', 'stp'))
    if op in ('ldrb', 'ldrsb', 'ldurb', 'ldursb', 'strb', 'sturb'):
        size = 1
    elif op in ('ldrh', 'ldrsh', 'ldurh', 'ldursh', 'strh', 'sturh'):
        size = 2
    elif op in ('ldrsw', 'ldursw'):
        size = 4
    elif first.startswith('q'):
        size = 16
    elif first.startswith(('w', 's')):
        size = 4
    else:
        size = 8
    return size * (2 if pair else 1)


Directives = {'DCD': 4, 'DCW': 2, 'DCB': 1}


def units_of(body):
    """The instructions, the code labels, and the jump tables, by BYTE address.

    Returns (units, labels, tables): units are (mnemonic, operands, source line, address),
    labels map a name to the index of the instruction after it, and tables are
    (address, element values) for each `|__swt|`, in listing order. A table's elements are
    DCD, DCW or DCB, so an address is counted in bytes, never in lines.
    """
    units = []
    labels = {}
    tables = []
    pending = []
    table = None
    address = 0
    source = '?'
    for line in body:
        m = SOURCE.match(line)
        if m:
            source = m.group(1) + ': ' + m.group(2).strip()
            continue
        m = LABEL.match(line)
        if m:
            if m.group(1) == '__swt':
                table = (address, [])
                tables.append(table)
            else:
                pending.append(m.group(1))
            continue
        words = line.split()
        if words and words[0] in Directives:
            value = int(words[1], 0)
            bits = 8 * Directives[words[0]]
            if table is not None:
                table[1].append(value - (1 << bits) if value >= 1 << (bits - 1) else value)
            address += Directives[words[0]]
            pending = []
            continue
        m = INSTRUCTION.match(line)
        if m:
            for name in pending:
                labels[name] = len(units)
            pending = []
            table = None
            units.append((m.group(1), m.group(2).split(';')[0].strip(), source, address))
            address += 4
    return units, labels, tables


def stack_bases(units, labels):
    """Each register the body reaches the stack area through, with its distance above
    the body's sp.

    Two prologue shapes were seen. A large frame saves registers, sets `fp`, then
    allocates with `sub sp,sp,#N` and copies sp into a scratch register (`mov x23,sp`),
    so `fp` sits N above it. A small one allocates with a pre-indexed `stp fp,lr` and
    sets `fp` equal to sp. Either way sp itself stays put for the whole body.
    """
    end = labels.get('$__' + PROLOG_END)
    if end is None:
        raise Unmodelled('no $__resumable_prolog_label')
    bases = {'sp': 0}
    for mnemonic, operands, _, _ in units[:end]:
        if mnemonic == 'sub':
            m = SUB_SP.match(operands)
            if m:
                for register in [r for r in bases if r != 'sp']:
                    bases[register] += number(m.group(1))
        elif mnemonic == 'mov':
            m = FROM_SP.match(operands)
            if m:
                bases[m.group(1)] = 0
    return bases


def is_sp_restore(mnemonic, operands):
    return (mnemonic == 'add' and operands.startswith('sp,sp,#')) or \
        (mnemonic == 'ldp' and operands.startswith('fp,lr,[sp],#'))


def graph(units, labels):
    """Basic blocks as (start, end) unit ranges, their successors, and the unit indexes
    that belong to an exit sequence (sp restored, the frame no longer this one's)."""
    tables = []
    exits = set()
    leaders = {0}
    for index in labels.values():
        leaders.add(index)
    for i, (mnemonic, _, _, _) in enumerate(units):
        if mnemonic in Conditional or mnemonic in ('b', 'br', 'ret', 'brk', 'svc'):
            leaders.add(i + 1)
    starts = sorted(s for s in leaders if s < len(units))
    blocks = [(s, (starts[n + 1] if n + 1 < len(starts) else len(units))) for n, s in enumerate(starts)]
    block_at = {s: n for n, (s, _) in enumerate(blocks)}
    swt_uses = 0
    successors = []

    def block_named(operands):
        target = TARGET.search(operands)
        if not target or labels.get(target.group(1)) not in block_at:
            raise Unmodelled(f'a branch to a label with no instruction after it: {operands}')
        return block_at[labels[target.group(1)]]

    for n, (start, end) in enumerate(blocks):
        mnemonic, operands, _, _ = units[end - 1]
        fall = [block_at[end]] if end in block_at else []
        if mnemonic == 'b':
            successors.append([block_named(operands)])
        elif mnemonic in Conditional:
            successors.append([block_named(operands)] + fall)
        elif mnemonic in ('ret', 'br'):
            restore = next((k for k in range(end - 1, max(start, end - 10) - 1, -1)
                            if is_sp_restore(*units[k][:2])), None)
            if restore is not None:
                exits.update(range(restore, end))
                successors.append([])
                continue
            if mnemonic == 'ret':
                successors.append([])
                continue
            window = range(max(start, end - 8), end)
            if not any(units[k][0] == 'adr' and units[k][1].endswith('__swt') for k in window):
                raise Unmodelled('a `br` that is neither an exit nor a jump table')
            base = next((TARGET.search(units[k][1]).group(1) for k in window
                         if units[k][0] == 'adr' and TARGET.search(units[k][1])), None)
            if base is not None and labels.get(base) not in block_at:
                raise Unmodelled(f'a jump table based at a label with no instruction after it: {base}')
            tables.append((n, base, swt_uses))
            swt_uses += 1
            successors.append(None)
        elif mnemonic in NoSuccessor:
            successors.append([])
        else:
            successors.append(fall)
    return blocks, block_at, successors, exits, tables


def resolve_tables(units, labels, tables, block_at, successors, uses):
    """Point each jump-table `br` at the blocks its table names.

    The k-th `adr ...,__swt` in a function reads the k-th `|__swt|`. An element is a count
    of instructions from the base: a code label the `br`'s block names, or, where it names
    none, the table itself.
    """
    if len(uses) > len(tables):
        raise Unmodelled(f'{len(uses)} jump table(s) used, {len(tables)} laid out')
    by_address = {u[3]: i for i, u in enumerate(units)}
    for block, base, which in uses:
        table_address, values = tables[which]
        if not values:
            raise Unmodelled('a jump table with no elements')
        origin = units[labels[base]][3] if base is not None else table_address
        targets = set()
        for value in values:
            index = by_address.get(origin + 4 * value)
            if index is None or index not in block_at:
                raise Unmodelled(f'a jump table element that lands inside a block ({value})')
            targets.add(block_at[index])
        successors[block] = sorted(targets)


def audit_function(body):
    units, labels, tables = units_of(body)
    bases = stack_bases(units, labels)
    blocks, block_at, successors, exits, uses = graph(units, labels)
    resolve_tables(units, labels, tables, block_at, successors, uses)

    def slot_of(operands, pattern):
        m = pattern.match(operands)
        if not m or m.group(1) not in bases:
            return None
        return bases[m.group(1)] + number(m.group(2) if m.lastindex and m.lastindex >= 2 else None)

    # Where a call's result lands, and which units write it.
    results = {}
    for i, (mnemonic, operands, _, _) in enumerate(units):
        if mnemonic not in ('add', 'mov'):
            continue
        slot = slot_of(operands, X8_FROM)
        if slot is None:
            continue
        for j in range(i + 1, min(i + 8, len(units))):
            if units[j][0] == 'bl':
                results[j] = (slot, units[j][1])
                break
            if units[j][1].startswith('x8,'):
                break
    if not results:
        return []
    watched = [(slot, slot + ResultWindow, callee) for slot, callee in results.values()]

    def transfer(start, end, written, report):
        written = set(written)
        for k in range(start, end):
            if k in exits:
                continue
            mnemonic, operands, source, _ = units[k]
            if k in results:
                slot = results[k][0]
                written.update(range(slot, slot + ResultWindow))
                continue
            if mnemonic == 'add':
                taken = slot_of(operands, ADDRESS_OF)
                if taken is not None:
                    written.update(range(taken, taken + AddressTakenWindow))
                continue
            if not (mnemonic.startswith('ld') or mnemonic.startswith('st')):
                continue
            m = MEMORY.match(operands)
            if not m or m.group(2) not in bases:
                continue
            at = bases[m.group(2)] + number(m.group(3))
            span = set(range(at, at + access_width(mnemonic, m.group(1))))
            if mnemonic.startswith('st'):
                written |= span
                continue
            if report is None or span <= written:
                continue
            for low, high, callee in watched:
                if at < high and at + len(span) > low:
                    report.append((callee, low, at, mnemonic, source))
                    break
            written |= span
        return written

    # Must-written dataflow from the function's first instruction: a byte is written at a
    # point only if every path of this activation reaching it wrote it.
    count = len(blocks)
    everything = None
    into = [everything] * count
    into[0] = set()
    order = list(range(count))
    changed = True
    while changed:
        changed = False
        for n in order:
            if into[n] is None:
                continue
            out = transfer(blocks[n][0], blocks[n][1], into[n], None)
            for successor in successors[n]:
                merged = set(out) if into[successor] is None else into[successor] & out
                if into[successor] is None or merged != into[successor]:
                    into[successor] = merged
                    changed = True
    findings = []
    for n in order:
        if into[n] is not None:
            transfer(blocks[n][0], blocks[n][1], into[n], findings)
    return findings


def audit_listing(text):
    """(resume functions analysed, [(name, why)] not modelled, findings)."""
    analysed = 0
    unmodelled = []
    findings = []
    name = None
    body = []
    for line in text.splitlines():
        m = PROC.match(line)
        if m:
            name, body = m.group(1), []
            continue
        if name is not None and ENDP.match(line):
            if '$_ResumeCoro$' in name and not name.startswith(Funclets):
                try:
                    found = audit_function(body)
                except Unmodelled as why:
                    unmodelled.append((name, str(why)))
                except (KeyError, IndexError, ValueError) as why:
                    # A shape nobody modelled: named as not read, never passed as clean.
                    unmodelled.append((name, f'{type(why).__name__}: {why}'))
                else:
                    analysed += 1
                    findings.extend((name, *f) for f in found)
            name = None
            continue
        if name is not None:
            body.append(line)
    return analysed, unmodelled, findings


def short(symbol):
    return symbol.strip('|').lstrip('?').split('@')[0]


def run(root):
    if not root.exists():
        print(f'audit: {root} does not exist -- NOTHING was analysed', file=sys.stderr)
        return 2
    listings = sorted(root.rglob('*.asm')) if root.is_dir() else [root]
    analysed = candidates = 0
    unmodelled = []
    for listing in listings:
        a, u, found = audit_listing(listing.read_text(encoding='utf-8', errors='replace'))
        analysed += a
        unmodelled.extend((listing.name, short(n), why) for n, why in u)
        for function, callee, slot, at, op, source in found:
            candidates += 1
            print(f'CANDIDATE {listing.name} {short(function)}: {short(callee)} returned into '
                  f'[sp,#{slot:#x}], read by {op} at [sp,#{at:#x}] unwritten in this activation -- line {source}')
    print(f'audit: {len(listings)} listing(s), {analysed} resume function(s) modelled, '
          f'{len(unmodelled)} not, {candidates} candidate read(s)')
    for listing, name, why in unmodelled:
        print(f'audit: NOT MODELLED {listing}: {name} -- {why}', file=sys.stderr)
    if analysed == 0:
        print('audit: NOTHING was analysed -- not a clean result', file=sys.stderr)
        return 2
    return 1 if candidates else 0


# The shapes, cut down from the real listings. A large frame whose dispatch jumps to one
# of two states: HandleMs before #1545, and the same call copied into the coroutine frame
# (x19) before the suspension. And a small frame, whose stack area is reached through fp,
# with its early-exit branch laid out AFTER a suspension -- HandleMg's layout, which a
# reading in listing order took for a stale read.
SelfTestStale = """|?F$_ResumeCoro$1@@Z| PROC
\tstp         fp,lr,[sp,#-0x40]!
\tmov         fp,sp
\tsub         sp,sp,#0x4D0
\tmov         x23,sp
\tmov         x19,x0
|$__resumable_prolog_label|
\tldrsh       w8,[x19,#0x78]
\tadr         x9,__swt
\tldrsw       x8,[x9,w8 uxtw #2]
\tadr         x9,|$LN9@F|
\tadd         x8,x9,x8,lsl #2
\tbr          x8
|$__resumable_state_2|
; 596  :         auto const f = ParseSetFlags(flagTokens);
\tadd         x8,x23,#0x110
\tbl          |?ParseSetFlags@@Z|
\tadd         sp,sp,#0x4D0
\tldp         fp,lr,[sp],#0x40
\tbr          x8
|$LN9@F|
|$__resumable_state_12|
; 626  :         if (f.markStale)
\tldrb        w8,[x23,#0x148]
\tret
|__swt|
\tDCD         0xfffffffb
\tDCD         0x0
\tENDP  ; |?F$_ResumeCoro$1@@Z|
"""

SelfTestCopied = SelfTestStale.replace(
    '\tbl          |?ParseSetFlags@@Z|\n',
    '\tbl          |?ParseSetFlags@@Z|\n\tldp         q17,q16,[x23,#0x110]\n\tstp         q17,q16,[x19,#0xB8]\n',
).replace('\tldrb        w8,[x23,#0x148]\n', '\tldrb        w8,[x19,#0xF0]\n').replace(
    # Two more instructions before the table's base: the first state is two further back.
    '0xfffffffb', '0xfffffff9')

SelfTestLaidOutAfter = """|?G$_ResumeCoro$1@@Z| PROC
\tstp         x19,x20,[sp,#-0x20]!
\tstp         fp,lr,[sp,#-0xB0]!
\tmov         fp,sp
\tmov         x19,x0
|$__resumable_prolog_label|
; 10   :         auto const v = Make();
\tadd         x8,fp,#0x40
\tbl          |?Make@@Z|
\tcbz         x0,|$LN5@G|
\tldp         fp,lr,[sp],#0xB0
\tldp         x19,x20,[sp],#0x20
\tbr          x8
|$LN5@G|
; 12   :         if (v.quiet)
\tldrb        w8,[sp,#0x45]
\tret
\tENDP  ; |?G$_ResumeCoro$1@@Z|
"""

SelfTestUnresolved = SelfTestLaidOutAfter.replace('\tcbz         x0,|$LN5@G|\n', '\tbr          x9\n')


def self_test():
    cases = [
        ('a result read in a later activation is a candidate', SelfTestStale, 1),
        ('the same result copied into the frame first is not', SelfTestCopied, 0),
        ('a block laid out after a suspension but reached before it is not', SelfTestLaidOutAfter, 0),
        ('a `br` it cannot resolve is NOT MODELLED, never passed', SelfTestUnresolved, 'unmodelled'),
        ('a listing with no resume function is NOTHING, not clean', 'nothing here\n', None),
    ]
    failed = 0
    for name, text, want in cases:
        analysed, unmodelled, findings = audit_listing(text)
        got = 'unmodelled' if unmodelled else (None if analysed == 0 else len(findings))
        ok = got == want
        failed += not ok
        print(f'{"ok  " if ok else "FAIL"} {name} (want {want}, got {got})')
    print(f'self-test: {len(cases)} case(s), {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--self-test':
        sys.exit(self_test())
    if len(sys.argv) != 2:
        print('usage: msvc-arm64-coro-stack-slots.py <listing-or-directory> | --self-test', file=sys.stderr)
        sys.exit(2)
    sys.exit(run(Path(sys.argv[1])))
