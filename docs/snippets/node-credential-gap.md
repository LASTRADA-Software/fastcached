!!! danger "A node has no inbound credential, and setting one breaks it"

    None of a node's three framed surfaces — scheduler, compile port, cache tier
    — serves the `AUTH` verb, so there is nothing for a credential to
    authenticate against. `--requirepass` on a node is only the secret it
    **presents** when it dials somebody else, and it works in exactly one
    direction: against a `fastcached` named by `--upstream`, which does serve
    `AUTH`. Presented to another *node* it is refused `dispatch-not-permitted`,
    and the caller reports that in place of the answer to the request it actually
    sent:

    | You set | What breaks |
    |---|---|
    | `--requirepass` on a worker | `REGISTER` is refused; the worker never joins the fleet. |
    | `FASTCACHE_TOKEN` on a client with `FASTCACHE_SCHEDULER` set | Every `LEASE` is declined and every compile happens locally, behind a green build. |
    | `--requirepass` with `--cluster-status` and friends | Refused, naming a verb you never typed. |

    That is [#198](https://github.com/LASTRADA-Software/fastcached/issues/198),
    and it is a gap rather than a design: what an inbound credential should be
    spelled, and what `AUTH` means against a node that has none configured, are
    the questions it is open on.
