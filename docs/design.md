# Design

## The problem

A forwarding table is read once per packet and written once per route change,
by different threads, forever. Those two access patterns have almost nothing in
common: reads are short, enormously frequent, and must never stall; writes are
rare by comparison, bursty, and structurally complicated. Any scheme that makes
them share one exclusion mechanism is optimising for the wrong one.

The failure mode this project is built around is specific and unglamorous. A
BGP session flaps, the control plane produces a large burst of updates, and the
forwarding plane's throughput collapses — not because the new routes are wrong,
but because programming them holds a lock that every lookup also needs. The
routes converge; the traffic does not survive the convergence.

## Epoch-based reclamation

### Why reclamation is the hard part

Publishing a change to a lock-free structure is easy: build the new node fully,
then release-store a pointer to it. A reader either loads the old pointer or the
new one, and both are consistent.

Freeing what the change displaced is the hard part. A reader that loaded a node
pointer one instruction before the writer unlinked it is still walking that node.
No amount of atomicity on the pointer helps — the reader already has it. The
writer needs to know when every reader that *could* be holding it has finished,
and nothing in the pointer itself carries that information.

### The mechanism

Readers announce which epoch they are reading in. A writer that retired memory
during epoch E waits until the global epoch has advanced twice: at that point
every reader is pinned to E+1 or later, so nobody can still hold a reference to
anything unlinked during E.

```
        global epoch:  E              E+1            E+2
                       |               |              |
  writer retires ------X               |              |  free X here
                       |               |              |
  reader A      [--pinned E--]         |              |
  reader B              [------pinned E+1------]      |
  reader C                             [---pinned E+2---]
```

Three epochs is the minimum. Two would mean a writer wanting to advance had
nowhere to put newly retired objects that was not also the bag it was about to
free.

### The read path

```cpp
const std::uint64_t epoch = global_epoch_.load(std::memory_order_relaxed);
record.state.store(epoch | pinned_bit, std::memory_order_relaxed);
std::atomic_thread_fence(std::memory_order_seq_cst);
```

Two relaxed stores and a fence. No atomic read-modify-write, no cache line
shared with another reader, no possibility of blocking. The epoch and the pinned
flag live in one word so that a writer scanning records cannot observe half of
an announcement.

**The fence is the entire correctness argument**, and it is worth being explicit
about why a weaker one will not do. The reader stores its announcement and then
loads a shared pointer. The writer stores the new pointer and then loads reader
announcements. That is the store-buffer shape:

```
  reader:                          writer:
    store announcement               store new pointer
    load  pointer                    load  announcements
```

Without sequential consistency on both sides, both loads may be satisfied from
before the corresponding stores became visible. The writer sees no pinned
reader, frees the old node, and the reader — which had not yet published its
announcement when the writer looked — walks into freed memory. A `seq_cst`
fence on each side is what forbids that interleaving. Release/acquire is not
enough, because the hazard is a store followed by a load, and release/acquire
orders neither against the other.

Unpinning is a release store, which is sufficient: it only needs to ensure the
reader's loads complete before a writer observes it as idle.

### The cost

The honest accounting:

| | Cost |
|---|---|
| Read | 2 relaxed stores, 1 fence, no contention |
| Write | Serialised against other writers by a mutex |
| Free | Deferred by up to two epoch advances |
| Worst case | A reader that pins and then stalls delays reclamation globally |

That last row is EBR's real weakness, and the tests exercise it directly:
`ALongPinnedReaderStallsReclamationButNotWriters` holds an epoch open and
asserts that the backlog grows while writes continue unimpeded. For a dataplane
whose readers are short, bounded lookups, this is a trade heavily in our favour.
For a system where a reader might block on I/O while pinned, hazard pointers
would be the better answer — they bound memory at the cost of a per-read store
and a scan.

### Thread lifetime

Records are allocated on first use, reused after their thread exits, and never
freed before the domain — a writer may be scanning a record at the instant its
thread exits. When a thread does exit it hands whatever is still in its bags to
the domain as orphans, because it will not be back to free them.

The domain slot carries a generation counter. Without it, a thread that exits
after its domain was destroyed would call into freed memory; with it, the thread
sees a stale generation and leaves the domain alone.

## The trie

