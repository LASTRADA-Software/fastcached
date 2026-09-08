# Upgrading a fleet

**A `fastcache-compile-node` fleet upgrades as a unit. A rolling upgrade — replacing
one node at a time while the others keep serving — is not supported in this release,
and this page is here so that is something you plan around rather than discover during
a rollout.**

Read this before upgrading more than one machine.

## Why

Every `0xFC` exchange carries a wire version. A build accepts exactly one:
`MinSupportedVersion` equals `CurrentVersion` in `CompileCacheWire.hpp`, and has at
every point in this project's history. So the moment one end moves, every peer still on
the old version is refused.

That is a deliberate choice rather than an omission, and the reasoning is recorded at
the constants themselves. In short: a reply on this wire carries a status byte and a
length and *no kind*, so the "step over what you do not understand" property that the
request framing has does not exist on the way back. An old client meeting a new reply
does not skip it — depending on the change it either abandons a compile several minutes
in, as a transport failure naming nothing, or reads a record it does not understand as
the field it expected and continues. Refusing the older version outright is
`UnsupportedVersion`, which names the supported range and arrives before any source is
sent. A loud, immediate, named refusal is the better failure.

What makes it a *fleet* problem rather than a daemon problem is that a fleet has more
than one process. A cache daemon is upgraded, restarted, and done; twenty nodes cannot
be.

## The supported procedure

1. **Stop the builds.** Anything running `fastcache-cc` against the fleet should be
   quiescent. Builds do not break — see *What a mismatch looks like* — but they compile
   locally, so a build started here is slow rather than wrong.
2. **Stop every node**, including any machine running only the scheduler or only a
   cache tier.
3. **Upgrade every node and every client to the same release.** The launcher
   (`fastcache-cc`) speaks this wire too, so a client left behind is a client that
   silently stops using the cache.
4. **Start the nodes**, schedulers first if you run them separately.
5. **Confirm** with the counter below before releasing the builds.

The order matters only in that nothing old should be running once anything new is.

## What a mismatch looks like, and why you must go looking

**A version mismatch never fails a build.** `fastcache-cc` treats an unusable cache as a
miss and compiles locally, which is the contract that keeps a cache problem from
becoming a build problem. The cost is silent: every affected client loses the cache and
every dispatched compile stops being dispatched.

The signal is server-side, and it is spread over **four** counters because a fleet has
four places a frame can be refused. Watch all of them: a mismatch shows up only on the
surface the stragglers actually talk to, so seeing zero on one counter tells you
nothing about the others.

| Series | Refused where |
|---|---|
| `fastcache_node_cache_requests_refused_unsupported_version_total` | a node's cache tier |
| `fastcache_worker_frames_refused_unsupported_version_total` | a node's compile worker |
| `fastcached_dispatch_frames_refused_unsupported_version_total` | the fleet scheduler's port |
| `fastcached_cache_frames_refused_unsupported_version_total` | the `fastcached` daemon's compile-cache port |

Any of them non-zero and rising means something is still speaking the other version.
These are the *only* places a mismatch is visible, so an operator who has not been told
to watch them will not see one — which is why they are named here rather than left to
be found during a rollout.

All four are exported only when `--metrics` is set. If you do not scrape, the honest
statement is that you cannot tell whether the upgrade was complete, which is a reason
to do step 5 rather than a reason to skip it.

The asymmetry is worth stating plainly: **the server can see this and the client
cannot.** Check the servers.

## Downgrading

The same procedure, in the same order. Nothing in the on-disk cache format is tied to
the wire version, so a node that is downgraded serves its existing cache; entries it
cannot decode are treated as misses rather than as corruption.

## When this changes

A compatibility window — a build accepting more than one wire version — is a real
possibility rather than a promise, and it would be stated per verb rather than implied
by a single constant, because the verbs differ in how much room they have.
`Op::Compile` has exact arity and is the binding constraint; the cache verbs are
cheaper to widen. Until such a window is documented here, assume none exists and treat
every upgrade as a flag day.
