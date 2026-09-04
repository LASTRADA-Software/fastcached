# Wire formats, protocol handlers and sockets

Rules for `src/FastCache/Protocol/`, `src/FastCache/Net/` and the shared framing
in `src/FastCache/Core/`.

Read this before touching `CompileCacheWire`, `WireFrame`/`WireFields`,
`CompileCacheHandler`, `TcpClient`, `BlockingSocket`/`BlockingConnector`, or any
reactor socket implementation.

Every rule below has already been a bug.

## Framing

- **A compile-cache frame declares its own length, so a rejection can be a reply
  instead of a close.** The pre-1 header was `[magic][op]` with no length, and
  that is what made every refusal — bad magic, unknown opcode, oversize field —
  a silent `co_return`: with no declared length the server could not find where
  the frame it was refusing ended, so it could not answer and resynchronize. A
  client cannot tell that apart from a dead connection, so a mismatched install
  presented as a flaky network and a cache that never warmed. The header is now
  `[magic][version][op][u32 payloadLength]` and every reply is
  `[status][u32 payloadLength][payload]` — uniformly, including a miss, which is
  a zero-length payload rather than no payload. `MemcachedBinary` already proved
  the pattern: it can refuse-and-continue precisely because its header declares
  `totalBodyLen`. Consequences that are each load-bearing: `Miss` is distinct
  from `Error` (both were `0x00`, so a rejected client saw an endlessly cold
  cache); an `UnsupportedVersion` message names the supported *range*, since a
  rejection that cannot say what would have worked cannot be acted on; and there
  is deliberately **no handshake**, because the launcher opens a fresh connection
  per *operation* and a HELLO would cost 2–4 round trips per translation unit on
  the exact path this list already records regressions on.
