# rcu-fib

**Lock-free RIB/FIB synchronisation for a routing control plane, in C++20.**

A forwarding table is read once per packet and written once per route change, by
different threads, forever. Those two access patterns have almost nothing in
common, and any scheme that makes them share one exclusion mechanism is
optimising for the wrong one.

The failure mode this exists to fix is specific: a BGP session flaps, the control
plane produces a burst of updates, and forwarding throughput collapses — not
because the new routes are wrong, but because programming them holds a lock that
every lookup also needs. The routes converge; the traffic does not survive the
convergence.

This implements the fix — epoch-based reclamation with lock-free readers — and
then measures it against the alternatives instead of asserting it is better.

---

## The result

200,000 prefixes, 4 reader threads, one writer reprogramming continuously:

| variant | idle M lookups/s | under churn | **degradation** | **p99 latency under churn** |
|---|---|---|---|---|
| `mutex` | 1.40 | 0.28 | 80.1% | 209,250 ns |
| `shared_mutex` | 3.26 | 0.47 | 85.7% | 133,583 ns |
| `seqlock` | 17.54 | 3.97 | 77.4% | 21,708 ns |
| **`rcu`** | **18.73** | **13.60** | **27.4%** | **792 ns** |

A writer running flat out costs the lock-based tables four fifths of their read
throughput. It costs the RCU table about a quarter, most of which is simply the
writer occupying a core.

**The tail is the stronger result.** Under identical churn the p99 lookup takes
792 ns under RCU and 209 µs under a mutex — a factor of 264. That is the
difference between a convergence event being invisible and being an outage: the
mean barely moves, and every packet that misses its deadline is in the tail.

And while a full 200,000-route table is being reprogrammed:

| variant | reprogram time | **lookups/s surviving during it** |
|---|---|---|
| `mutex` | 0.129 s | 0.66 M |
| **`rcu`** | **0.098 s** | **22.93 M** |

**35× more forwarding survives the convergence event.**

Full methodology, the update-rate sweep, and an honest account of variance:
[`docs/benchmarks.md`](docs/benchmarks.md).

---

## Quick start

```bash
git clone https://github.com/<you>/rcu-fib.git && cd rcu-fib
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

./build/rcufib_bench            # the table above
./build/rcufib info             # what the trie looks like
ctest --test-dir build          # 110 tests
```

Needs a C++20 compiler and CMake 3.24. No dependencies beyond GoogleTest, which
CMake fetches if it is not installed; zlib is used if present, to read gzipped
MRT dumps directly.

---

## What is implemented

### Epoch-based reclamation

Publishing a change to a lock-free structure is easy. Freeing what it displaced
is the hard part: a reader that loaded a node pointer one instruction before the
writer unlinked it is still walking that node, and no amount of atomicity on the
pointer helps, because the reader already has it.

EBR answers that with a counter. Readers announce which epoch they are reading
in; a writer that retired memory during epoch E waits until the global epoch has
advanced twice, at which point no reader can still hold a reference to it.

The read path is two relaxed stores and a fence — no atomic read-modify-write,
no cache line shared between readers, no path by which a writer can make a reader
wait:

```cpp
const std::uint64_t epoch = global_epoch_.load(std::memory_order_relaxed);
record.state.store(epoch | pinned_bit, std::memory_order_relaxed);
std::atomic_thread_fence(std::memory_order_seq_cst);
```

That fence is the entire correctness argument, and the reasoning is worth
stating: the reader stores its announcement then loads a pointer; the writer
stores the pointer then loads announcements. That is the store-buffer shape, and
without sequential consistency on **both** sides the writer can conclude no
reader is pinned at the moment a reader is about to walk into memory it is about
to free. Release/acquire does not help — the hazard is a store followed by a
load, and release/acquire orders neither against the other.

### A path-compressed LPM trie

Generic over address width, so IPv4 and IPv6 share one implementation.
Compression matters here for a reason specific to routing tables: the global
table is extremely sparse in its upper bits, and an uncompressed trie spends
most of its nodes on single-child chains consuming bits nobody branches on.

Measured on 50,000 generated prefixes: **1.80 nodes per prefix, mean lookup
depth 17**. Depth is what a lookup costs, because each level is a dependent load
and therefore a potential cache miss — not a comparison.

Structural changes publish through a single release-store, so a reader sees the
old shape or the new one and never a half-linked node. Freed memory goes to a
`Reclaimer` policy rather than `delete`, because whether immediate freeing is
safe is exactly the variable under study — which is what lets all four variants
share one trie.

### Four synchronisation strategies, measured against each other

| Variant | Reader cost | Blocks? | Reclamation |
|---|---|---|---|
| `mutex_fib` | exclusive lock | on everything | immediate |
| `shared_mutex_fib` | shared lock (atomic RMW) | on writes | immediate |
| `seqlock_fib` | two loads + retry loop | no, but retries | deferred |
| `rcu_fib` | two stores + fence | never | deferred |

The seqlock is a control rather than a competitor, and it makes a point worth
being explicit about: it removes the writer's exclusion of readers, but a reader
mid-walk can still hold a node the writer just unlinked, because it dereferences
that node *before* it re-reads the sequence counter. So it needs exactly the same
deferred reclamation RCU does — it inherits the hard part and adds retries on
top. The benchmark records 232 million of them in three seconds.

RCU serialises *writers* with a mutex. That is not a compromise, it is what RCU
is: it makes reads lock-free, not writes, and in a router that costs nothing
because one thread programs the FIB.

