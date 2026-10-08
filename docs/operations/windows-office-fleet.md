# Running a Windows office fleet

This is the runbook for one shape of deployment: a handful of Windows developer PCs, some in
the office and some at home over a VPN, sharing compiles and a cache. It says what to do, in
order, and points at the reference pages for why. Every flag and condition named here is the
binary's own; `fastcache-compile-node --help` and `fastcache-cli --help` are the authority if
the two ever disagree.

## The shape

| Machine | What it runs | Role in the fleet |
|---|---|---|
| **A**, the office PC | the node, always on | founds the fleet: its only voter, its leader and scheduler, a worker and a cache of its own |
| **C**, the office cache host | the node, always on | a member the fleet's `shared-cache` setting names; every other node reads through to it |
| every other PC | the node, plus `fastcache-cc` for its builds | joins as a **learner** (it votes for nothing), works for the fleet, and compiles with it |

Every machine runs the same MSI and the same `fastcache-compile-node` service. Nobody types an
address into a node: A forms a fleet of one at its first start, every later machine finds it
(by LAN beacon, or by a seed name across the VPN), asks to join, and **keeps building on its
own while it waits**. It becomes part of the fleet when an operator approves it on A.

**A decides who is in the fleet.** The commands that change it -- approving, rejecting, opening
an auto-approve window, forgetting a machine, promoting a voter, changing a cluster setting --
are answered only for a process on A itself, or for a machine that holds a **voter's** seat. A
PC is a learner, so it can read the fleet but not change it: run those commands on A (or on any
voter you promote later). A PC that tries is refused `operator-standing-required`, which names
the remedy.

Give A a fixed address (a DHCP reservation) and a DNS name you control, such as an alias
`office-a.example.com`, that resolves from the office and over the VPN. Every PC reaches A by
that name as its seed (and, on a learner older than this release, as its builds' scheduler;
see step 5).

## 1. Install A first

On A, run the MSI and select **fastcache-compile-node** (silently:
`msiexec /i fastcached.msi /qn ADDLOCAL=CM_C_Cli,CM_C_Node,CM_C_Launcher`). Pass no property.

The `FastCacheCompileNode` service starts at once. With no fleet to join, A founds one: it is
a cluster of one, leads it, schedules for it and works for it. Check:

```powershell
fastcache-cli node
```

`scheduler-role` reads `leader`, `consensus-standing` reads `voter`, and `node-id` and
`public-key` name A. The service logs on as `NT SERVICE\FastCacheCompileNode` and keeps its
identity under `%ProgramData%\fastcache-node`, readable only by SYSTEM, Administrators and that
account. The install opens the Windows Firewall, on every network profile, for the node port
(TCP 6674), the consensus port (TCP 6680) and discovery (UDP 6681, and the reply port UDP 6682,
which the package pins through `FASTCACHE_DISCOVERY_REPLY_PORT`).
`FASTCACHE_FIREWALL_ALLOW=10.0.0.0/8` narrows the remote side of every rule to one address with
an optional prefix, so pick one prefix that covers both the office LAN and the VPN pool. The
node is the cache: if the **fastcached** feature is installed as well, its `FastCached` service
is disabled and stopped, since both want port 6674 and it must not be started beside the node.

### Pin the fleet

`fastcache-cli node` on A also prints `fleet-id`: the cluster id and the identity key of every
voter, `<cluster-id>@<key>[,<key>...]`. That is the **pin**. A node given one belongs to that
fleet and no other, and takes that fleet's word only as one of the pinned voters signed it.
**A key pin stops a LAN impostor; an unpinned node trusts on first use.** A cluster id alone is
no pin, because every beacon carries it, so a value without a key is refused where it is typed.
Do not compose the value: copy it whole from `fleet-id`.

Pin **A itself**, so that no stranger proving an older fleet can take A, and the fleet with it.
A re-registration does not restart a running node, and the pin is read at startup, so restart
it afterwards:

```powershell
msiexec /i fastcached.msi ADDLOCAL=CM_C_Node FASTCACHE_FLEET_ID=<fleet-id as printed>
Restart-Service FastCacheCompileNode
```