- **A reply is one frame per request, and `Status::Progress` is the one exception --
  bounded to exactly one verb, carrying nothing, and paid for with a version step.**
  A dispatched compile is bounded by how long a COMPILER runs, so its client's total
  deadline is minutes by construction and was therefore also how long a worker whose
  process had stopped making progress went unnoticed. Keepalive (#247) answers a dead
  HOST and is blind to a kernel that answers every probe while nothing above it does;
  only a periodic *I am still here* separates those, and only the party doing the work
  can send it ([#245](https://github.com/LASTRADA-Software/fastcached/issues/245)).
  Four consequences, each load-bearing:
  - **A reply carries a status byte and a length and NO KIND, so the extensibility the
    request framing has does not exist on the way back.** `DecodeReplyHeader` refuses
    an unknown status outright, and the launcher maps that refusal to a transport
    failure -- so a client built before this meets a pulse by abandoning the compile
    several minutes in, naming nothing. `MinSupportedVersion` therefore moved WITH
    `CurrentVersion` to 3: the older request is refused `UnsupportedVersion`, which
    names the range that would have worked and arrives before a byte of source is
    sent, and the capability becomes implied by the version rather than negotiated.
    "The framing already lets a receiver step over what it does not know" is the
    tempting reason not to bump, and it is a fact about REQUESTS.
  - **Which verbs may be answered with it is a column of `OpTable`**, and
    `ProgressIsCompileOnly()` is `static_assert`ed. Every other verb on this wire is
    answered from a table in microseconds, so a pulse on one would be a frame no client
    could observe and a second reply shape every reader would have to handle for
    nothing -- and a mask widened by hand would give it to them by omission.
  - **It says nothing about the work, and `EncodeProgressReply()` takes no argument so
    there is nothing to pass.** Partial diagnostics here would be a second, racy channel
    for output the result frame already carries whole. A receiver still drains the
    declared length rather than asserting it is zero, for the same reason every frame
    here is drained by its declared length.
  - **A reader loops until a TERMINAL status, so one answer per request survives.**
    `IsTerminalStatus` is in the wire header rather than at each reader, because there
    are four of them in this tree -- the launcher's cache and worker exchanges, the
    admin CLI and the protocol test client -- and a reader that does not ask treats the
    first pulse as the answer. Nothing bounds how MANY arrive: a worker that pulses
    forever is alive and never finishing, which is what the exchange's total budget has
    always been for, and a frame count beside it would be two ceilings on one thing
    that could never be converted into each other.
- **The wire's two grammars are shared, and both live in `Core/` for the same
  reason.** `Core/WireFields` is the payload — a run of `[u32 length][bytes]` —
  and `Core/WireFrame` is the seven bytes in front of it:
  `[magic][version][kind][u32 payloadLength]`. `CompileCacheWire` and `RaftWire`
  had each spelled the second one out, the encoder character-identical and
  `IsSupported` identical outright, which is the drift `CompileCacheWire`'s own
  documentation warns about applied to the half a reader reaches first. What
  `WireFrame` deliberately does **not** decide is what any of it means: the magic
  is the caller's, so two protocols on two ports stay distinguishable; the kind
  byte comes back raw and is validated against the caller's own table, because a
  receiver has to be able to *step over* a frame whose kind it does not know; and
  the supported version range is a parameter, because the wires version
  independently and always will. `RaftWire::FrameHeader` is an alias of
  `WireFrame::Header` while `CompileCacheWire::RequestHeader` is rebuilt from it —
  not an inconsistency but the cost of a rename: that struct's field is spelled
  `opRaw` at some seventy call sites across the daemon, the launcher and the test
  client, and the thing that had to stop being duplicated was the *layout*.
- **`Protocol/CompileCacheWire.hpp` must stay header-only and dependency-free.**
  Same constraint as `Cli/UsageDoc`, same reason: `fastcache-cc` does not link
  the `FastCache` library, so an include of anything from `Net/`, `Cache/`,
  `Async/` or `Config/` there breaks the launcher's **link**, not merely its
  build. Being header-only is also what keeps it free — it costs no row in
  `_fc_cc_core`. The dependency runs *out* of `ProtocolAutodetect.hpp` (which
  pulls in `Task`, `CacheEngine` and `ISocket`, and so can never be included by a
  client) into the wire header, never the other way. The launcher's own framing
  lives in `apps/fastcache-cc/CacheProtocol.cpp` rather than `main.cpp` for a
  related reason: `main.cpp` is in no test target, so while the framing sat there
  it had *no* unit coverage at all.
- **A length a PEER declares sizes nothing until it has been checked against this
  side's own cap — and a codec envelope's `rawLen` is such a length.** The frame
  header's `payloadLength` was checked; the envelope's declared *decompressed*
  size, one layer in, was passed straight to `Compression::Decompress`, which
  **value-initializes** a buffer of exactly that size. So the pages are touched
  rather than lazily reserved, and a `u32` chosen by the sender with no enforced
  relation to the compressed bytes beside it turned a thirty-byte frame into a
  4 GiB allocation. The surface's in-flight byte budget charged only the *frame*
  length, so it passed admission having reserved nothing like what it cost
  ([#241](https://github.com/LASTRADA-Software/fastcached/issues/241)).
  - **Both headers already said so, and neither was implemented.**
    `Core/Compression.hpp` states the precondition — `originalLen` is "the trusted
    expected size taken from the record header", which off a socket it is neither —
    and `Protocol/CompileCacheWire.hpp` goes further: carrying `rawLen` "is what
    lets a decoder reject a payload whose declared expansion exceeds its cap before
    decompressing a byte". A guard a header *promises* is not a guard; grep for the
    decoders before believing the comment.
  - **The cap is the SURFACE's, injected rather than assumed.** A decoder does not
    see the listener that enforced the frame length, so a surface with a smaller
    request cap has to say so or the two disagree about what one request may cost.
    `WorkerProtocol` takes it as a constructor argument, the launcher as a
    `DispatchBudgets` field — a byte budget beside the two time budgets, bounding
    the same thing they do. **And the surface has to actually pass it**: the node
    took the decoder's default and left `WorkerServer`'s own request cap as a second
    literal holding the same number, which is precisely the "two must agree forever"
    shape this whole change exists to remove. `WorkerMaxRequestBytes` is exported
    from `WorkerServer.hpp` and handed over at construction, so lowering the surface's
    cap lowers the decoder's.
  - **Both ends of a payload need the guard, so there is ONE decoder**
    (`apps/fastcache-cc/CodecEnvelope`). The worker opening a request and the launcher
    opening a worker's reply were copies of one function; a guard added to either is
    half a fix, and two guards must then agree forever. The launcher is not the safe
    half: it dialled a worker the *scheduler* named, which is not a worker it trusts
    with its address space.
  - **Sharing that decoder must not cost the payload a copy, and there is no call site
    cheap enough to be the one that pays.** The two callers want different containers
    — a `std::string` a compiler will read, a `std::vector<std::byte>` object file —
    and each spelling that unifies them at the *return type* taxes one of them with a
    full extra allocation and memcpy of a multi-megabyte payload, peak `2N` instead of
    `N`, on the path a developer's build is waiting on. Returning bytes taxes the text
    caller, whose source arrives `Identity` — the only codec a node negotiates — and
    used to be built straight out of the frame. Returning a *generic* container taxes
    the object caller, because `Compression::Decompress` already hands back a
    `vector<std::byte>` sized exactly `rawLength` that can be moved through, and a
    range-constructed generic result copies it. "It is only one call site" is the trap:
    both are the hot one. So the ceiling, the framing check and the `Identity` length
    check — everything two implementations could disagree about — live in one internal
    helper, and `Unenvelope` / `UnenvelopeText` differ only in the container each
    fills, once.
  - **`auto const` on an `expected` silently turns the move back into a copy.**
    `*std::move(x)` on a `const` object is a `T const&&`, which binds to the **copy**
    constructor with no diagnostic at any warning level — a fix that reads as applied
    and is not. The decompressed buffer is held in a non-`const` local for exactly
    this reason.
  - **An envelope refusal's wire code and its message are one fact, so they are one
    table row.** A ternary picking the code beside a separate call picking the text
    answered `unsupported-codec` for a *malformed* envelope while the message said
    "malformed" — a refusal that sends an operator hunting a codec mismatch that never
    happened. This is `RefusalTable`'s rule, one layer in, and it is an `EnumTable`
    guarded by `RowsInEnumeratorOrder` for the same reason that one is.
  - **An `Identity` envelope is checked too.** It takes no decompression path, so it
    never reached `Decompress`'s own length check and could declare any size beside
    any payload. Not an allocation — but a field describing bytes it does not
    describe, and the next receiver to believe it is the next defect.
  - Refusal is `payload-too-large` and a **reply**, never a close: the frame declared
    its own length, so the link is still synchronised and the peer learns which of
    its fields was refused. That is the framing rule at the top of this section,
    applied one layer in.
  - **A per-request ceiling is not a bound on a surface that serves many at once, and
    the guard above is only per request.** `Unenvelope` refuses one envelope declaring
    more than 256 MiB; it says nothing about `slots` of them declaring exactly that.
    The worker's in-flight byte budget was the thing that bounded the surface, and it
    reserved `payloadLength` — the **compressed** frame length — so a hundred-byte
    frame declaring a 256 MiB expansion passed admission having charged a hundred
    bytes. `slots` connections, `slots × 256 MiB` of value-initialized memory: the
    exact shape `MaxInFlightBytes` was introduced to close, reopened one layer in by
    the fix that named the layer. **A ceiling and a budget are different questions,
    and answering the first is not answering the second.**
    - **What a request costs is charged where the budget is, on the accept path**, and
      it is `DeclaredRequestFootprint` — the larger of the frame's own length and what
      its envelope declares it expands to. One counter, one refusal, one `EndpointBusy`:
      a second budget inside the decoder would be two numbers that have to agree about
      one machine's memory, and a job that has to remember to give back two things.
    - **It cannot be charged any earlier, and that is a property of the format.** The
      envelope is a *field of the payload*, so nothing knows the declared expansion
      until the payload — which the frame length already paid for — has been read. The
      reservation is therefore RAISED rather than taken twice: a job holds one amount
      and gives back what it holds.
    - **The larger of the two, not their sum, and the budget bounds the surface at a
      constant multiple of one request rather than byte-exactly.** Byte-exactness was
      never on offer and is not what was lost: by the time the footprint is known, the
      reader's buffer, the payload copy and the assembled frame are already three
      copies of the same bytes. The sum would buy a number that still is not the peak,
      at the price of refusing one honest maximal translation unit on an idle worker —
      which is dividing the per-request cap by the slot count, arrived at from the
      other side. What matters is that the bound does not grow with `slots`.
    - **A price nothing could ever pay is not charged at all.** A footprint above the
      *whole* budget is the per-request ceiling, which `Unenvelope` answers by name
      with `payload-too-large` and without allocating. Reserved for, it comes back
      `EndpointBusy` on a completely idle worker — "ask again shortly" for a frame no
      amount of waiting will fit, which is a client in a retry loop and an operator
      reading a busy signal from an idle machine. `EndpointBusy` is transient or it is
      the wrong word.
    - Refused as a **reply**, like every other refusal here: the frame declared its
      own length and has been read in full, so the link is synchronised and the peer
      learns that memory — not the fleet, and not a slot — is what it waited for.

- **A number a PEER declares sizes nothing until it has been checked against the bytes
  that are actually there.** A declared count or length is a *claim about bytes the
  frame must already contain* — not a request, and not a size to allocate from. Every
  element costs some fixed minimum in the shared field grammar, so the claim is
  checkable, and `WireFields::DeclaredCountFits` is where that check lives
  ([#267](https://github.com/LASTRADA-Software/fastcached/issues/267)). **This is a
  class, not a list of sites** — it has now been found four times, in four unrelated
  formats, and each was written by somebody who knew the rule for the *length* fields
  in the same decoder. `DecodeCompileValue` (~172 GB from a **nine-byte** frame —
  measured, `sizeof(TextRegion)` is 40, and under a 2 GiB address-space cap the
  pre-fix decoder aborts on `std::bad_alloc`), `PrefetchGroupManifest`'s key list,
  `DirectManifest` (~274 GB from thirteen bytes), and `SetCodec` (~137 GB from
  **six**, [#271](https://github.com/LASTRADA-Software/fastcached/issues/271)).
  **Grep for `reserve(`, `resize(` and `assign(` in any decoder you touch, and ask of
  each one where its argument came from.**
  - **A value blob is a peer's bytes too, and that is where the worst one hid.**
    `SetCodec::Decode` reads a set *this daemon wrote* — except that what marks a value
    as a set is its `flags` word, and the memcached text verbs let a client choose it.
    So `set evil 1584398337 0 6` carrying `FC 01 FF FF FF FF`, then `SMEMBERS evil`,
    reserved `0xFFFFFFFF` strings from **six** bytes: no privilege, no fleet
    membership, two stock front ends over one engine, and the `std::bad_alloc` escaped
    the RESP handler uncaught. It is the widest-reaching member of the family and the
    least obvious, because nothing about the decoder says "network".
    **"Only we write these bytes" is a claim to check against every protocol the
    engine serves, never an assumption** — and a shared `CacheEngine` means every
    front end is every other front end's writer.
    - **`Cache/StreamCodec` was the same door, on the clamping shape**
      ([#269](https://github.com/LASTRADA-Software/fastcached/issues/269), fixed).
      `FcTypeStream` (`0x5E700002`) is the second tag a memcached `set` can choose, and
      its **five** declared counts went through `detail::BoundedReserve` —
      `reserve(min(count, remaining))`, the variant this rule's own header argues
      against, because it keeps a provably malformed blob alive and still commits
      `remaining * sizeof(element)`. It carried a second defect the clamp hid: it
      assumed **one byte per element** for all five, where the format's true minimums
      are 20, 8, 36, 4 and 36 — so forty trailing bytes bought forty reservations where
      the grammar permits two and one. A clamp is not merely a weaker refusal; it
      *disguises* a bound nobody checked. `BoundedReserve` was **deleted, not
      tightened**.
  - **It lives in `Core/WireFields.hpp` beside `FieldPrefixSize`**, which is usually the
    answer to its `minBytesEach`, and which that header already argues must stay
    header-only and dependency-free because `fastcache-cc` compiles it in *without
    linking `FastCache`*. A guard under `CompileCache/` was invisible to `Protocol/`,
    `Cluster/DiscoveryWire` and `Consensus/RaftWire` — the decoders that share this
    grammar and will grow the next declared count.
  - **It refuses; it does not clamp.** `reserve(min(count, cap))` keeps a provably
    malformed frame alive and merely makes its allocation smaller, so the decode fails
    later having already committed memory. `Cache/StreamCodec`'s `BoundedReserve` was
    that weaker shape *and* assumed one byte per element, at all five of its counts
    ([#269](https://github.com/LASTRADA-Software/fastcached/issues/269)). Both halves
    mattered: forty trailing bytes bought forty reserved elements where the format can
    hold at most two, and the *entry* minimum is twenty. It is deleted, not tightened
    — a second, weaker mechanism beside the guard is how a decoder ends up reaching for
    the wrong one.
  - **A test that only asserts the refusal does not test this.** All five StreamCodec
    sites returned `false` before the fix too: the clamped reserve happened and *then*
    the loop failed on bytes that were never there. What separates refusing from
    reserving-then-failing is whether the reservation happened, so the case asserts
    `capacity() == 0` on the vectors the caller owns — and it needs trailing bytes to
    do it, because with none the old clamp bounded to zero and looked correct.
  - **The bound is an argument you cannot omit, because remembering it does not scale.**
    Three of these were counts read by hand in a decoder whose author had correctly
    bounds-checked the *length* fields a few lines away — nothing about `ReadU32` says a
    count is different. So `Core/ByteCursor::ReadCount(out, minBytesEach)` obtains a
    count and takes the bound as an argument with no default, which it will never get
    ([#272](https://github.com/LASTRADA-Software/fastcached/issues/272)). Same reasoning
    as putting a payload ceiling in the verb table rather than at each consumer.
    **It is not yet the only way to obtain a count, so do not read this as an
    invariant.** `Cache/StreamCodec` and `Cache/SetCodec` go through it; `CompileValue`,
    `PrefetchGroupManifest` and `DirectManifest` still hand-roll a cursor and guard the
    count beside it. Those three carry the guard already, so the remaining conversion is
    a refactor with no security delta and is tracked separately by
    [#304](https://github.com/LASTRADA-Software/fastcached/issues/304) — deliberately
    not ridden along with a security fix. Until it lands, a new decoder uses
    `ByteCursor`; an old one is not evidence that hand-rolling is still sanctioned.
  - **A per-element minimum is a security bound, not a sizing hint** — the distinction a
    later caller will get wrong. It must be a true *lower* bound or honest data is
    refused, so it always under-estimates the count for typical data; that is what makes
    it correct, not what makes it improvable. Sizing an allocation from it is the mistake
    it invites, and on `SetCodec` a clamp derived that way *raised* peak memory on real
    input while claiming to bound it. Each minimum is pinned to its own encoder by a test
    that encodes one empty element and subtracts.
  - **Divide, never multiply.** `count <= remaining / minBytesEach`, because
    `count * minBytesEach` is the overflowing spelling and wraps on exactly the values
    the check exists to refuse.
  - **Validating the count is necessary and not sufficient.** It bounds the claim by
    bytes present, which still leaves an amplifier whenever the in-memory element is
    bigger than its wire minimum. So a reservation is sized from something this side
    OWNS — the bytes in hand (`remaining / sizeof(Element)`) or its own configured cap
    (`MaxKeysPerGroup`) — never from the peer's number alone. Dropping the reserve
    entirely is the third option and is right only where the count is small anyway.
  - **The per-element minimum is pinned to the ENCODER by a test**, not by a comment
    asking the next author to remember: encode one empty element, subtract, compare.
    A field added to the encoder then fails a test instead of silently weakening the
    guard.
  - **A decoder that cannot say "refused" will be misread by one of its callers.**
    `DecodeKeyList` returned a plain vector, so an empty one meant both "no keys" and
    "these bytes are not a manifest" — harmless on the read path and destructive on the
    write path, where `AddKey` would have replaced a hundred-thousand-key group with a
    one-key list on the strength of bytes it had just failed to understand. It returns
    `optional` now, and both callers answer `Corrupt`.

## Authentication on the compile-cache port

- **Every protocol checks the configured credential, and the compile cache was the
  one that did not.** `session.CurrentAuth()` was consulted by `MemcachedText`,
  `MemcachedBinary` and `RedisResp` — and by nothing in `CompileCacheHandler`. So a
  daemon started with `--requirepass` gated three protocols and served the `0xFC` port
  to anyone who could open a socket, with no flag, log line or doc saying so. On its own
  that is a cache-poisoning surface; it becomes remote code execution the moment that
  port carries anything that *runs* a compiler, which is why it is closed before any
  distribution work rather than after. Consequences that are each load-bearing:
  **which verbs are reachable before a credential is a column of `OpTable`**
  (`OpDescriptor::preAuth`), not a predicate with its own `switch` — it is the
  security-relevant property of the whole verb set, so a reviewer must read it off the
  table, a verb added without a thought about it defaults to closed, and an opcode the
  table does not know is refused rather than waved through. The **gate runs before the
  payload is buffered** and drains with `Skip`, exactly as `MemcachedBinary`'s does:
  checking afterwards would let an unauthenticated peer pipeline frames each declaring
  `maxPayloadBytes` (256 MiB by default) and force that allocation per frame — a
  memory-exhaustion hole opened by the check meant to close a hole. And the
  per-connection state records **only what was verified**, never "is this connection
  allowed through": seeding a flag from the policy at connect time is the obvious
  spelling and is wrong in both directions — a connection opened while auth was off
  stays exempt for life across a `SIGHUP` that turns auth *on*, and nothing then
  distinguishes "auth is off" from "this peer proved something", so enabling auth later
  silently blesses every open connection. Rotation is the deliberate exception the other
  way: a peer that proved the credential current when it connected keeps access when the
  secret changes under it, as redis does, because re-gating on rotation fails every
  in-flight build at the moment an operator rotates.
  - **The gate has exactly one door held open, and that door needs its own lock.**
    `Op::Auth` is `preAuth` by construction, so its payload is read while the peer has
    proved nothing — bounded only by `session.maxPayloadBytes`, i.e. the whole 256 MiB
    the gate exists to deny, reached through the gate. `OpDescriptor::maxPayload` is
    therefore a second column (`MaxAuthPayload`, 4 KiB, for AUTH; `0` = "the session
    cap" for STORE and FETCH, which carry object files and are read only after
    authentication), and `PreAuthVerbsAreBounded()` is `static_assert`ed so a future
    pre-auth verb cannot reopen the hole by omission rather than by decision. The
    refusal names the verb whose ceiling it hit, because "exceeds cap 268435456" tells
    an operator nothing about a 4 KiB limit.
  - **Adding a verb must not break the fleet that does not have it, and that is a
    property of the CLIENT.** `Op::Auth` deliberately did not bump `CurrentVersion` —
    the framing exists so a receiver steps over a verb it does not know — so a daemon
    predating this change answers AUTH `unknown-opcode`, skips it, and serves the
    pipelined command correctly. Returning that refusal as the exchange's outcome, which
    is what a plain "any error is the answer" client does, gives a token-configured
    launcher a permanent **0% hit rate** against every not-yet-upgraded daemon, reported
    as `rejected (unknown-opcode)`: a plausible-looking message with no obvious cause,
    and the exact mixed-fleet case the wire's extensibility was built for. So
    `unknown-opcode` **on AUTH specifically** falls through to the command's own reply;
    every other refusal is about the credential and is still reported. It is not
    silent, though — `CacheOutcome::credentialIgnored` surfaces one note per build,
    because the operator asked for authentication and did not get it, and "the cache
    quietly did less than you told it to" is the failure mode this list exists for.
  - **It costs no round trip, and that is a property of how the client sends rather
    than of the wire.** Authentication is per-connection state and the launcher opens a
    fresh connection per *operation*, so AUTH-then-await-then-command would double the
    round trips of every translation unit — the exact cost the "no handshake" decision
    below exists to avoid. Replies are strictly ordered and one-per-request, so the
    launcher **pipelines**: both frames go out before either reply is read. They are two
    `SendAll` calls, not one concatenated buffer — equally pipelined, since neither waits
    for a reply, but concatenating means copying a STORE frame that carries a whole
    object file, raising peak footprint from about twice the object to three times it on
    the hot path of a parallel build, to buy nothing. The test therefore asserts the
    *write/read interleaving* (`"SSR"`, never `"SRSR"`) rather than a write count: a
    count of one would state the copy instead of the property, and the bytes are
    identical either way so the outcome alone cannot tell the two apart. The client must
    still consume the AUTH reply even when it intends to ignore it; skipping it strands a
    frame and the next command reads the previous one's answer.

- **The code an endpoint refuses an unimplemented verb with is a wire contract, and
  the only tolerated one is `UnknownOpcode`.** `Cc::CacheProtocol::Exchange` steps
  over exactly that code and proceeds unauthenticated — correct against a surface
  with no credential to check — and treats every *other* refusal as being about the
  credential, returning it in place of the answer to the request the caller actually
  sent. So a surface that answers AUTH with `DispatchNotPermitted` gives every
  `FASTCACHE_TOKEN`-configured launcher a permanent 0% hit rate, reported as
  `rejected`, which is indistinguishable from a cache that is merely cold. The
  launcher's own comment had already recorded this for the *daemon* — a
  pre-AUTH `fastcached` answers `unknown-opcode` and serves the pipelined command
  perfectly well, which is the mixed-fleet case the framing's extensibility exists
  for — and the node then reintroduced it on three surfaces by picking a code that
  reads as more accurate. It is not more accurate; it is a different sentence to a
  reader that only parses one. `DispatchNotPermitted` says *this endpoint does not
  do that job*, which is a routing fact a client acts on; `UnknownOpcode` says *I do
  not implement this verb*, which is what an absent capability is. The correction
  belongs in a small `(op, code, why)` table consulted before the generic refusal —
  the shape `CacheProxy::RefusedVerbs` and `CompileCacheHandler::RelocatedVerbs`
  both have — never a special case in the `switch`, because the moment there are two
  answers the next verb must *state* which it is instead of inheriting whichever the
  catch-all happens to give. Three surfaces answering this question in three
  hand-written ways is how they drifted apart to begin with.

  **The code itself is ONE named constant -- `Wire::UnimplementedVerb` -- that every
  surface's table and the client's tolerance spell.** #283 fixed the cache tier
  because that is what its acceptance named; the scheduler and compile ports kept
  answering `DispatchNotPermitted` for another two weeks, so a `FASTCACHE_TOKEN`
  client had every `LEASE` declined behind a green build and a `--requirepass` worker
  never joined the fleet at all (#340). Three tables each naming
  `ErrorCode::UnknownOpcode` would have fixed those two and left the *next* surface
  free to drift again; a constant in `CompileCacheWire.hpp` -- the one header the
  launcher compiles in and the library also uses -- makes every party spell one name,
  which is the strongest form available given they link nothing in common. The struct
  and the lookup are shared for the same reason: four hand-written copies of
  `(op, code, why)` is the answer to "if a sixth case showed up tomorrow, how many
  places would I edit".

  **A wire constant has TWO facts -- its name and its value -- and a symbol shared by
  both ends can only test the first.** This is the trap that "one name every party
  spells" *creates* while closing the drift it was written for, and it is worse than
  what it fixes because it looks green from inside the tree: change the alias
  consistently, and every in-tree assertion still agrees while **every deployed
  launcher breaks**, because the binaries in the field tolerate `0x02` and nothing
  else. Nobody in this repository can recompile the other end of this wire.

  So something must pin the **number**: `static_assert` on the byte, plus at least one
  test asserting the raw enumerator. Those are not redundancy with the shared-name
  assertions -- the raw one is the *anchor*, and the shared-name ones are tautologies
  under a consistent change. Discovered by flipping the alias and **counting** what
  went red: three of five stayed green, including the behavioural contract tests,
  because both ends name the constant. A reviewer read the raw-enumerator assertion as
  the weak one and it was the only one that could fail; the instinct that a literal is
  a code smell is usually right and was exactly wrong here.

  **And the test for it is a behavioural one, not "the code changed".** Asserting the
  enumerator passes the moment somebody edits a constant. What regresses the defect is
  a credentialled client reaching a surface that does not implement `AUTH` and *still
  getting its request answered* -- driven by the bytes the real server produces, so the
  two ends have to actually agree. `AuthRefusalContract_test` does that for the
  scheduler (the node's test target is the only one holding both parties) and
  `WorkerProtocol_test` for the compile port.

## The Net boundary

- **`Net/` is meant to be lifted out of this tree, so what it may include is a
  table and a test rather than an intention.** The constraint was already written
  down -- "`Net` must not depend on `Core`, so `ConnectTcp` takes host and port
  separately" -- and honoured for *new* code, while ten edges that predated it sat
  there untouched (issue #100). That is the shape the constraint will always fail
  in: an include graph drifts in silence. Nothing fails, nothing warns, no test
  goes red, and the edge is discovered by whoever finally attempts the lift.
  `ctest -R net-boundary` is the answer, and four things about it are
  load-bearing:
  - **`Async/` travels WITH `Net/`, and that decision had to come first.**
    `ISocket::Read`/`Write` return `Task<T>`, `IoAwaitable` is the reactor's
    completion hook, and `EpollSocket`/`IocpSocket`/`KqueueSocket` are the
    reactors' own I/O side -- there is no `Net` without the awaitable vocabulary.
    Moving that vocabulary into `Net/` instead is the alternative, and it is worse
    twice over: it leaves `Async/` -- a general coroutine and event-loop library --
    unusable without `Net/`, or it duplicates `Task`.
  - **Three `Core/` leaf headers travel too, each a row with a reason, and the
    check verifies they are still leaves.** `Core/Clock.hpp` (`IClock` and
    `TimePoint`, which every deadline in `Net/` and every timer in `Async/` is
    expressed in -- carrying a second clock interface would fragment the one seam
    the whole codebase injects), `Core/Ranges.hpp` (`FindOrNull`, a toolchain
    shim rather than a domain type) and `Core/Profiling.hpp` (the `FC_ZONE_*`
    macros, which expand to `(void) 0` and carry no code at all). The row is only
    safe while the header depends on nothing, so the check reads each one and
    fails if it has grown a `FastCache/` include: a leaf that quietly gained one
    would drag the whole of `Core/` back across the boundary while still passing.
  - **An edge is closed by moving the file to the layer that owns it, not by
    widening the table.** `Core/Errors/NetError.hpp` became `Net/NetError.hpp` --
    it is `Net`'s own taxonomy and sat in `Core/Errors/` only because that is
    where the taxonomies were shelved. `Net/Framing/LineReader` became
    `Protocol/Framing/LineReader`: it fails with `ProtocolError`, its caps are a
    session's caps, and its own doc lists the protocol handlers as its callers.
    `Net/InheritedListener` became `Platform/InheritedListener`: it reads the
    environment and checks a pid, and handing back an `IListener` does not make
    socket activation a network primitive. In all three the dependency was
    pointing the wrong way round, and moving the file makes `Protocol -> Net` and
    `Platform -> Net` the directions that were always intended.
  - **The check is a scan of the include graph, deliberately, and not a target
    that compiles the set.** Compiling it means a second full build of `Net/` +
    `Async/` in every configuration on every platform, and a staged include root
    copied at configure time goes stale exactly when a header changes -- which is
    the moment the answer matters. The scan reads the same graph the compiler
    would, from the sources, in milliseconds; combined with this project's
    separate rule that public headers are self-contained, a set closed under
    inclusion is a set that compiles standalone. It was verified by running it
    against the tree as it stood before this work, where it names all ten edges.
  - **Test sources are out of scope and that is a decision, not an oversight.**
    What gets lifted is the library. `Net/HealthProbe_test.cpp` drives the
    daemon's own `AdminHttpServer`, which is the entire point of that case;
    gating it would force either a second `AdminHttpServer` fake inside `Net/` or
    the loss of the one test that proves the probe works against the real thing.
    (This is also why the issue's own edge count was high: it counted `_test.cpp`
    files, so `Core/Bytes.hpp` and `Core/Logger.hpp` appeared on the list while
    never being reachable from production `Net/` code at all.)

## Sockets

- **Three implementations of one TCP client, and the rot was in the one nobody
  built.** `Net/BlockingConnector` dialled non-blocking through `getaddrinfo` and
  was coroutine-aware; `fastcache-cc` carried a synchronous `Cc::ITcpClient` with
  its own `SetIoTimeouts` copied from `Net/BlockingSocket` and a comment saying it
  could not be shared; and `compile-cache-testclient` carried a hand-written class
  that was `inet_pton(AF_INET)`-only (so a hostname could never work), unbounded,
  unprotected against SIGPIPE while STOREing whole object files, and **did not
  compile on POSIX at all** (issue #84). The third was invisible because
  `FASTCACHED_BUILD_TESTCLIENT` defaults OFF and no job turned it on, so nothing
  ever discovered that two of its methods named functions that did not exist
  there. `Net/TcpClient` is now the only one. Consequences that are each
  load-bearing:
  - **`Net` must not depend on `Core`, so `ConnectTcp` takes host and port
    separately.** `Net` is meant to be liftable out of this codebase, so it does
    not reach into `Core/HostPort` for a grammar its caller can apply first —
    which also keeps the one parser one parser, since `rfind(':')` picks the wrong
    colon in `[::1]:7000`. The join lives one layer up in `Cc::DialEndpoint`,
    because six call sites across the launcher and the node were otherwise about
    to write it out separately. It refuses a **bare port**, which
    `ParseEndpoint` would accept by supplying a default host: that is right for a
    bind address an operator types and wrong for a dial, where text with no host
    is a misconfiguration and quietly trying loopback turns a typo into a
    connection to whatever happens to be listening locally.
  - **The partial-transfer loops are coroutines because `ISocket::Read`/`Write`
    are awaitables, and synchronous callers drive them with `SyncRun`.** That is
    sound over a blocking socket and nowhere else: such a socket resolves every
    awaitable inline, so the task is never left suspended, which is the one thing
    `SyncRun` refuses to read from. `RaftPeerTransport` already relied on exactly
    this. Over a *reactor* socket the awaitable really does suspend and `SyncRun`
    throws — the defect `cluster-e2e` found the first time consensus was run.
  - **`IConnector::Connect` grew an `ioTimeout`, with no default argument.** A
    dial that succeeds says the peer accepted and nothing about whether it will
    ever answer, and a peer that accepts and then goes quiet parks the calling
    thread forever — which for the launcher turns an optional cache into a
    build-stopping dependency. Bounding it has to happen before the first read, so
    it belongs where the socket is minted rather than in a step every caller has to
    remember. No default, because a default argument on a virtual binds statically
    and would silently differ between a call through the base and one through the
    derived type.
  - **The launcher still does not LINK `FastCache`, and that rule survived
    intact.** The four `Net` rows added to `_fc_cc_core` reach only
    `Net/NetError.hpp`, `Async/Task.hpp` and `Core/Profiling.hpp`, all
    header-only and all std-only, so the launcher stays free of yaml-cpp, OpenSSL
    and the reactor and can still link the CRT statically.
  - **`std::array`'s iterator is a raw pointer on libstdc++ and libc++ and a class
    on MSVC**, so `readability-qualified-auto` asks for `auto const* const` while
    MSVC cannot deduce it — no spelling of `auto` satisfies both. The lookup
    returns the row by value instead of picking a side. Found by building Windows
    immediately after Linux rather than in CI a phase later, the same ordering that
    the `ParsePort` entry above exists to argue for.

- **A daemon that ignores SIGPIPE process-wide hands that decision to every
  program it launches.** `Detail::EnsureNetworkInitialised` did
  `::signal(SIGPIPE, SIG_IGN)` the first time anything touched the network, which
  keeps a broken-pipe write from killing a server and is wrong for any process that
  also spawns a child: an ignored disposition is **inherited across exec**.
  `fastcache-compile-node` links this library, listens on a socket and then runs a
  compiler per job, so it was handing every one of those compilers a disposition
  they never asked for — which is precisely what `fastcache-cc` is documented
  as having to avoid, for the same reason, reached by a different route. Nothing
  in the parent misbehaves, which is why it went unnoticed. Suppression is
  per socket now (`Detail::ArmNoSigPipe`: `SO_NOSIGPIPE` on macOS and the BSDs,
  `MSG_NOSIGNAL` per send elsewhere, process-wide only where neither exists),
  applied at each implementation's single construction funnel. Two things worth
  keeping:
  - **Removing a process-wide safety net exposes every raw sender that was
    leaning on it, and they had to be found by grep rather than by test.**
    `EpollSocket` already passed `MSG_NOSIGNAL` on all three of its sends;
    `KqueueSocket` passed `0` on all three and so needed its descriptor armed;
    `HealthProbe` owns a bare socket and needed the same. `Stats.cpp` writes a
    regular file and the reactors' self-pipes are internal, reachable only through
    a lifetime bug a stray write would already be.
  - **Both regression cases were verified by reintroducing the defect.** Removing
    the send flag terminates the test binary with **signal 13**, exactly as issue
    #68 records for the launcher; restoring the process-wide ignore fails the
    disposition assertion instead. A regression test for a fatal signal that
    cannot be seen to fail is worth nothing.

- **Keepalive is armed per DIAL, and `ApplyHotSocketOptions` is the wrong home for
  it precisely because every socket passes through there.** That function is where
  TCP_NODELAY and the buffer sizes live, and it is reached by every accepted and
  every dialled socket on all four backends — so arming keepalive in it would change
  when an idle memcached or Redis client connection is dropped, and when a Raft peer
  link is torn down, fleet-wide, for a change nobody asked for. Same rule as SIGPIPE
  above, reached from the option side rather than the signal side: per socket, never
  process-wide. `DialOptions::keepAlive` says so at the call, `Detail::ArmKeepAlive`
  applies it, and `SocketAddress_test` asserts the socket is **still not probing**
  after `ApplyHotSocketOptions` — the negative half, which is the assertion that
  fails when somebody later folds the two together for tidiness
  ([#247](https://github.com/LASTRADA-Software/fastcached/issues/247)). Four things
  come with it:
  - **Bare `SO_KEEPALIVE` is worth nothing.** Without the intervals it inherits the
    system default — two hours on Linux — which is longer than any deadline it would
    be protecting, while reading back as armed to anything that checks the flag. The
    intervals go on FIRST and the flag last, so a socket that would not take them is
    never left looking protected.
  - **A parameter that cannot be applied is not taken.** Windows sets idle and
    interval through `SIO_KEEPALIVE_VALS` and fixes the probe count at 10 with no way
    to change it, so detection is ~30 s there against ~16 s on Linux and macOS. Both
    are under the bar the values were chosen for; the asymmetry is stated in
    `KeepAliveSettings` rather than papered over by accepting a count and dropping it.
  - **It answers "is this connection dead", never "is this peer working".** A worker
    that is alive and simply not writing is `Status::Progress` (#245), and keepalive
    does not retire it. Collapsing the two is how a slow peer gets killed and a dead
    one gets waited on.
  - **A per-call option must stay per-call.** Fixing it on the connector would be
    per-socket today only by accident of `Cc::RunOneExchange` building one connector
    per exchange, and would become per-many-sockets, silently, the first time anyone
    reused one. `IConnector.hpp` records a post-connect timeout being moved OUT of the
    per-call surface, which reads like a precedent for the opposite — it is not: the
    reason given there is that `SO_RCVTIMEO` is meaningless to a socket whose reads
    suspend, so it belonged to the blocking connector ALONE. That is a fact about
    *which connector*, not about per-call versus construction.

- **A faster failure nobody can name is not an improvement.** Expiry CLOSES the
  socket, so "this side gave up" and "the peer went away" reach the caller as the same
  broken socket — and `Dispatch` folded them, with `Unreachable`, into one sentence
  because the ACTION is the same (compile locally). The same action is not the same
  diagnosis: "that machine is off" and "that compile took longer than the budget" are
  fixed in different places by different people. So `SocketDeadlineTarget` records
  that the timer fired, `Cc::TransportFailure` names the three states — `Unreached`,
  `PeerLost`, `Expired` — and the cause is asked of the TIMER, never inferred from
  elapsed time. Without it, keepalive turns a five-minute non-answer into a
  sixteen-second one that reads identically, and the only visible effect is that
  builds got faster for no stated reason.

- **The same process hands those children its SOCKETS, and neither platform stops
  it.** A compiler that inherits a client connection holds it open for as long as it
  runs, so the client's peer sees a socket that will not finish closing -- and on a
  node serving `slots` compiles at once, an arriving connection is inherited by every
  compiler spawned while it is open. Three beliefs kept this invisible:
  - `SOCK_CLOEXEC` covers the accept path — it covers the *epoll and kqueue* accept
    paths. `BlockingListener::Accept` uses a plain `::accept()` on both platforms,
    and that is the listener the worker port runs on.
  - Windows has no per-descriptor close-on-exec — it has `HANDLE_FLAG_INHERIT`, and
    `ArmCloseOnExec` was written as a no-op saying so.
  - A Windows socket is not inheritable unless asked for — it arrives **inheritable**;
    `IocpConnector` passes `WSA_FLAG_NO_HANDLE_INHERIT` for exactly that reason, and
    that lone correct site is what the rest drifted from.

  It is armed in `ApplyHotSocketOptions`, which every socket this process owns passes
  through — accepted and dialled, on all four backends — because a rule that has
  already been forgotten at three sites does not get a fourth chance. The test
  asserts the platform default as well as the result: without the *before* half it
  would pass on a platform where the call did nothing.

- **Naming what a child may inherit beats marking what it may not.** The launcher's
  own spawn does the other half: `CreateProcess` with `bInheritHandles = TRUE` hands
  over every inheritable handle in the process, so a sibling's compiler held another
  job's pipe write-end and that job's drain never saw EOF. Windows passes an explicit
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` rather than chasing handles one at a time,
  which also closes whatever the next inheritable handle turns out to be. POSIX has
  no equivalent, so both pipe ends are marked close-on-exec under a lock that also
  covers the spawn — the window between creating a descriptor and marking it is a
  window a sibling can spawn in, and `pipe2` does not exist on macOS.

- **A listening socket claims its address, and the option that says so is spelled
  differently on each platform.** `Detail::BindAndListen` set `SO_REUSEADDR`
  unconditionally, commented "so restart-after-crash rebinds without TIME_WAIT
  delay" -- POSIX reasoning about an option that does not mean the same thing on
  Windows. On POSIX it only lets a bind step over a `TIME_WAIT` left by a **dead**
  socket, and a live listener still holds its address alone; on Windows it lets a
  second socket bind an address a **live** socket already holds, which is the
  documented reason `SO_EXCLUSIVEADDRUSE` exists. So on Windows any process on the
  box -- unprivileged -- could bind the port `fastcached`, a compile node, or
  either one's admin endpoint was already serving, with which of the two answered a
  given connection undefined: for a compile cache reached without a credential that
  is object injection into everybody's build, and for `/metrics` it is a scrape
  surface an attacker can answer (issue #85). It is `ExclusiveBindOption` now --
  one intent, each platform's own spelling. Four things worth keeping:
  - **The `TIME_WAIT` concern the old comment raised is not what was traded away.**
    Measured on Windows 11, across processes: a fresh process rebinds a listening
    port while a connection its crashed predecessor accepted is still in
    `TIME_WAIT`. A listening socket that never accepted does not enter `TIME_WAIT`
    itself, which is what the comment had actually been reasoning about.
  - **Sharing a port on purpose is still opt-in, and it is a different option.**
    `ReusePort::Yes` (`SO_REUSEPORT`, POSIX only) is what lets N reactor threads
    bind one port and have the kernel load-balance across them. Exclusivity is the
    default, not the only setting, and both halves are asserted -- the second bind
    refused, and two `ReusePort::Yes` listeners sharing.
  - **A `setsockopt` carrying a security property is not best-effort.** It fails
    the candidate rather than being ignored the way `TCP_NODELAY` and
    `IPV6_V6ONLY` are: a daemon that silently came up shareable is worse than one
    that visibly did not come up at all.
  - **The discovery beacon's UDP socket keeps `SO_REUSEADDR`, and it is a
    different question rather than the same one answered differently.**
    `OpenUdpSocket` binds the wildcard on the *shared* beacon port so every node
    on the segment hears the broadcast, and claiming that port exclusively would
    let the first process on a host lock every other one out of hearing beacons
    at all. What sharing it does **not** buy is two nodes on one host completing
    the handshake: measured on Windows 11 and on Linux, two sockets on one port
    both receive a broadcast and only one receives a unicast -- and the challenge
    and the proof are both unicast to `received->from` (`DiscoveryService.cpp`).
    That was a defect in discovery rather than in the bind option, and #126 fixed
    it where it lived: a node now **listens** on the shared port and **answers**
    from one only it holds, so sharing is asked for per socket
    (`PortSharing`) rather than being every UDP socket's default. See
    `.agent/rules/consensus-and-cluster.md`.

- **A platform socket error is classified in one place.** `Detail::TranslateError`
  in `BlockingSocket.cpp` mapped ten conditions onto `NetErrorCode`;
  `BlockingConnector` then grew a three-condition copy of it, so `EACCES` — a
  firewall or a privileged port, the two most likely reasons an outbound
  connection is refused *administratively* — came back as an unclassified
  `SystemError`, and a caller matching on `PermissionDenied` never saw it. It is
  now `Detail::TranslateSocketError`, published from `BlockingSocket.hpp` and used
  by both; the connector's own contribution, `ENETUNREACH`/`WSAENETUNREACH`, moved
  into the table rather than being lost, beside `EHOSTUNREACH` because no route
  and no answer are the same fact to a caller: this endpoint is unreachable from
  here, and neither is retryable at this layer.
- **A failure is reported with its reason, and the reason has to be captured where
  it is still in scope.** `ReadWholeFile` in `FileRaftStorage` returned a `bool`,
  so every caller could say no more than `cannot read <path>` — the one thing the
  operator already knew. Each step there fails for a different and actionable
  cause (the path is a directory, the permissions are wrong, the file shrank
  under the read), `std::filesystem` reports through `error_code` and `fopen`
  through `errno`, and a caller handed a `bool` cannot recover either. It returns
  `std::expected<void, ConsensusError>` and translates both at the point of
  failure. A missing file stays *success with nothing read*: a store starting for
  the first time is the ordinary case, not a fault.
- **EOF means "this peer has finished sending", not "this peer is gone" — and a
  server answers what is already determined while abandoning what is still
  pending.** Three places in this tree answered that question and disagreed, so it
  was settled against a running reference rather than by argument
  ([#671](https://github.com/LASTRADA-Software/fastcached/issues/671)).

  **How it was determined, because a citation without its conditions outlives its
  own truth.** `redis-server 7.0.15` on `Linux 5.15.167.4-microsoft-standard-WSL2`,
  stock configuration (`--save "" --appendonly no`), driven over raw sockets. Every
  scenario has a control, so a null result cannot be read as an answer:

  | | observed |
  |---|---|
  | `BLPOP` + half-close, then a push arrives | server closed, **no reply** |
  | `BLPOP`, no half-close, then a push arrives *(control)* | reply delivered |
  | `PING` + immediate half-close | `+PONG` delivered |
  | `BLPOP` + half-close, nothing ever pushed | server closed promptly |
  | `SET`, `GET`, `BLPOP` pipelined, then half-close | `+OK`, `written`, then EOF |
  | blocked clients in the server's own `CLIENT LIST` | **1 before the half-close, 0 after** |
  | same, without the half-close *(control)* | 1, stays |

  The `CLIENT LIST` row is the one to rest on: the drop is visible in the *server's
  own view of its clients*, so it depends on no inference from a client socket —
  which is the failure mode of every probe that reads a close as an absence. The
  pipelined row rules out a race, since two determined replies came back in order
  and only the block was abandoned.

  - **It is neither of the two doctrines this tree held, and both.** *Finished
    sending* for replies already determined — which is `RedisResp`'s reading and
    every `ShutdownWrite` test's — and *gone* for futures still pending, which is
    the compile surface's. That the contradiction resolves without either side
    being simply wrong is the sign the framing was off rather than one of the
    verdicts.

  - **`ArmDisconnect`'s comment asserts the opposite of the reference.** It says of
    a half-closed blocked client that *"abandoning the read there would be wrong"*.
    The measurement is that the reference unblocks exactly such a client and frees
    the connection — deliberately, as its own `CLIENT LIST` shows. A comment
    asserting what nothing checks cannot make the code fail, and this one had been
    contradicting the reference for as long as it had been there.

  - **The compile surface is vindicated by the RULE, not by transfer.** #662 reads
    EOF during a compile as *gone* and skips writing the object. That is not Redis's
    answer carried onto the `0xFC` wire — answers do not transfer between wires, and
    a half-close mid-compile means something different from one mid-`BLPOP`. It is
    the same rule evaluated on a different surface: a compile's reply is not
    determined when the watcher fires, so it is a pending future, and abandoning it
    is what the rule prescribes. State which surface any future measurement covers.

  - **The memcached, RESP-command and admin surfaces are unaffected.** They have no
    blocking verbs, so every reply is determined and *answer what you owe* is what
    their `ShutdownWrite` tests already assert.

  - **A judgement that a real client "would not do that" is not this rule.** The
    reading that got there first was *a real `XREAD BLOCK 0` client keeps its
    connection open, so the two blocking tests are an artifact* — a guess about
    clients, and right by accident. The measurement is sharper and about servers: if
    a client does half-close, **the reference drops it**, so those tests assert a
    reply the reference would never send. Same conclusion, and only one of the two
    can be checked.

  - **`ISocket::ShutdownWrite` exists so the question is askable in production.** It
    did not, and that absence is why the disagreement was unreachable from inside
    the repository. Not for want of VISIBILITY — `InMemorySocket::ShutdownWrite` has
    been public since the MVP commit (`57076ec6`) and some forty tests call it — but
    because it sat on the CONCRETE type. Nothing holding an `ISocket&` could reach
    it, so every consumer was a test by construction, and "make it public" was never
    the missing step: production code never names `InMemorySocket`. A rule nothing can
    express is a rule nothing can be held to. The default is a no-op for fakes; every
    transport this library hands out overrides it.

  - **The error-only reading detected neither way a client leaves**, and
    `ArmDisconnect` now reads the COUNT
    ([#673](https://github.com/LASTRADA-Software/fastcached/issues/673)).
    `RedisResp.cpp`'s *"a full peer close surfaces as the error case"* is false on
    IOCP — a full close arrives as readable, measured while landing #662 — and the
    table above adds that a **half**-close is acted on too, which an error-only rule
    cannot see at all. Two arms, and they answer different questions: an ERROR is an
    abortive close (RST), a count of **`0`** is EOF, and EOF is the ordinary way a
    client goes away. A count `>0` is still not a disconnect — it is a pipelined
    command — so this is not "readable means gone", which would abandon every
    blocking read the instant it parked.

    **A watch is proved by DELETING an arm, never by a passing suite**, because both
    arms set one flag and either alone makes every test green. That is how #673 was
    found (delete the error arm on IOCP: 7 cases, 61 assertions, all still pass — so
    it had never fired) and it is how its fix is checked: delete the EOF arm and
    exactly one case fails, `RESP: XREAD BLOCK 0 is abandoned when the peer closes
    gracefully`, while its control stays green. The control is not optional — without
    a case that must survive an open write side, "detect a graceful close" and
    "abandon everything" are the same passing test.

    And the signal is that the HANDLER RETURNED, not that a reply was empty: a reader
    still parked and one that unwound having written nothing produce identical bytes,
    which is why the in-memory suite could not see this even after
    `InMemorySocket::WaitReadable` made the condition expressible (#677).

  The probes are `scripts/probes/redis-eof-semantics.py`, runnable in about two
  minutes against any `redis-server`. A rule people can re-run is one they stop
  re-litigating.

- **A TLS peer says "I have finished sending" with a RECORD, so the raw socket
  cannot answer that question, and delegating to it was reporting the opposite.**
  A well-behaved peer emits `close_notify` and only THEN the transport FIN, so at
  the instant it goes away there are bytes on the wire: `TlsSocket::WaitReadable`
  deferred to `_raw->WaitReadable()`, which peeked, saw the alert as data and
  answered `>0`. #673's EOF arm therefore declined for every TLS client, on the one
  transport where the graceful close is the ORDINARY close rather than a corner
  ([#712](https://github.com/LASTRADA-Software/fastcached/issues/712)). The record
  is decrypted now, by `SSL_peek`, which removes nothing OpenSSL decoded -- so
  `SSL_pending()` reports the whole record afterwards and the caller's next `Read`
  returns every byte of it. Four things travel with it:
  - **"Consumes nothing" is about bytes the CALLER could have read.** The probe does
    consume raw ciphertext into the decorator's own incoming BIO, and must: that is
    the only way to have an answer. What the contract forbids is losing a byte the
    next `Read` would have returned, which is why the control case reads the
    plaintext back after the probe rather than merely checking a count.
  - **Three answers, and the third is the one nobody writes down.** A decrypted byte
    is `>0`; `SSL_ERROR_ZERO_RETURN` is `0`; and a raw EOF arriving BEFORE a full
    record is `0` too -- a truncated stream is still a peer that has stopped
    sending, and it is also the case the old delegation got right, so a fix pinned
    only on `close_notify` can silently break it.
  - **It writes, like every other TLS read**, which `ShutdownWrite`'s own comment
    already recorded for `Read`: the `WANT_READ` path flushes whatever OpenSSL has
    queued outbound, so a failure there surfaces as a `NetError` from a call a
    plaintext socket answers with a count.
  - **The case that reproduces it is the one that looks redundant.** Over an
    in-memory wire the pre-fix version answers `1` *synchronously* when the wire is
    idle, so a case that arms the wait, closes, and only then checks the count is
    red for the right reason by accident. The assertion that the wait had NOT
    resolved before the peer acted is what separates "parked, then told EOF" from
    "never parked at all". Measured on Linux x86-64 (GCC 15 / libstdc++, OpenSSL
    3.5): 3 of 5 cases fail on the parent commit, `1 == 0`.

## Dialing, and the reactor underneath it

- **A synchronous dial spends a thread the caller does not own, and the argument
  for it reasoned about the wrong thing.** `IConnector::Connect` blocked, and its
  header defended that at length: the caller has nothing to do until the
  connection exists, so a coroutine "would buy the ability to interleave work
  that does not exist", with one rule holding it up -- **a reactor thread never
  calls this**. Three things were wrong with it, and the third had already
  shipped:
  - **The caller has nothing to do; the THREAD has thousands of other
    connections.** The argument described one caller's own work and silently
    ignored whose thread it was spending. That is the same mistake, in the same
    direction, that `Net/PlatformListener.hpp` records for the accept side.
  - **The rule was not free, it was paid for.** `RaftPeerTransport` owned a
    thread per peer *because of this interface*, and it could not be made safe:
    thread-per-peer is only expressible over a blocking socket, because the write
    was driven by `SyncRun` -- which this list already records throwing the
    instant its task really suspends. So the decision pinned one component's
    outbound half to `BlockingSocket` for good while its inbound half was already
    on the reactor: two socket implementations in one class, chosen by direction.
  - **It hid a hang that killed nodes.** The transport passes no I/O timeout,
    deliberately, so over a blocking socket there is no `SO_SNDTIMEO` and a peer
    that accepts and then stops reading parks the sender inside `::send` once the
    buffer fills. `Stop()` cleared the outbox, notified a condition variable the
    sender was not waiting on, and joined it unconditionally -- and the socket was
    a LOCAL of the sender, so nothing else could reach it to close it.
    `~RaftPeerTransport` blocked forever and the node died to SIGKILL, which is
    the `systemctl stop` escalation this list already records once, reached from
    the outbound side.

  And the rule never covered the worst case anyway: `connectTimeout` bounds the
  dial, while **name resolution runs first and is bounded by nothing**, because
  `getaddrinfo` takes no timeout. For `fastcache-cc` that is every translation
  unit in a build waiting on a wedged resolver with no knob anywhere.
  Consequences that are each load-bearing:
  - **`Connect` takes `std::string` by value, not `string_view`.** A coroutine
    frame outlives the call expression, so a view names storage the caller may
    already have destroyed -- the hazard `Net/TcpClient.hpp` records for reference
    parameters, reached by another route. clang-tidy enforces the reference half
    (`cppcoreguidelines-avoid-reference-coroutine-parameters`) and cannot see the
    view half, which is why it is written down. The copy is one the threaded
    resolver needed anyway.
  - **`ioTimeout` left the interface.** It is `SO_RCVTIMEO`, which bounds a
    *blocking* syscall and is inert on a socket whose reads suspend -- so keeping
    it would hand every reactor caller a bound that does not exist, which is worse
    than having none. It survives as `BlockingConnectorOptions::ioTimeout`. A
    reactor caller arms a `DeadlineTimer` that closes the socket instead, which is
    strictly *more* than the option gave: the option bounds one call, so a peer
    dribbling a byte at a time could still take forever.
  - **The budget is divided across candidates, and both halves are needed.**
    Giving every candidate the full timeout means a caller asking for two seconds
    waits four -- a bound that multiplies by however many addresses a name happens
    to have is not a bound. Giving the FIRST candidate all of it defeats the
    fallback whenever that candidate black-holes rather than refuses, which is the
    AAAA-on-a-machine-with-no-IPv6 case trying every candidate exists for. Found
    on Windows, where a closed loopback port is silently *dropped* rather than
    reset: the dead candidate consumed the whole budget and the real one was never
    tried. The test had been passing only because each candidate previously got a
    fresh timeout.
  - **`DialEndpointBlocking` takes a `BlockingConnector&`, never an
    `IConnector&`.** Every remaining `SyncRun` is sound only because the socket
    underneath resolves inline, and the failure when it does not is a
    `std::logic_error` thrown from inside a heartbeat thread. A comment saying so
    is a rule somebody breaks; the type is the rule.
  - **A literal address never reaches a resolver thread.** Every internal dial
    here is to one -- Raft peers, `127.0.0.1:6674`, an endpoint discovery proved --
    and the launcher makes one per translation unit, so a thread hand-off on that
    path would be a real regression. It is also what lets the whole connect path be
    tested without a thread existing. The pool is fixed at two (never one per dial;
    never sized to cores, since this is I/O-bound) and its queue is bounded and
    *refused* rather than waited on, which is the same shape as the pre-auth
    payload cap.

- **Four defects sat between the reactor and a dial that could work, and three of
  them were already latent.**
  - **`EpollReactor` routed only `EPOLLIN` and `EPOLLOUT`, and dropped
    `EPOLLERR`/`EPOLLHUP`.** Those arrive whether or not they were requested, and a
    failed connect can be reported with neither direction set -- so the dial would
    never be told, and because the fd is level-triggered it would be re-reported on
    the very next iteration: a hang AND a loop spinning at 100% CPU, with nothing
    logged at either end. `EpollFdHandler::onError` is where an error goes now, and
    `SelectEpollCallback` is a pure function precisely so the rule is unit-testable
    without a socket or a way to provoke a kernel error.
  - **That same loop read `handler->onWritable` after `onReadable` may have freed
    the object the handler lives in.** It services at most one callback per fd per
    iteration now; level-triggering re-reports whatever was skipped, so the cost is
    one extra turn.
  - **`TestReactor::Submit`/`Schedule` touched bare containers** while `IReactor`
    documents both as callable from any thread. Nothing noticed while every
    producer was the test's own thread -- and every primitive added here crosses
    threads by definition, so a double that cannot be used the way its interface
    reads forces each of those cases onto a real reactor, where nothing is
    deterministic.
  - **`IListener` had no `BoundPort()`**, so only `BlockingListener` could answer
    "which port did I actually get" -- the question every caller binding port 0 has
    to ask, and the one every script-driven test here relies on.

- **The dial's own residuals are recorded rather than dressed up.**
  - **The handler detach in `SettleDial` guards the reactor's loop against a spin,
    NOT the socket built afterwards.** It looks as though it should guard both: the
    socket's constructor attaches the same fd, epoll refuses that with `EEXIST`,
    and the failure is ignored. But `UpdateInterest` uses `EPOLL_CTL_MOD` with a
    fresh `ev.data.ptr`, so the socket's first armed read overwrites the stale
    registration. Verified by removing the detach and watching the byte-transfer
    case still pass. Claiming otherwise would send the next reader looking for a
    bug that is not there.
  - **A loopback connect completes INLINE, so the readiness path is unreachable
    from an ordinary test.** `::connect` returns 0 and the whole
    attach/park/settle block is skipped, which means a dial test that stops at
    "connected" exercises none of it. Provoking it needs a filled accept queue.
  - **On IOCP an accept must be awaited while it is outstanding.**
    `IocpListener::Accept` issues `AcceptEx` immediately but records the awaitable
    only in its suspend callback, so a completion arriving before anyone awaits is
    dropped and the accept never resolves. Arming the accept before a dial and
    awaiting it after -- which reads naturally and works on epoll -- deadlocks.
  - **Both connector tests move BYTES, and arrange the read to park.** `Read` tries
    the syscall before suspending, so a read finding data or EOF already waiting
    would be answered perfectly well by a socket the reactor was never told about.
    Only a read with nothing to return proves the registration exists -- and on
    Windows only a real transfer proves `SO_UPDATE_CONNECT_CONTEXT` was applied,
    without which the socket is connected and every ordinary call on it fails.

- **`ConnectEx` needs two steps `AcceptEx` does not, and neither had precedent
  here.** The socket must be `bind`-ed to the wildcard of its family before the
  call, or it fails with `WSAEINVAL` and names nothing; and
  `SO_UPDATE_CONNECT_CONTEXT` must be applied afterwards, or the handle's context
  stays unset and `getpeername`, `shutdown` and the ordinary calls all fail on a
  socket that is genuinely connected. The extension pointer is cached in a two-row
  table keyed by family, because a connector -- unlike a listener, which has one
  family -- dials whichever the resolver hands it. `IocpSocket` therefore takes an
  `IocpAttachment`: `ConnectEx` requires the port association BEFORE the operation
  is issued, and a second `CreateIoCompletionPort` on an associated handle fails,
  so without it the constructor would report `IsAttached() == false` and tell the
  caller to abandon a connection that works. And **`overlapped.Internal` is an
  NTSTATUS, not a WSA code**: the reactor hands it over as-is, so `0xC0000236`
  (refused) falls through every `WSAE*` row onto `SystemError` -- useless to a
  connector whose job is to tell refused from unreachable. `WSAGetOverlappedResult`
  is the documented conversion. The same wart affects IOCP reads and writes today
  and is left alone deliberately: their `Dispatch` cannot reach the socket handle.

## Socket and coroutine lifetime

- **`Close()` can be the last thing that runs on a socket, so it must touch no member
  after it completes an awaitable.** `EpollSocket::Close` walked `{readOp, writeOp}`
  and completed each parked awaitable inside the loop. Completing one resumes the
  coroutine that was waiting on it -- and a coroutine that OWNS the socket
  (`ServePeer` holds it in a by-value `unique_ptr` parameter) then runs to its end and
  destroys it, so the loop's next iteration reads `_impl->writeOp` out of freed
  memory. Reported by ASan as a heap-use-after-free from `RaftPeerServer::Shutdown`,
  which is the one caller that closes accepted connections from outside their own
  coroutines. The awaitables are detached from the ops first and completed last now,
  with `_fd` cleared before them. Two things worth keeping: the parked awaitables live
  in their coroutines' own frames rather than in `_impl`, which is what makes it safe
  to complete the second after the first has taken the socket down; and the same edit
  went to `KqueueSocket::Close`, which was a copy of the same loop and had the same
  defect on a platform where nothing had ever run a sanitizer either.
- **A watchdog may not write to a socket a coroutine owns, and the reason is
  OWNERSHIP rather than interleaving.** `FrameServer::CloseOverdue` closes connections
  past their deadline, so a peer got a bare TCP close it cannot tell from a crash, a
  drop, or a refusal it never received. The obvious repair — have the sweeper send a
  named refusal first — has an obvious objection and a decisive one, and only the
  decisive one survives contact with a fix. The obvious objection is interleaving:
  `ServeConnection` performs seven `WriteAll` calls across its loop, so a second writer
  splices a refusal into somebody's answer, which is the crossed-reply hazard of
  [`distributed-compilation.md`](distributed-compilation.md) reached from the OUTPUT
  side. That is true, and it invites the wrong fix — a write lock — because it reads
  as a synchronisation problem. **The decisive objection is that the socket lives in
  `ServeConnection`'s coroutine frame (`std::unique_ptr<ISocket> owned`) and the
  sweeper's table holds a bare pointer.** A `co_await socket->Write(...)` in the
  sweeper holds that pointer across a suspension the connection can complete and
  destroy through, so it is a use-after-free that **any amount of interlocking leaves
  in place**. A lock there is a second bug wearing a fix's clothes.
  - **So the sweep asks WHERE the connection is parked, and the two answers differ in
    kind.** Parked on the socket — dribbling a header, dribbling a declared payload,
    pushing out a reply — nothing but the close ends it, and the close is the write
    side gone: it is closed, and told nothing. Parked inside the responder, the close
    wakes nothing at all (the compile runs to completion, hops home, and its
    `WriteAll` fails — `IFrameResponder::RequestTimeout` had already recorded this),
    the write side is idle, and the coroutine comes back on its own. There the entry
    is MARKED and left open, and the connection writes its own refusal when `Answer`
    returns.
  - **"Explainable" therefore means parked in the surface, never "has named a verb".**
    The wider phrasing is the one that reads well and silently promises the
    payload-dribble case, which cannot be honoured without a read-side half-close on
    `ISocket` — and `SHUT_RD` waking a pending IOCP `WSARecv` is not the same story as
    epoll. State the narrow rule; a clause delivered at 80% closes a ticket over a
    surviving gap.
  - **The observation point is ONE line — after `Answer` returns — and that is the
    whole reason this is safe.** A "check whether you were swept before writing" rule
    at each of seven write sites is a rule to forget at the eighth. Here it is a
    consequence of where the mark can be set at all, so exactly one writer is
    structural rather than agreed.
  - **A deferral needs its own bound, and that bound is DERIVED from the responder's
    own window — never a constant.** `Answer` is an interface and nothing can promise
    it returns, so leaving the socket open would hold a descriptor for the life of the
    process where the old close released it; `ExplanationGraceFor` closes it after all.
    It shipped once as a flat `SweepInterval * 4`, which is five seconds, against a
    `CompileResponder::RequestTimeout` of six hundred — **a factor of 120**. A TU that
    has just outrun a ten-minute grant does not return inside five seconds, so the
    deferral expired, the socket closed, and nothing was owed when `Answer` came back:
    the fix explained itself reliably on the cache surface, where the problem is mild,
    and silently not at all on the compile surface it was written for. The constant's
    own justification — *"a property of this mechanism and not of a site's workload"* —
    sat three hundred lines below `IFrameResponder::RequestTimeout` arguing that **one
    number cannot bound both things this endpoint carries**, and pure virtual precisely
    so no surface can inherit five seconds. The general form: **a duration this endpoint
    applies to a responder's work belongs to the responder**, and a constant is how it
    silently stops applying to the surface that needed it most. The connection SLOT is
    held by the coroutine frame either way, so the close was only ever buying one
    descriptor.
  - **And the case that proves the fix could not see it.** The integration test used a
    50 ms window and released its hold at once, so it never approached the grace. Nor is
    the magnitude cheap to assert: the sweep fires at `RequestTimeout` and the grace is
    derived from that same number, so discriminating the old constant needs a window
    above five seconds and a hold between the two — sixteen-plus seconds, measured
    through sleeps whose spread is the quantity. So the grace is a **pure function**
    over the window, pinned on both sides, which is the testing rules' preference
    applied to exactly the situation they describe.
  - **Two counters, two events.** `FrameAnswerDeadlineSweeps` keeps its exact meaning
    (a sweep happened) so nothing scraping it breaks; `FrameDeadlineRefusalsSent` says
    how many of those peers were told why, moved by the connection when the write
    SUCCEEDS. The gap between them is a real quantity — swept peers left to infer it —
    rather than an error term, and a peer that had already hung up belongs in it.
- **A socket has ONE read operation, `Read` and `WaitReadable` share it, and the
  rule lived nowhere it could be obeyed.** Both verbs begin by claiming the same
  per-direction `awaitable` pointer, so arming either while the other is parked
  drops the parked awaitable: that coroutine is never resumed and never freed, with
  no assertion, no error and no log, and the leak is proportional to traffic on
  whatever path did it
  ([#663](https://github.com/LASTRADA-Software/fastcached/issues/663)). The one
  correct statement of the discipline in this tree was a comment in `RedisResp.cpp`
  about that file's own watcher -- so somebody writing the next `WaitReadable` user
  read `ISocket::WaitReadable`, which documented what the call does and said nothing
  about exclusivity, and got a leaked frame per occurrence with no signal of any
  kind. Four things are load-bearing:
  - **The rule is on `ISocket`, because that is where it must be obeyed.** A
    constraint stated by one consumer about itself is a constraint the next consumer
    never meets.
  - **The claim and the check are ONE expression.** `Detail::ClaimReadSlot`
    (`Net/ReadSlot.hpp`) asserts the slot is free and then clears it, and the six
    arm sites across `EpollSocket`, `KqueueSocket` and `IocpSocket` have no bare
    `awaitable = nullptr` left. There is no line to forget the guard on, at the
    seventh site as at the first -- the same shape as `SigningDomain` leaving no
    argument to pass a bare label to. `IocpSocket`'s destructor still assigns
    directly, and that is a teardown rather than a claim.
  - **Debug-only, and the two louder options are both worse.** Refusing the new
    operation would turn today's silent leak into a broken connection on a path that
    is live right now (#710 arms a fresh wait per iteration and cancels none), and
    completing the dropped awaitable would resume a coroutine that may already have
    lost the socket it holds -- a use-after-free where there is currently a leak. So
    the fix for a caller that does this belongs at the caller; what belongs in `Net/`
    is the tripwire that names it.
  - **It is watched refusing, and the hazard has no other coverage at all.** Measured
    while landing the guard: a probe at all six arm sites reported ZERO double-arms
    across `FastCacheTest` (2073 cases) and `fastcache-compile-node-tests`, and no
    script fixture drives a blocking RESP verb or a subscription over a real socket
    -- so nothing in the suite can see this, which is the ticket's own point.
    `read-slot-guard-canary` double-arms a REAL socket through the real reactor, so
    what it drives is the call site rather than the guard function, and
    `scripts/read-slot-guard-gate.cmake` requires the assertion's OWN words: a bare
    `WILL_FAIL` passes for a segfault, a missing library and a refused bind alike,
    which is the same argument `iterator-debug-gate.ps1` makes about the same shape.
    Shown red by removing the guard from one arm site, where it reports `the canary
    SURVIVED` rather than a bare failure.

- **A wait that cannot be cancelled is a frame that cannot be freed, so `Schedule`
  grew a counterpart.** `IReactor` could park a coroutine on a deadline and had no
  way to take it back, which is why `DeadlineTimer` and `InterruptibleSleepUntil`
  poll in bounded steps rather than parking once: a disarmed timer then retires at
  its next tick instead of never. What that still leaves is a frame parked *during*
  the tick, and a reactor destroyed in that window frees nothing --
  `IReactor::Run` returns with its timer heap exactly as it was, deliberately. One
  leaked coroutine frame **per dial and per cache exchange**, harmless in a daemon
  that outlives them and fatal for `fastcache-cc`, where an ASan build then exits
  non-zero and turns every cached compile into a failed one.
  `IReactor::CancelPending` closes it: `Disarm()` takes the timer's own handle back
  off the reactor and destroys it there and then. Five things are load-bearing:
  - **The return value is an ownership transfer, not a status.** `true` means THIS
    call removed the handle, so the caller is now the only one who may resume it;
    `false` means the reactor still has it -- running, or already queued -- and the
    caller must not touch it. That is what makes the race against a timer firing
    concurrently decidable instead of a guess, and it is decided under the same lock
    the timer heap is popped under.
  - **The frame is DESTROYED, not resumed, and that is the difference between
    fixing the leak and moving it.** Resuming only queues it, and the caller that
    disarms is typically about to stop the reactor on its very next line --
    `ReactorExchange` does exactly that -- so the queued handle would never run. The
    first version resumed, and ASan reported the same leak in the same place.
  - **`DeadlineTimer` had to stop awaiting a nested `Task`.** Awaiting one parks the
    INNER coroutine's handle, so the handle the timer recorded would not be the one
    `CancelPending` has to name. Its wait is written out against `SleepUntil`
    directly, and the handle is captured by an awaitable whose `await_suspend`
    returns `false` -- recorded on the way past, without suspending, so it is
    available even in the window before the first hop onto the reactor. It is also
    what makes destroying it sound: the handle is the whole chain, and a
    `DetachedTask` leaves no awaiter holding it.
  - **The first attempt was for the reactor to free what it still held at teardown,
    and it is unsound.** `Submit` and `Schedule` convey no ownership: a `Task` local
    in a test parks its handle and then destroys its own frame at scope exit, so the
    reactor -- destroyed afterwards -- destroyed it a second time. ASan named it
    immediately, which is the argument for the sanitizer fix below in one line.
  - **IOCP cancels timers and not submissions**, because a submission there is a
    completion packet already posted to the kernel and no call takes one back. It
    answers `false` for that case, which is the honest answer under the rule above.

- **A struct returned BY VALUE from a decoder must not borrow from what it decoded.**
  `DecodeCapacity(EncodeCapacity(x))` is the obvious spelling and was a
  use-after-free the moment `CapacityFields` grew one `string_view` member: the
  encoded buffer dies at the semicolon and the view outlives it. Nothing in the type
  warned anybody -- `RegisterView` says "View" precisely because it borrows, while
  `CapacityFields` is used for both directions and reads as a value, so a borrowing
  member turned every existing call site into a trap without a single name changing.

  The decode side of a value-returning record **owns** its bytes. A registration is
  once per node; the copy is free, and the alternative is a hazard in a header every
  binary compiles.
  - **It passed on libstdc++ and failed on libc++**, which is the whole difficulty:
    the read only misbehaves once something reuses the freed block, and which
    allocator does that promptly is a property of the platform. A local ASan run
    reproduces it only if the test *churns the heap* between the decode and the read
    -- without that, ASan reports nothing and the bug looks absent.
  - **The regression test decodes from a temporary on purpose**, and a second one
    scopes the buffer and reads every field after it is gone. A rule this shape
    cannot be left to a case that happens to exercise it.
  - **It happened twice, and the second time nobody was warned either.**
    `CompileResult` and `CodecEnvelope` in `CompileCacheWire.hpp` were the same
    shape -- returned by value from a decoder, borrowing what they decoded -- and
    neither name said so ([#366](https://github.com/LASTRADA-Software/fastcached/issues/366)).
    `CompileResult` reached four borrowing members before anyone counted; #280 added
    the fourth.

    **They got different answers, decided per type rather than by uniformity.**
    `DecodeCompileResult` now returns an owning `CompileResultFields`, because
    `Dispatch` holds a decoded reply across statements and hands the object onward;
    the encode side keeps the name `CompileResult` and its spans, since borrowing an
    INPUT is safe and is what every encode-side struct there does. `CodecEnvelope`
    became `CodecEnvelopeView` and still borrows, because **every** consumer reads it
    in scope and its `Identity` branch in `OpenAs` hands `bytes` straight to the
    caller's container -- owning would reinstate precisely the "second full copy of a
    preprocessed translation unit, on the path least able to afford it" that the
    comment there exists to prevent. The question to ask is per type: does anything
    depend on this not copying, and does the result outlive the buffer in practice.
    Both shapes satisfy the rule; what the rule forbids is borrowing while saying
    nothing.

    The tell was already in the tree: `WorkerProtocol_test.cpp` carried **two**
    hand-rolled workarounds -- a `FieldOf` that copied the span out, and an
    `ObjectField` that mirrored `CodecEnvelope` with an owning `bytes` and a comment
    explaining the dangle. Somebody hit this, understood it, and solved it locally
    twice rather than at the type. `FieldOf`'s copy is gone, because that type owns
    now. `ObjectField` stays, and the difference is the point: it returns outward
    from a helper whose buffer dies with the call, so a local copy is the right
    answer against a type that borrows BY DESIGN. A workaround against an unlabelled
    hazard is evidence the type is wrong; the same code against a `*View` is just a
    caller doing what the name told it to.
  - **Detectability depends on the SIZE of the payload, not only on heap churn.**
    Measured while fixing #366: a four-byte object decoded from a temporary reads
    back correctly under ASan even when the member borrows, and reports nothing. The
    same case at 64 KiB is reported immediately. So a regression test for this rule
    that uses short literals is a test that cannot fail -- which is how it would be
    written by anybody tidying one up. The sizes in
    `CompileCacheWire_test.cpp` are load-bearing and say so.
  - **It happened a THIRD time, and the per-type question is a CONJUNCTION.**
    `Distributed::CallerContext::peerId` was a `std::string_view` in a type whose
    name promised nothing
    ([#395](https://github.com/LASTRADA-Software/fastcached/issues/395)). Ask both
    halves -- does every consumer read it in scope, AND does something depend on this
    not copying -- because one clause false is not a tie. This type satisfied the
    first, and measuring the second killed it: the figures live on `CallerContext`
    itself, in `SchedulerService.hpp`, and are not restated here so there is one row
    to read and one place to correct.

    **What the borrowed field DECIDES outranks that arithmetic**, and it is what
    settled this one. A wrong `CapacityFields` is a wrong number. A wrong `peerId` is
    a **membership decision read from freed memory** -- it is the kernel's peer host
    and admission is judged from it -- so the two mistakes are not commensurable and
    the nanoseconds do not get a vote. Where a borrowed field feeds a trust boundary,
    own it and skip the benchmark.
  - **A correctly-SIZED regression test in the wrong ARRANGEMENT still reports
    nothing**, which is the half of the size rule above that #395 nearly died of. Its
    ticket concluded the defect could not be tested at all, and the reason was
    arrangement rather than impossibility:
    - Constructed and read in ONE expression -- `Use(Make(x))` -- **nothing dangles
      at any size**, because a by-value parameter lives to the end of the full
      expression. That is how a test gets written, and it passes under the defect.
    - STORE the value, drop the source, churn the freed storage, then read. Now the
      size decides *which* check fires rather than whether one does: inside
      libstdc++'s 15-character SSO buffer it is `stack-use-after-scope`, visible only
      with ASan's stack poisoning, and past it the buffer is on the heap and it is
      `heap-use-after-free`, caught with no ASan options at all.

    So pick a size that is REAL rather than merely large, and `static_assert` it: the
    36-character peer in `SchedulerService_test.cpp` is what `getpeername` returns on
    any IPv6 deployment, which makes the case that exposes this the ORDINARY one --
    the suite had simply only ever been run against v4 loopback.
  - **A deleted overload IS probeable, but only from a dependent context**, and
    getting this wrong is easy. `static_assert(!requires { Decode(Encode(x)) })`
    written at namespace scope with concrete types is a **hard error** on MSVC,
    Clang and GCC alike: a non-dependent requires-expression checks its requirements
    immediately, and there is no substitution for the failure to be a *substitution*
    failure. Lift it into a template and it works everywhere:

    ```cpp
    template <class T> concept Decodable = requires(T&& t) { Decode(std::forward<T>(t)); };
    static_assert(Decodable<std::vector<std::byte>&>);   // named buffer: fine
    static_assert(!Decodable<std::vector<std::byte>>);   // temporary: rejected
    ```

    That is the mechanism the ranges poison pills rely on, and `std::is_invocable`
    reports `false` for a deleted candidate on the same basis. Measured on all three
    compilers in both forms before this was written down, because the first version
    of this bullet claimed the property was untestable and was simply wrong.

    No decoder here carries such an overload, and the reason is **not** testability:
    the ten `*View` types all have the same trap, so guarding one would be
    inconsistent, and it would reject three existing call sites that use the
    spelling *safely* inside a single full expression. If it is ever added it should
    be added to all of them at once.

## Keyspace events for what nobody asked for

`expired` and `evicted` are the two keyspace events no verb handler can fire,
because no verb is executing when they happen. The path that produces them —
tier records, `NotifyingStorage` drains, `RedisMutationObserver` publishes —
has four constraints, and every one of them was a defect first.

- **Naming the victim at the tier is necessary and not sufficient.** For as long
  as `MutationKind::Expire` and `::Evict` existed, the obvious reading was that
  the storage layer's silence was the whole problem. It was not:
  `RedisMutationObserver::DescriptorFor` was a stub returning an empty
  descriptor for *every* kind and the observer held no notifier, so a named
  victim would still have published nothing. Whenever an event is missing, check
  both ends before changing either.
- **Publish only for the kinds no handler covers.** `RedisResp.cpp` already fires
  verb-specific events for everything a client asked for. A second publish at the
  storage layer puts two `__keyevent@0__:*` frames on the wire per write, which
  every existing subscriber sees as a behaviour change. That is why most rows of
  `EventTable` are deliberately empty — and it is a table, so the next kind is a
  row rather than a condition somebody has to remember to add.
- **A reclaim is reported BEFORE the call that caused it, because they can name
  the same key.** `ADD k` on a lapsed TTL reclaims `k` inside the lookup and then
  re-creates it. Drained at scope exit, that reads as `set k` then `expired k`,
  and a subscriber concludes a live key is gone. `Notify` drains first; the scope
  guard is only the backstop for calls that report nothing (a failed `DEL` on an
  expired key, a strict-LRU `Get`).
- **The recording gate and the publishing gate must agree.** The tiers copy a
  victim's key only when `IStorageMutationObserver::HasObservers()` says somebody
  is listening, so an observer that answered for WATCHers alone would leave a
  daemon with subscribers and no WATCHers publishing nothing — nothing would ever
  have been recorded to publish. `WouldPublish` likewise has to test `K`/`E` and
  not just the class bit: `notify-keyspace-events: A` names every class and no
  channel, and publishes nothing at all.

And one that is about the cache rather than the wire:

- **In a layered cache no single tier's eviction is total, so none is reported.**
  L1 dropping an entry is a demotion — the key is still in L2 and the next read
  serves it. L2 dropping one leaves the key in the L1 mirror, which `Get` answers
  without ever consulting L2, and L1 is exactly where it will be, because L1
  absorbing the hits is what left L2's recency stale enough to evict it. Erasing
  from L1 to make the event true would throw a hot key out of RAM to justify a
  notification. An *expiry* is total — both tiers hold the same TTL — so
  `LayeredStorage` forwards expiries to L2 and swallows its evictions.

## The active expiry cycle

`ExpiryReaper` is the only thing that calls `IStorage::PurgeExpired` in
production, and for a long time nothing did — which made every rule below a
consequence rather than a precaution.

- **A reclaimer that nothing constructs is the bug it was written to fix.**
  `PurgeExpired` existed, was correct, was tested, and had no production caller,
  so expiry was entirely access-driven: with the default `Approximate` LRU a read
  does not reclaim either, and a key that lapsed and was never touched again kept
  its bytes and its byte-budget contribution until eviction happened to reach it,
  publishing no `expired` event for exactly the case a subscriber subscribes for.
  Assert the wiring, not only the mechanism — `Detail::StartExpiryCycle` is
  exposed for no other reason.
- **Sweep the chain the engine writes through, not a tier below it.** The reaper
  is handed `engine.Storage()`, which is the `NotifyingStorage`. Pointed one layer
  down it would free the bytes and publish nothing — half the bug left in place,
  and invisible to every storage-level test, because the tiers *do* record into
  the reclaim log either way. Nothing drains it.
- **One cycle per daemon, not one per reactor.** The sweep takes each shard's
  exclusive lock in turn, so a second cycle contends with the first for no gain.
  It runs on reactor 0, and its owner is declared after the reactors and before
  the acceptor threads: destroyed while the timer wheel it parked on still exists,
  and after every thread that could resume it has been joined.
- **The reclaim ceiling lives below `ReclaimLog::DefaultCapacity`, and is
  `static_assert`ed against it.** A sweep permitted to reclaim more keys at once
  than the log holds discards the `expired` events it runs to produce, and counts
  them in `fastcached_keyspace_reclaim_events_dropped_total`. The failure would be
  a metric nobody reads, not a symptom.
- **A bounded sweep must resume, not restart.** Each tier keeps a cursor and
  `ShardedStorage` rotates its starting shard, because a budget smaller than the
  shard count is spent before the loop reaches the far ones. Without both, a cache
  larger than one budget never expires anything past its own prefix. The cursor
  outlives the call that produced it, which makes **every** erase a place it can
  dangle: one erase point per tier (`EraseAt`, `EraseNode`) is what keeps that
  fix-up in one place, and without it MSVC's debug iterators abort the process.
- **`completedPass` is what separates "nothing left" from "out of budget",** and
  only the first may back off. A pass that stopped early has entries it has not
  looked at yet.
- **Zero means opposite things in the two knobs, and both are load-bearing.**
  `--expiry-interval=0` disables the cycle, which is a real thing to ask for, and
  a disabled cycle is a coroutine that *ends* rather than one parked forever on a
  deadline nothing will move. `--expiry-scan=0` is what `PurgeBudget` spells as
  *no ceiling* — sweep the whole keyspace under the shard lock — so it is refused.
- **The reaper's options are baked in at construction, so they are immutable
  across a reload.** Accepting a SIGHUP that changes them would leave
  `reloader.Current()` reporting an interval the daemon is not sweeping at.

## Reclaiming on the disk tier

- **A read may not reclaim; a write must.** `CowTreeStorage::Get` and `Peek`
  reject a lapsed record without erasing it, because a read can be holding nothing
  but a shared lock and opening a write transaction there would break CowTree's
  single-writer contract. Every write verb holds the exclusive lock, so the rule
  never applied to them — and for as long as they were lumped in with the reads,
  the same client sequence produced a different event stream depending on
  `--storage`, and the dead bytes stayed on disk until a sweep or a `DELETE`
  reached them. `AcceptLiveRecord` is the one spelling of that question, so the
  reclaim cannot be remembered at four sites and forgotten at a fifth.
- **Reporting without erasing would have been worse than neither.** The record
  stays, so the next `APPEND`/`INCR`/CAS on the same lapsed key fires `expired`
  again. The erase is what makes the event true exactly once.

## Open work

- **[#710](https://github.com/LASTRADA-Software/fastcached/issues/710)** —
  `RunBlockingRead` arms a fresh `ArmDisconnect` per loop iteration and cancels none,
  so a wait resolved by the data or timeout arm leaves the previous trampoline parked
  in `WaitReadable`, against the socket's single read-op slot. Predates #673 and is
  unchanged by it. Same family as
  [#663](https://github.com/LASTRADA-Software/fastcached/issues/663) and deliberately
  not folded into it: #663 is the shared slot, this is one caller misusing it. Since
  #663 landed the misuse is no longer silent — `Detail::ClaimReadSlot` asserts, so a
  Debug build reaching this path dies naming it, which is what a fixture for this
  ticket should expect to see before it sees a leak.
- **[#711](https://github.com/LASTRADA-Software/fastcached/issues/711)** — three
  comment blocks in `src/apps/fastcache-compile-node/` still state the pre-#671
  doctrine and the pre-#677 socket behaviour. Listed here rather than only with the
  compile surface because what they contradict is the EOF rule above, and because one
  of them is the stated reason that surface takes "the opposite rule" — a contrast
  that no longer exists.

