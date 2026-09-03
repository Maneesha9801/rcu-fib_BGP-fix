# Benchmarks

## What is measured, and why

Three questions, in the order an engineer asks them:

1. How fast does each table look up when nothing is changing?
2. **How much of that survives while the control plane reprograms it?**
3. How long does a full table reprogram take with the dataplane running?

The second is the one that matters. Every design here looks acceptable on the
first.

## Methodology

Run it yourself:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/rcufib_bench --prefixes 200000 --readers 4 --duration 3000 --sweep
```

**Fairness.** All four variants wrap the same trie, hold the same entries, and
receive the same prefixes, the same lookup keys and the same churn sequence
from one generated dataset. A difference in the numbers is a difference in
synchronisation and nothing else.

**The reader cannot be optimised away.** Every lookup folds a field of the
result into a per-thread accumulator that is published at the end, so the
compiler cannot elide the walk.

**Latency sampling does not distort throughput.** Timing every lookup would
cost more than the lookup does, so one in 128 is timed and the rest are only
counted. Percentiles use nearest-rank over the exact samples — no interpolation,
so no reported value was ever un-measured.

**Warm-up is discarded.** Counters reset after a 300 ms warm-up, once the
threads are running and the caches are populated.

**The table is realistic.** Uniformly random prefixes flatter an LPM structure
badly: the real global table is more than half /24s and clusters into deep,
sparse subtrees because prefixes are carved out of allocations. The generator
reproduces both. `rcufib info` prints the distribution it produced, and
`--mrt PATH` swaps in a real RouteViews or RIPE RIS snapshot to check the
synthetic table is not quietly flattering anything.

## Results

Apple M-series, 4 performance cores and 4 efficiency cores, Apple clang 21,
`RelWithDebInfo`. 200,000 prefixes, 4 reader threads, one writer, batch size 64,
3-second measured window.

### Idle: readers only

| variant | lookups M/s | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|
| mutex | 1.40 | 1375 | 41,042 | 100,000 |
| shared_mutex | 3.26 | 333 | 12,208 | 29,958 |
| seqlock | 17.54 | 208 | 625 | 2,875 |
| rcu | **18.73** | **208** | **625** | 1,666 |

Note what the lock costs before a single write has happened. `mutex` serialises
readers against each other, so four threads deliver less than one thread's worth
of work. `shared_mutex` lets them share, but its own bookkeeping is an atomic
read-modify-write on a cache line all four are hammering — which is why it
reaches 3.26 M/s and not 4× the single-thread rate.

### Under continuous churn

| variant | lookups M/s | updates/s | p50 ns | p99 ns | p99.9 ns | retries |
|---|---|---|---|---|---|---|
| mutex | 0.28 | 1,725,415 | 1833 | 209,250 | 417,500 | — |
| shared_mutex | 0.47 | 1,540,641 | 667 | 133,583 | 293,750 | — |
| seqlock | 3.97 | 1,995,256 | 250 | 21,708 | 47,666 | 232,259,360 |
| rcu | **13.60** | 1,653,356 | **209** | **792** | 4,667 | 0 |

### The headline

| variant | idle M/s | under churn | degradation |
|---|---|---|---|
| mutex | 1.40 | 0.28 | **80.1%** |
| shared_mutex | 3.26 | 0.47 | **85.7%** |
| seqlock | 17.54 | 3.97 | **77.4%** |
| rcu | 18.73 | 13.60 | **27.4%** |

A writer running flat out costs the lock-based tables four fifths of their read
throughput. It costs the RCU table around a quarter, and most of that is simply
the writer occupying a core.

**The tail latency result is the stronger one.** Under the same churn, the p99
lookup takes 792 ns under RCU and 209,250 ns under a mutex — a factor of 264.
That is the difference between a convergence event being invisible and being an
outage: the mean barely moves, and the packets that miss their deadline are all
in the tail.

The seqlock's 232 million retries in three seconds are the visible form of its
problem. It never blocks, but under sustained churn it spends most of its time
restarting walks it had almost finished.

### Full table reprogram, dataplane running

| variant | routes | seconds | routes/s | reader M/s during |
|---|---|---|---|---|
| mutex | 200,000 | 0.129 | 1,552,758 | 0.66 |
| shared_mutex | 200,000 | 0.132 | 1,519,139 | 1.60 |
| seqlock | 200,000 | 0.107 | 1,873,972 | 10.98 |
| rcu | 200,000 | **0.098** | **2,030,615** | **22.93** |

RCU installs the table about 25% faster — the writer is not fighting readers for
a lock. But the interesting column is the last one: while the table is being
replaced, the dataplane keeps running at 22.9 M lookups/s under RCU and 0.66 M/s
under a mutex. **35× more forwarding survives the convergence event.**

### Read throughput against update rate

| variant | 1k/s | 10k/s | 100k/s | unbounded |
|---|---|---|---|---|
| mutex | 29.5% | 23.4% | 24.1% | 80.4% |
| shared_mutex | 12.5% | ~0% | 18.0% | 88.2% |
| seqlock | 3.8% | 12.4% | 14.3% | 78.0% |
| rcu | 9.4% | 5.9% | 12.4% | **26.8%** |

Degradation relative to that variant's own idle throughput. The lock-based
tables lose double digits at rates a single BGP session can produce, and fall
off a cliff when the writer is unthrottled. RCU degrades gradually and never
collapses.

## Honesty about these numbers

**Run-to-run variance is real.** The machine has four performance cores and the
benchmark runs four readers plus a writer, so it is oversubscribed by design —
that is the interesting case, but it costs stability. Degradation figures move
by 5–8 percentage points between runs; RCU's churn degradation has been observed
between 18% and 27% across runs of the same binary. The *ordering* is stable and
the order-of-magnitude gaps are stable; the third significant figure is not.
Some cells show small negative degradation, which simply means the measurement
noise exceeded the effect.

**This is a laptop, not a router.** Absolute throughput on a real forwarding
plane would be dominated by things this benchmark does not model — NUMA, DDIO,
packet I/O, hardware offload. The comparison between synchronisation strategies
is what transfers; the absolute Mpps figure is not.

**No thread pinning.** macOS does not expose affinity control, so threads move
between performance and efficiency cores, which widens the tails for every
variant. On Linux the same benchmark under `taskset` produces tighter
distributions and, in my testing, the same ranking.

**The mutex baseline is not a straw man.** It batches updates under one lock
acquisition, which is the first optimisation anyone reaches for, and it is
measured with the same batch size as everything else. It still loses, because
batching reduces how *often* readers stall without changing the fact that they
do.

**RCU is not free.** It defers reclamation, so memory is held longer, and a
reader that pins an epoch and then stalls holds up freeing for every thread.
`ALongPinnedReaderStallsReclamationButNotWriters` demonstrates exactly this: the
backlog grows while writes continue unimpeded, and drains the moment the reader
leaves. For a dataplane whose readers are short bounded lookups the trade is
overwhelmingly favourable; for readers that might block, hazard pointers would
be the better answer.

## Reproducing against the real global table

```bash
curl -O https://routeviews.org/bgpdata/2024.01/RIBS/rib.20240101.0000.bz2
bzcat rib.20240101.0000.bz2 > rib.mrt
./build/rcufib info --mrt rib.mrt          # distribution and trie shape
./build/rcufib_bench --mrt rib.mrt --prefixes 1000000
```

The prefix-length histogram `rcufib info` prints from a real dump and from the
generator should look the same — that is the check that the synthetic table is
representative rather than convenient.
