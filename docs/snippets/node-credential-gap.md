!!! note "A node verifies machines, and holds no shared secret"

    A compile node's framed surfaces — scheduler, compile port, cache tier — take one
    inbound credential, and it identifies a **machine**, not a person: a machine ticket,
    signed by a machine's own identity key and verified against the roster the node holds.
    There is no password and no key shared across the fleet, so nothing a node could leak
    lets another machine speak for it. `--requirepass` on a node is only the secret it
    **presents** when it dials a `fastcached` named by `--upstream`, which is the one
    surface that takes one.

    A launcher sends `FASTCACHE_TOKEN` to the cache at `FASTCACHE_ADDR` and to nothing
    else; every exchange with another machine presents a ticket its own node minted for
    that exchange. A password `AUTH` that does reach a node is answered `Ok` and
    establishes nothing, so a client configured with a token is never broken by a node —
    the node's scheduler once answered it `dispatch-not-permitted` instead, which the
    launcher treats as fatal
    ([#340](https://github.com/LASTRADA-Software/fastcached/issues/340)).

    A ticket travels in the clear, so it is bound to ONE endpoint, lives a minute and is
    spent once at the node that accepts it; a node proof is sealed from its first frame. A
    network where a ticket can be captured costs a replay at no other node and no later
    moment. The cache tier admits this machine only, whatever the caller presents.