A path-compressed binary trie, generic over address width so IPv4 and IPv6 share
one implementation.

Path compression matters here for a reason specific to routing tables. The
global table is extremely sparse in its upper bits: an uncompressed binary trie
would spend most of its nodes on single-child chains that exist only to consume
bits nobody branches on. Measured on a 50,000-prefix generated table, compression
gives **1.80 nodes per prefix and a mean lookup depth of 17 nodes**. Depth is
what a lookup actually costs, because each level is a dependent load and
therefore a potential cache miss — not a comparison.

### Publication

Every structural change is a single release-store of a pointer into a slot that
readers load with acquire. The two interesting cases:

**Splitting.** When a new prefix and an existing node diverge, a branch node is
built with both as children and then published with one store. A reader sees
either the old subtree or the new one — never a branch node with one child
linked and the other not.

**Value replacement.** Re-advertising an existing prefix swaps one pointer and
retires the old value. No structural change, no node copied, no reader
disturbed. This is the common case during churn and it is deliberately the
cheapest one.

### The Reclaimer policy

The trie hands freed memory to a policy rather than calling `delete`:

```cpp
template <class Address, class Value, class Reclaimer = immediate_reclaim>
class radix_trie;
```

`immediate_reclaim` frees at once, which is correct precisely when readers are
excluded during mutation. `epoch_reclaim` defers. This is what lets the four
FIB variants share one trie: the difference between them is *when freeing is
safe*, and that is exactly what the policy expresses.

## The four variants

| Variant | Reader cost | Blocks? | Reclamation |
|---|---|---|---|
| `mutex_fib` | exclusive lock | yes, on everything | immediate |
| `shared_mutex_fib` | shared lock (atomic RMW) | yes, on writes | immediate |
| `seqlock_fib` | two loads + retry loop | no, but retries | deferred |
| `rcu_fib` | two stores + fence | never | deferred |

The seqlock is a control rather than a competitor, and it makes a point worth
stating plainly: it removes the writer's exclusion of readers, but a reader
mid-walk can still hold a node the writer just unlinked, because it dereferences
that node *before* it re-reads the sequence counter. So it needs exactly the
same deferred reclamation RCU does. It inherits the hard part and adds retries
on top — and under sustained churn the benchmark records hundreds of millions
of them.

RCU serialises writers with a mutex. That is not a compromise; it is what RCU
is. It makes *reads* lock-free, and in a router that costs nothing, because one
thread programs the FIB.

## RIB, FIB and the synchroniser

The RIB holds every route every protocol offered, including the losers. The FIB
holds only winners. Keeping them separate is not bookkeeping: when an IS-IS
route is withdrawn, the BGP route sitting behind it must become the forwarding
entry immediately, and that is only possible if the loser was retained.

The synchroniser between them is where convergence time is won. It tracks
**which prefixes changed, not what changed**:

```cpp
void mark_dirty(const prefix_type& prefix);   // idempotent
std::size_t flush(std::size_t limit = 0);     // resolve each once, against the RIB
```

A session flap re-advertises the same prefix repeatedly — added, withdrawn,
re-added by a different path. Programming each of those into the forwarding
plane in turn is work the dataplane pays for and nobody benefits from. Marking
is idempotent, so fifty updates to one prefix cost one FIB write, and the
intermediate states never reach the hardware at all. `RepeatedMarksCollapseToOneWrite`
asserts exactly that, and `AnAddFollowedByAWithdrawalCostsNothingButARemove`
asserts the stronger version: a prefix that appeared and vanished before the
flush is never programmed once.

Flushes are bounded. An unbounded flush would hold the writer inside a single
batch for as long as the burst lasts — and for the lock-based variants, that is
precisely how long readers stall.

## What is deliberately not here

- **No lock-free writers.** Multiple writers serialise on a mutex. Making
  writers lock-free would multiply the complexity for a case a router does not
  have.
- **No hazard pointers.** They are the right answer when a reader might stall
  while holding a reference; a bounded lookup is not that case.
- **No DIR-24-8 or other O(1) lookup structure.** They win on lookup depth and
  lose badly on update cost and memory, and update cost is the axis under study
  here. Swapping the trie for one would be a data-structure project, not a
  synchronisation one.
