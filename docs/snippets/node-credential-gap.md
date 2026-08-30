!!! warning "A node checks no inbound credential"

    None of a node's three framed surfaces — scheduler, compile port, cache tier
    — serves the `AUTH` verb, so there is nothing for a credential to
    authenticate against. `--requirepass` on a node is only the secret it
    **presents** when it dials somebody else, and it works in exactly one
    direction: against a `fastcached` named by `--upstream`, which does serve
    `AUTH`.

    **Setting a credential no longer breaks anything.** All three surfaces refuse
    `AUTH` with `unknown-opcode`, which is the one refusal `fastcache-cc` steps
    over before carrying on unauthenticated — the right outcome against a surface
    with no credential to check. So a client with `FASTCACHE_TOKEN` set leases,
    compiles, registers, reads and writes normally. On its **cache** exchanges it
    also reports `credential ignored`, so the fact is not silent there; on a lease,
    a compile, a release or a registration it is — those callers receive the same
    flag and discard it.

    That code is a wire contract between binaries that do not link each other, and
    the three surfaces once answered it three different ways. Two of them said
    `dispatch-not-permitted`, which the launcher treats as fatal and returns in
    place of the answer to the request actually sent —
    [#283](https://github.com/LASTRADA-Software/fastcached/issues/283) corrected
    the cache tier and [#340](https://github.com/LASTRADA-Software/fastcached/issues/340)
    the other two. Until then, a `FASTCACHE_TOKEN` client had every `LEASE`
    declined behind a green build, and a `--requirepass` worker never joined the
    fleet at all.

    **What is still open is the credential itself**, and it is a gap rather than a
    design: what an inbound credential should be spelled, and what `AUTH` should
    mean against a node that has none configured, are the questions
    [#198](https://github.com/LASTRADA-Software/fastcached/issues/198) is open on
    ([#289](https://github.com/LASTRADA-Software/fastcached/issues/289) is the
    scheduler's half). Until it closes, a fleet's boundary is **network
    reachability plus membership**, not a secret — so keep these surfaces on a
    network you would run a compiler for.