Then pass the same value as `FASTCACHE_FLEET_ID` on every other install (step 2), at the least
on every machine outside the office LAN. A laptop that hears another fleet on a hotel network or
a VPN segment, or a machine claiming the office's id under a key of its own, is then never
asked, and is reported as `foreign-fleet-visible` instead. `fastcache-cli node` shows the pin a
node trusts by as `fleet-pin`, which reads `none` on an unpinned node.

The installer checks the value with the node's own parser before it registers or remembers
anything. A value the node refuses **fails the whole transaction** (msiexec exits 1603, and the
log names `FastCacheNodeCheckArguments`), leaving the registration that was there before;
`fastcache-compile-node --check-arguments --fleet-id=<value>` says what is wrong with it. A node
whose state already commits it to a cluster the pin does not admit refuses to start, naming
both: to move it to the pinned fleet, stop the service, delete `%ProgramData%\fastcache-node`
and start it again.

**The pin guards the JOIN.** Once a PC has joined, it follows the fleet's own agreed state, as
every member does, so PCs already in the fleet never need a new pin. When you promote another
voter (step 6), `fleet-id` grows by its key, and the new value belongs in two places: the pin of
every PC you have **not installed yet** (its join may be redirected to the new voter, and a pin
without that voter's key refuses the redirect, naming the key to add), and the **new voter's
own** pin, or it admits nobody.

## 2. Install every other PC

**On the office LAN**, run the same MSI with the pin:

```powershell
msiexec /i fastcached.msi ADDLOCAL=CM_C_Cli,CM_C_Node,CM_C_Launcher FASTCACHE_FLEET_ID=<fleet-id>
```

The node hears A's beacon, sees the fleet its pin names, and asks A to admit it. Until somebody
approves it, it is its own fleet of one and builds on its own; nothing waits.

**Over the VPN**, beacons do not cross, so tell the node where to ask, by A's name:

```powershell
msiexec /i fastcached.msi ADDLOCAL=CM_C_Cli,CM_C_Node,CM_C_Launcher FASTCACHE_FLEET_ID=<fleet-id> FASTCACHE_FLEET_SEED=office-a.example.com
```

`FASTCACHE_FLEET_SEED` is one name or `name:port` (the port defaults to 6674); a node that needs
more than one names them under `fleet_seed:` in its configuration file,
`%ProgramData%\fastcache-compile-node\fastcache-compile-node.yaml`. An office whose PCs carry a
DNS suffix can skip the property altogether: every node also asks DNS for
`_fastcache._tcp.<its DNS suffix>`, so one SRV record whose target is A (port 6674) reaches
every PC, at home included, once its VPN hands out that suffix.

A seed is asked only while the node is alone. Once it has joined, the node remembers its
fleet's endpoints in its state directory and asks the seed again only when none of them
answers.

**The MSI remembers what it was given.** `FASTCACHE_FIREWALL_ALLOW`, `FASTCACHE_FLEET_SEED` and
`FASTCACHE_FLEET_ID` are written to `HKLM\SOFTWARE\fastcached\Installer` and read back by the
next repair, feature change or upgrade, so one that names none of them registers what the
install was given; one that states a new value replaces the remembered one. An empty value
counts as leaving the property out, so it cannot clear one: to drop a remembered value,
uninstall (which forgets them all) and install again. There is no property for the address
other machines reach a node at: the node advertises the address this PC routes from, and
re-derives it when the PC changes network -- a new DHCP lease, Wi-Fi to the dock, the VPN
coming up -- so a renamed PC or a new VPN address needs no reinstall and no restart. A name is
an opt-in pin: one that must be typed goes under `advertise:` in the configuration file, and a
literal address typed there stops the node from following the network.

## 3. Approve the machines that ask

On A:

```powershell
fastcache-compile-node --enroll-list
```

Each waiting machine is a row with its id, its **whole** identity key, the address it claims
and the host it came from. On the machine itself, `fastcache-cli node` prints `public-key`:
compare the two. That comparison is the whole of what makes an approval safe. Then paste the
line `--enroll-list` prints for the row:

```powershell
fastcache-compile-node --enroll-approve=<node-id>@<key>
```

The machine becomes a learner of the fleet at its next poll; `fastcache-cli node` on it then
reads `consensus-standing` `learner`. `--enroll-reject=<node-id>` refuses a row: that machine is
told so and leaves the fleet alone for an hour before it asks again. Rejecting a machine that was already approved does not remove it:
`--cluster-forget` does (step 6). `--enroll-clear` drops every undecided row, for a list
somebody filled. One address may hold at most four undecided rows at a time, and a machine that
stops asking for ten minutes is dropped from the list.

**On go-live day**, when every PC is installed within the hour, open a window instead:

```powershell
fastcache-compile-node --enroll-auto-approve=15min
```

Until it ends, every machine that asks is admitted under the key it asks with, and nobody
compares anything. The duration is a whole number and one of `ms`, `s`, `min`, `h`, `d`:
`15min`, never `15m`, which is refused. At most `24h`; running it again re-arms it from now,
`=off` ends it, and a restart or a change of leader ends it too. It also admits rows already
waiting, so reject the ones that must not get in **before** you open it. While it is open the
node raises `enrollment-window-open` and `fastcache-cli node` reads `auto-approve (N min left)`;
afterwards `--enroll-list` marks every row it admitted.

## 4. The shared cache on C

The fleet's shared cache is a member, named by its **id** in one replicated setting. No
`fastcached` is needed and no node is configured to use it.

1. On C, install the MSI with **fastcache-compile-node**, exactly as any PC (step 2). It is
   always on, so it may work as hard as it can: set `node_class: dedicated` in its
   configuration file, and `shared_cache_disk:` to the disk space you give the shared tier (the
   default is `64g`; `0` grows as needed). The budget is what the store occupies on disk, with
   compressed values at their compressed size. Run `Restart-Service FastCacheCompileNode` after
   editing the file.
2. Approve it (step 3). Note its `node-id`, from the `--enroll-list` row or from
   `fastcache-cli node` on C.
3. On A, name it, once:

   ```powershell
   fastcache-compile-node --cluster-set=shared-cache=<id-of-C>
   ```

Every node then reads through to C after a local miss and offers it what it stores. It proves
C's recorded key before it sends anything, and presents no secret. Moving C, forgetting it, or
naming another machine reaches every node from the cluster's state, with nothing to edit and
nothing to restart. An unreachable C is a miss, never an error. C keeps the tier under its state
directory (`%ProgramData%\fastcache-node\shared-cache`), which the install already secured, so
it needs no directory of its own.

`fastcache-cli node` on any PC shows where its shared cache comes from (`shared-cache`), which
machine and endpoint that is, and how the last attempt went (`shared-cache-state`, with
`shared-cache-detail` saying why). The conditions are `shared-cache-unproven` on a PC that is
not reaching C, and `shared-cache-unavailable` on C when its tier will not open.

## 5. Each developer's build environment

`fastcache-cc` fronts every compile. For a CMake build:

```powershell
$env:FASTCACHE_SOURCE_DIR = 'D:\src\product'
$env:FASTCACHE_BINARY_DIR = 'D:\src\product\build'
$env:FASTCACHE_SCHEDULER  = '127.0.0.1:6674'
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER_LAUNCHER=fastcache-cc -DCMAKE_CXX_COMPILER_LAUNCHER=fastcache-cc
```

- **`FASTCACHE_SOURCE_DIR` and `FASTCACHE_BINARY_DIR`** are required: with either unset, nothing
  is cached. They are what lets two checkouts at different paths share an entry.
- **`FASTCACHE_SCHEDULER` is `127.0.0.1:6674` on every PC**, this PC's own node. Once approved
  that node is a learner: it schedules nothing itself, and answers a lease with `not-leader`
  naming the leader's scheduling endpoint, which the launcher follows, and the launcher
  releases the lease where it was issued. So the setting survives leadership moving to another
  voter, and no PC is re-pointed after an election. A PC whose own node leads, or is still its
  own solitary cluster (not yet approved), leases from its own scheduler directly, so the
  setting is right from the first day. While the node knows no leader, or has not heard from
  it for longer than an election timeout (a PC off the VPN), it names nobody and every miss
  compiles locally. A learner older than this release refuses the lease instead of
  redirecting, and every miss compiles locally; on such a PC name A here, by the same name as
  the seed. A build tree configured through `cmake/portable/CompileCache.cmake` with A's name
  keeps it (below); reconfigure it with `-DFASTCACHE_SCHEDULER=127.0.0.1:6674`. Any other build
  reads the variable at each compile. Unset, every miss compiles locally.
- **`FASTCACHE_ADDR` needs no setting**: its default, `127.0.0.1:6674`, is this PC's own node,
  which is this PC's cache and the node that mints the machine ticket every exchange with
  another machine presents.
- **Debug information in the object: `/Z7`**, never `/Zi` or `/ZI`. `cl` with `/Zi` writes a
  shared PDB beside the object, which a hit cannot reproduce, so such a compile is never
  cached. With CMake 3.25 or later and policy CMP0141 set to `NEW`:
  `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`.
- **Precompiled headers off.** `/Yc` writes a second artefact a hit cannot carry, and a `cl`
  `/Yu` object names the PCH of the checkout that built it, so `cl` caches neither; set
  `-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON`.
- **No `/FI`.** A forced include is a file the worker would have to open on its own disk, so a
  compile naming one is never dispatched. It still builds and caches on this PC.

A build of this repository through `cmake/portable/CompileCache.cmake` sets the source and
build roots, `/Z7` and the PCH switch by itself, and reads `FASTCACHE_SCHEDULER` at **configure**
time, baking it into the build tree; set it before the first configure. Any other project states
all of it as above.

## 6. Operating it

### How much each PC gives

A PC is a **workstation** unless told otherwise: it keeps two cores for the person at it.
Change that in its configuration file (`reserve_cores: 4`, or `reserve_cores: 0` to keep none,
which is not the same as leaving the key out), or fix the number outright with `slots:`, which
is the answer and is not reduced further. `slots: 0` runs no worker on that PC at all: it
still builds with the fleet and caches, and is never sent a compile. Capacity is read at
startup, so restart the service after a change.

Each node's own cache is in memory, a quarter of its RAM within 512 MiB to 8 GiB
(`cache_memory:`; `0` turns it off). A disk tier is opt-in: `cache_dir:` names it and
`cache_disk:` caps it at what the store occupies on disk, compressed values at their compressed
size. The service account must be able to write that directory, and beyond the state directory
the install grants it only what its own command line names, which on an MSI install is nothing:
grant `NT SERVICE\FastCacheCompileNode` modify rights on it yourself. Most PCs need no disk
tier: C's shared tier is the fleet's disk.

### One Visual Studio, everywhere

A worker serves the compilers it finds, each under a fingerprint of the compiler and its
include roots, and a client's compile goes only to a worker with the same fingerprint. So keep
Visual Studio and the Windows SDK at **one version on every PC**, and upgrade them together. A
worker notices a compiler upgraded under it and re-registers under the new fingerprint by
itself. A PC whose compiler no worker serves compiles locally, and the leader raises
`unserved-toolchain` naming the driver and the version. `fastcache-cc
--print-toolchain-fingerprint cl.exe` on a client is the number to compare with what a worker
logs as `serving`.

### What a reboot of A means

A is the only voter, so while it is down the fleet has no scheduler. Every PC's builds compile
locally and keep their own cache: within about one consensus pass (a second or so) a PC's own
node stops naming the silent A, and its launcher then declines the lease without dialling A.
Reads through to C keep working, since each node still holds the cluster state it applied. A
elects itself again within seconds of starting, the PCs dial it again by themselves, their
workers register within one 20-second round, and dispatch resumes with nothing to do. An
auto-approve window open at the reboot is gone: it lived in A's memory.

A longer outage has one more effect. A PC whose worker has heard from no leader for 65 minutes
refuses every compile grant and raises `consensus-leader-silent`, because the state it would
check a grant against may be one its fleet has moved past. It clears at the first word from the
leader, so it needs no action once A is back.

Before a planned reboot, `fastcache-compile-node --cordon` on A stops its worker taking new
compiles and lets the running ones finish.

### Adding a second voter

A second voter is **not** more available than one: two voters need both for a quorum, so
either being down stops the fleet. Promote two learners at once, for three voters, or leave A
alone. Promote only machines that are always on: a voter that sleeps is counted by every
quorum while it sleeps. A voter whose address moves is followed -- with `advertise` and
`raft_self` left at their default, it announces its new address and the leader re-records it,
usually within seconds -- but one pinned to a name is reached again only once DNS catches up.
That re-recording is a commit, so it needs the voters that did NOT move to be a quorum: with
two voters, or with most of them renumbered at once (a new router, a new DHCP scope), it does
not heal by itself ([#1644](https://github.com/LASTRADA-Software/fastcached/issues/1644)). Keep three or more voters on fixed addresses -- a DHCP
reservation is enough -- or give the moved ones their old addresses back until a quorum
re-forms, then move them one at a time. To
promote one, read its `consensus-endpoint` from `fastcache-cli node` on
that machine, then on A:

```powershell
fastcache-compile-node --cluster-admit=<node-id>=<consensus-host>:6680
```

The key the cluster already holds for it stays. A promotion takes effect once the learner has
caught up; it then opens its consensus port, which its install already let through the firewall.
`fastcache-cli node` on any member prints the new `fleet-id`: put it in the new voter's own pin
and in the pin of every PC not installed yet (step 1).

### Taking a machine out

On A:

```powershell
fastcache-compile-node --cluster-forget=<node-id>
```

This removes the machine from the cluster and revokes the key it was admitted under, on every
node, with no restart anywhere: its launchers and its node are refused from then on, from any
address. The key is never admitted again; the machine falls back to a fleet of its own and does
not ask again. To bring the same PC back, stop its service, delete its
`%ProgramData%\fastcache-node` (it mints a new identity at its next start), start it, and
approve it again. Forgetting A, the only voter, is refused: promote another voter first.

### Two fleets that should be one

A PC that started alone, away from the office and unpinned, founded a fleet of its own. When it
later meets A's fleet and either side can prove the two were one fleet split in two, the side
that yields rejoins the other by itself, and a node shows `fleet-split-healing` meanwhile; a
machine the survivor never recorded waits on its `--enroll-list`. When neither side can prove
it, both raise `foreign-fleet-visible` and nothing moves: decide where each named machine
belongs, `--cluster-forget` it from the fleet it leaves, and approve it on the one it joins. A
pinned PC never founds a second fleet that anybody else joins, which is one more reason to pin.

## 7. When something looks wrong

Start on the machine whose builds are slow:

| Ask | Where | It tells you |
|---|---|---|
| `fastcache-cli node` | any machine | what this node is, its role and standing, its leader, its pin, its shared cache and the conditions it has raised |
| `fastcache-cli node-conditions` | any machine | every condition with its remedy; exits 1 when none is raised, so a loop over machines can test it |
| `fastcache-cli fleet members` | A | who the fleet has agreed is a member, from the leader; a node that does not lead says where to ask instead |
| `fastcache-compile-node --enroll-list` | A | who is waiting, and who an auto-approve window let in |
| `fastcache-compile-node --cluster-status` | any machine in the fleet: a PC's own node sends the question on to the leader | what the cluster has agreed, settings included |
| `fastcache-cli explain-admission <machine>` | A, or any machine with `--addr=office-a.example.com:6674` | why A admits or refuses that machine, naming every route that decided |
| `fastcache-cc --show-stats` | the PC | this PC's hits, misses, dispatches and every fall-back reason |

The conditions an office fleet meets most:

| Condition | Read it as |
|---|---|
| `enrollment-requests-waiting` | somebody installed a PC and nobody approved it yet |
| `enrollment-window-open` | an auto-approve window is admitting anyone who asks |
| `scheduler-unreachable` | this PC cannot reach the leader: the VPN, or A is down |
| `consensus-leader-silent` | this PC has heard no leader for over an hour, and refuses grants until it does |
| `host-name-reaches-only-this-machine` | this PC's name resolves to itself only; fix its DNS |
| `foreign-fleet-visible` | another fleet is heard here; see *Two fleets that should be one* |
| `fleet-split-healing` | two halves of one fleet are joining up again by themselves |
| `unserved-toolchain` | a client's compiler matches no worker's |
| `mixed-node-versions` | two builds of one wire are running: finish the upgrade |
| `shared-cache-unproven` | this PC is not reaching C |
| `shared-cache-unavailable` | C's shared tier will not open |
| `surface-not-accepting` | this node has stopped accepting connections; restart it |

For a scrape and a liveness probe, give A an admin surface in its configuration file
(`admin_listen: 6677`, loopback by default) and read `/metrics` and `/healthz` there. The fleet
page (`dashboard: true`) is served by a node that schedules, which on this shape is A alone, and
answered in full while it leads; to serve it beyond loopback, the node requires
`dashboard_token_file:` as well.

## 8. Upgrades are a flag day

Every node in the fleet runs the same build. A node of another wire version is refused rather
than misread, and a fleet on one wire at two builds raises `mixed-node-versions` on the leader.
[Upgrading a fleet](upgrading-a-fleet.md) lists, for each format change, what the new build
refuses by name and the step that clears it. The procedure:

1. **Stop the builds.** A build left running is not broken, only slow: a launcher and a node on
   different wires refuse each other, and the build compiles locally.
2. **Upgrade every machine on the same day**: A first, then C, then the PCs. The MSI carries the
   node, the launcher and the CLI, so one upgrade per machine moves all three, and each
   upgrade stops the node it replaces before it starts the new one. Machines on two discovery
   versions simply do not see each other, with no counter and no log line.
3. **Confirm** before releasing the builds: on each machine, `fastcache-cli node-metrics` shows
   the `*_refused_unsupported_version_total` counters, which stay at zero once nothing old is
   left talking to it. `mixed-node-versions` on A says the same for builds of one wire.

An upgrade keeps the service registrations and the remembered properties, and the first build
after one that moves the cache key misses once everywhere.

### Coming from 0.3.0

The go-live upgrade starts every machine afresh. On **every** machine, A included:

1. `Stop-Service FastCacheCompileNode`.
2. **Delete `%ProgramData%\fastcache-node`.** It holds the old identity key, which every local
   account could read: treat it as disclosed. The new install refuses a key other accounts could
   read rather than securing it, and the new node refuses one at startup. Deleting the directory
   also clears the old consensus state, formation record and roster file, which the new build
   would refuse to read.
3. In `%ProgramData%\fastcache-compile-node\fastcache-compile-node.yaml`, remove every key the
   new build refuses: `scheduler`, the admission-by-address keys `fleet_member`,
   `scheduler_token_file`, `cluster_admit_client` and `cluster_forget_client`, the cluster-shape
   keys `raft_peer`, `raft_join`, `cluster_id` and `serve_scheduler`, and the principal-mode
   keys `voter_key`, `enroll_from` and `cluster_admit_worker`. A node naming one refuses to
   start, and says which. The upgrade never replaces this file.
4. Install the new MSI. Installers up to 0.3.0 delete both services when they are removed, so
   the new one registers them again from scratch, carrying none of those flags. Such an installer
   remembered nothing, so this first upgrade registers only what it states: pass
   `FASTCACHE_FIREWALL_ALLOW` and `FASTCACHE_FLEET_SEED` again here if you use them. The
   properties `FASTCACHE_NODE_SCHEDULER` and `FASTCACHE_NODE_ADVERTISE` are no longer read; drop
   them from scripted installs. A failed upgrade puts both services back as 0.3.0 had them, unless
   rollback is disabled (the `DisableRollback` policy, or `DISABLEROLLBACK=1` on the command
   line): then both services are left unregistered, and running the upgrade again is the remedy.

Then follow steps 1 to 4 above, in order: A founds the new fleet and prints its new `fleet-id`
(the pin from before cannot be reused, since the cluster is new), every PC is upgraded with
that pin and asks to join, approve them (an `--enroll-auto-approve=15min` window suits the day),
and set `shared-cache` again. The first build afterwards is cold everywhere: the cache key moved.

### After go-live

A later release says in its notes whether it moves a format. When it does, the node refuses to
start by name (`UnsupportedFormatVersion`, never the damage code) and [Upgrading a
fleet](upgrading-a-fleet.md) has the step.

## 9. Costs this setup accepts

- **Debug paths name the checkout that built the object.** `cl` has no path-map switch, so a
  hit replayed from another checkout or another PC names that checkout's paths in its debug
  records. Point the debugger's source path at your own tree. Code, data and symbols are
  byte-identical; only the debug records differ, and keying on the location would end sharing
  between checkouts, which on Windows is most of the value.
- **`/Z7` objects are larger** than `/Zi` ones, since each carries its own debug information,
  so the cache holds fewer of them and a link reads more.
- **Every PC is a learner**, so the fleet survives any of them sleeping, and depends on A (and
  any voter you add) being up for scheduling and for every change to who is in it.

## 10. Known limits

Each of these is an open issue. None of them breaks a build; each costs speed, capacity, or an
assumption worth knowing about.

- **An unpinned first join trusts on first use**
  ([#1609](https://github.com/LASTRADA-Software/fastcached/issues/1609)). A machine installed
  with no `FASTCACHE_FLEET_ID` joins the fleet that proves itself first on its segment, once
  that fleet approves it. Pin every install (step 1).
- **The shared tier holds less than its budget when its values are small**
  ([#1623](https://github.com/LASTRADA-Software/fastcached/issues/1623)). The disk store frees
  a page only once every entry in it is gone, so a store of small values evicts more than it
  frees and swings well below `shared_cache_disk`. Most object files are past the size where
  this applies, so a compile cache sees little of it.
- **Pages a disk store leaks are lost to its budget for good**
  ([#1624](https://github.com/LASTRADA-Software/fastcached/issues/1624)). A crash can leave a
  few pages that nothing reclaims, and they count against `shared_cache_disk` (or `cache_disk`)
  from then on. The amounts are small on a node.
- **A Linux or macOS node waits up to 6 seconds for its own names before it serves**
  ([#1621](https://github.com/LASTRADA-Software/fastcached/issues/1621)). POSIX only, so it
  does not touch this shape; it matters only if such a machine joins the fleet.
- **A wrong dial hint costs every dispatched compile 300 ms**
  ([#1606](https://github.com/LASTRADA-Software/fastcached/issues/1606)). A client dials the
  address the scheduler last saw a worker at before the worker's name; behind a NAT or after a
  VPN address change, that address may be stale.
- **A dispatched compile's source reaches whatever answers at that address**
  ([#1626](https://github.com/LASTRADA-Software/fastcached/issues/1626)) before the worker has
  proved its key. A machine that is not the worker can no longer return an object the client
  accepts, but it does receive the source. Keep VPN DNS current.
- **A voter pinned to a name is reached again only when DNS catches up after its address
  changes** ([#1605](https://github.com/LASTRADA-Software/fastcached/issues/1605)); one left at
  the default follows its own announcement instead, so this applies only to a typed
  `raft_self` name. And **a learner can take up to 30 seconds to reach a newly elected leader**
  ([#1625](https://github.com/LASTRADA-Software/fastcached/issues/1625)). Both apply only once
  you have promoted more voters, which is why step 6 asks for machines that are always on.
- **`clang-cl` with `/Zi` is cached but never dispatched**
  ([#1602](https://github.com/LASTRADA-Software/fastcached/issues/1602)). `/Z7`, as step 5
  asks, is not affected.
- **One machine can hold the enrollment list full from many IPv6 addresses**
  ([#1603](https://github.com/LASTRADA-Software/fastcached/issues/1603)). The four-row bound is
  per address, and a host may own a whole /64. `--enroll-clear` and `FASTCACHE_FIREWALL_ALLOW`
  are the remedies today.
- **A `cluster_dir:` in the configuration file is not secured by the install**
  ([#1604](https://github.com/LASTRADA-Software/fastcached/issues/1604)): the node then refuses
  to start on it. Leave the state directory where the service puts it.
- **A repair that cannot add a firewall rule leaves fewer rules than before**
  ([#1612](https://github.com/LASTRADA-Software/fastcached/issues/1612)), since the old group
  is removed first. If a port stops answering after a repair, repair again.
- **It is unconfirmed whether a second Windows error ends an accept loop**
  ([#1627](https://github.com/LASTRADA-Software/fastcached/issues/1627)). If one does, the node
  raises `surface-not-accepting` and `/healthz` answers 503; restart the service.

See also [the fastcache-compile-node page](../tools/fastcache-compile-node.md) for each of
these in depth, and [fastcache-cc](../tools/fastcache-cc.md) for every fall-back reason a
client reports.
