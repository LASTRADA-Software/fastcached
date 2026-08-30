!!! danger "A node has no inbound credential, and setting one breaks it"

    None of a node's three framed surfaces — scheduler, compile port, cache tier
    — serves the `AUTH` verb, so there is nothing for a credential to
    authenticate against. `--requirepass` on a node is only the secret it
    **presents** when it dials somebody else, and it works in exactly one
    direction: against a `fastcached` named by `--upstream`, which does serve
    `AUTH`.

    What matters is **how a surface refuses it**. `fastcache-cc` steps over
    exactly one refusal — `unknown-opcode` — and carries on unauthenticated,
    which is the right outcome against a surface with no credential to check.
    The node's **cache tier** answers that code, so a client with
    `FASTCACHE_TOKEN` set reads and writes it normally and reports
    `credential ignored`. The **scheduler** and **compile** ports still answer
    `dispatch-not-permitted`, which the launcher treats as fatal, and the caller
    then reports that in place of the answer to the request it actually sent:

    | You set | What breaks |
    |---|---|
    | `--requirepass` on a worker | `REGISTER` is refused; the worker never joins the fleet. |
    | `FASTCACHE_TOKEN` on a client with `FASTCACHE_SCHEDULER` set | Every `LEASE` is declined and every compile happens locally, behind a green build. |
    | `--requirepass` with `--cluster-status` and friends | Refused, naming a verb you never typed. |

    Those three rows are the *refusal code* only, and are
    [#340](https://github.com/LASTRADA-Software/fastcached/issues/340) — the same
    one-line fault the cache tier had, in the two surfaces that have not been
    corrected yet.

    The larger gap behind them is
    [#198](https://github.com/LASTRADA-Software/fastcached/issues/198), and it is
    a gap rather than a design: what an inbound credential should be spelled, and
    what `AUTH` means against a node that has none configured, are the questions
    it is open on. The two are worth keeping apart — #340 restores what a client
    already expects and decides nothing about credentials; #198 is the decision.