### RIB, FIB, and update coalescing

The RIB holds every route every protocol offered, including the losers; the FIB
holds only winners. That separation is not bookkeeping — when an IS-IS route is
withdrawn, the BGP route behind it must become the forwarding entry immediately,
and that is only possible if the loser was retained.

The synchroniser tracks **which prefixes changed, not what changed**. A flapping
session re-advertises the same prefix repeatedly; marking is idempotent, so fifty
updates to one prefix cost one FIB write and the intermediate states never reach
the forwarding plane at all. A prefix that is added and then withdrawn before the
flush is never programmed once.

---

## Verification

```bash
ctest --test-dir build                                            # 110 tests
cmake -S . -B build-tsan -DRCUFIB_SANITIZER=thread            && \
  cmake --build build-tsan && ./build-tsan/rcufib_tests           # clean
cmake -S . -B build-asan -DRCUFIB_SANITIZER=address+undefined && \
  cmake --build build-asan && ./build-asan/rcufib_tests           # clean
```

Clean under ThreadSanitizer and under AddressSanitizer + UBSan. For an RCU
implementation those runs are the point — the failure modes here (a reader
following a pointer the writer just freed, a half-built node becoming visible)
do not reliably reproduce without instrumentation.

The concurrency tests turn on one invariant: the writer derives every entry's
gateway from its generation by a fixed function, so any entry a reader observes
must satisfy that relation. A torn read, a freed entry or a partially published
node all break it. Two tests are worth naming:

- `APinnedReaderKeepsRetiredMemoryAlive` takes a raw pointer inside a read
  guard and keeps dereferencing it while the writer replaces and retires it
  5,000 times. Under ASan a reclamation bug here is a hard failure, not a flake.
- `ALongPinnedReaderStallsReclamationButNotWriters` demonstrates EBR's
  documented weakness rather than hiding it: a stalled reader holds up freeing
  for everyone, the backlog grows, writers proceed regardless, and it drains the
  moment the reader leaves.

The trie is differentially tested against a brute-force LPM reference across
thousands of interleaved inserts and erases.

---

## The tool

```
rcufib info    [--prefixes N] [--mrt PATH]          table shape and depth
rcufib lookup  [--prefixes N] [--mrt PATH] ADDR...  resolve addresses
rcufib verify  [--prefixes N]                       check against brute force
rcufib dump    [--prefixes N] [--limit N]           print the table
```

`rcufib info` prints the mask-length distribution and the lookup-depth
histogram:

```
prefixes           50000
trie nodes         89758 (1.80 per prefix)
mean lookup depth  17.17 nodes
max lookup depth   23 nodes

mask length distribution
  /16      2493    5.0%  #########
  /20      2808    5.6%  ###########
  /22      4934    9.9%  ###################
  /23      3714    7.4%  ##############
  /24     26625   53.2%  ##########################################################
```

Benchmarking an LPM structure on uniformly random prefixes flatters it badly.
The real global table is more than half /24s and clusters into deep sparse
subtrees, because prefixes are carved out of allocations — both change the shape
of the trie and the cost of a walk. The generator reproduces both, so the
numbers can be reproduced offline; `--mrt` swaps in a real RouteViews or RIPE RIS
snapshot to confirm the synthetic table is representative rather than convenient.

---

## Layout

```
include/rcufib/
  epoch.hpp      epoch-based reclamation: guards, retire, grace periods
  trie.hpp       path-compressed LPM trie with a pluggable Reclaimer
  fib.hpp        the four synchronisation strategies
  rib.hpp        RIB with best-path, and the coalescing synchroniser
  address.hpp    fixed-width addresses, generic over bit count
  prefix.hpp     prefixes, parsing, containment
  route.hpp      next-hops, protocols, administrative distance
  synthetic.hpp  a route table shaped like the real one
  mrt.hpp        RFC 6396 dumps from RouteViews / RIPE RIS
bench/           the measurement harness
tests/           110 tests, including the sanitizer targets
tools/           the inspection CLI
docs/            design.md, benchmarks.md
```

---

## Honest limitations

- **Writers are not lock-free.** Multiple writers serialise on a mutex. Making
  them lock-free would multiply the complexity for a case a router does not have.
- **EBR defers reclamation.** A reader that pins an epoch and then stalls holds
  up freeing for every thread. For bounded lookups that is a good trade; for
  readers that might block, hazard pointers bound memory better at the cost of a
  per-read store.
- **The benchmark is a laptop, not a router.** Absolute throughput on real
  forwarding hardware would be dominated by things not modelled here. The
  comparison between strategies transfers; the absolute figure does not.
- **No thread pinning on macOS.** Threads migrate between performance and
  efficiency cores, which widens every variant's tail. Run-to-run variance in
  the degradation figures is 5–8 percentage points; the ordering is stable, the
  third significant figure is not.

---

## Background

I spent three years writing control-plane software for a carrier-grade router:
BGP best-path and EVPN, SRv6 endpoint behaviours, SR-MPLS FRR and LFA, and the
RIB-to-FIB path this project reconstructs. The original was the work described
in the first table — large BGP updates stalling forwarding-table programming,
rebuilt around an RCU-style model with lock-free readers and atomic
copy-and-swap. This is that idea implemented from scratch, in public, with the
measurements attached.

See also [NetOptix](https://github.com/Maneesha9801/NetOptixx), which observes
the control plane this one programs.

## License

MIT — see [LICENSE](LICENSE).
