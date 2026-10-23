# Bounded-cycle detection changes

Base MCAAT commit: `44cfd8815504b9af57734aa7b27681f7aae0e3f7`.
Base MEGAHIT submodule: `5f329c6d24a1480b75145a4c14567a25453b95bf`.

## Contract and behavior

The new detection kernel answers whether there is a nonempty simple cycle
through the requested root with length **at most** the supplied bound, under
the existing root-multiplicity predicate. It does not enforce minimum length
and does not enumerate paths. Graph validity and multiplicity must remain
unchanged throughout the candidate-detection phase.

Self-loops are accepted by the standalone detector when the bound is at least
one. Candidate selection retains the existing policy: multiplicity strictly
above the threshold, indegree at least two, and no incoming self-loop.
Multiplicity zero is inadmissible. Negative/zero length bounds return false.

The former `limit / 2 + 1` rounds could accept cycles beyond the upper bound.
That behavior is deliberately corrected; results are not intended to be
bit-for-bit identical to the old prefilter.

## Exactness argument

After separately checking a self-loop, let A be the admissible outgoing
neighbors and P the admissible incoming neighbors of the root s. Search G-s
for an A-to-P path of length at most L-2. A simple cycle yields such a path;
a shortest such path is simple and closes through s to produce a cycle.

Both searches retain exact visited membership. Each expansion completes one
BFS layer. Their visited balls are disjoint until success. If their radii
sum to L-2 without an intersection, any qualifying path would have had a split
covered by both balls, a contradiction. Exhausting either frontier without
an intersection also proves absence. Direction selection affects work order,
not completeness. An opposite-side hit already certifies admissibility, so
no multiplicity lookup is needed for that hit. The final layer only checks
intersections and does not retain discoveries that will never be used.

## Implemented

- Smaller-frontier expansion with exact combined-depth accounting.
- One SDBG adjacency call per expanded vertex, without caller-side duplicate
  zero-degree and validity checks. SDBG itself still validates neighbors.
- Root predicate converted exactly to `neighbor >= root / 501 + 1`.
- MEGAHIT compact-multiplicity comparisons avoid its large-value hash table;
  layout is dispatched outside the candidate-detection phase.
- 32-/64-bit frontier IDs selected once per candidate-detection phase.
- Explicit worker scratch, a forward bitmap reset through touched words,
  an exact flat backward set, and reusable frontier vectors.
- Owner-thread allocation of BFS bitmap storage. BFS scratch is released
  before enumeration bitmap allocation in the normal pipeline.
- One persistent OpenMP team with continuous dynamic worksharing in chunks of
  20000 graph IDs, matching the original chunk size. No candidate-window barriers,
  candidate staging arrays, or candidate-record copies.
- Root multiplicity and incoming neighbors reused from candidate selection.
- Progress is published with at most one relaxed atomic addition per completed
  20000-ID chunk. Approximately every million completed valid edges, a reporting
  critical region prints the count and elapsed scan time. Other workers continue
  processing; there is no global reporting barrier or per-node atomic counter.
- The normal pipeline skips the second multiplicity-filter pass: its earlier
  filter already invalidates all edges with multiplicity <= 1, and intervening
  tip pruning only invalidates edges. Standalone candidate selection retains its
  preprocessing default. The existing upfront low-abundance policy is unchanged.
- Fixed multiplicity buckets, integer bucket calculation, and reserved merging.
  Results merge in worker order without an additional global candidate sort.
- Separate wall-clock summaries for candidate scanning/BFS/merging, enumeration,
  and serialization/result copying. Candidate counts are worker-local.
- Positive thread-count validation and a 64-bit valid-edge statistic.
- Optional release IPO/LTO, optional forced loop unrolling/prefetching, and
  an explicit native-CPU build option. User release flags are no longer replaced.

## Limits and remaining work

The forward bitmap still costs one bit per global graph ID per active worker.
Hash/frontier storage depends on search size and retains its capacity until the
worker exits the detection phase. There is no total-RAM admission controller or
external-memory fallback. Accepted-root output and total search memory are not
claimed to be bounded independently of input.

The later enumerator is not rewritten here. Its existing cycle caps,
worker-history suppression, narrowing of relaxation IDs, and result retention
still require a separate correctness change. Root ordering remains dependent
on worker assignment, as in the original scheduler. Thread-count independence
or byte-for-byte reproduction of final array output is not claimed.

The following measurement-dependent proposals are not enabled automatically:
new hash/bitmap representations, graph caches, compressed paths, multi-root
bit-parallel searches, explicit prefetching/SIMD, forced affinity/huge pages,
allocator replacement, and PGO. They are not universally faster and should be
evaluated on the execution server. No graph, test binary, compiler, or CMake
configuration was run on the user's PC for this change.

## Dependency fork handoff

The MEGAHIT checkout currently has a local modification to `src/sdbg/sdbg.h`.
`patches/megahit-cycle-multiplicity.patch` preserves it in the parent project:
ordinary parent commits do not capture uncommitted submodule file changes.
The accessor uses a constant template branch compatible with the library's
older C++ callers; the new MCAAT search kernel requires C++17.

Until a fork URL and commit are supplied, `.gitmodules` and the submodule
commit remain unchanged. Root CMake configuration now applies this patch
automatically when the accessors are missing, before configuring dependencies.
Already-patched headers are left alone. An incompatible local edit produces
an actionable configuration error instead of cascading compiler errors; the
build never resets the submodule or discards edits.

For an older checkout without this configuration fix, apply manually:

```sh
git -C libs/megahit apply ../../patches/megahit-cycle-multiplicity.patch
```

Do not apply this again to the already-modified local checkout. Once the fork
contains the accessor, update the submodule URL and recorded commit to that
fork and remove the temporary patch as appropriate.

## Server-side validation (not executed here)

The standalone tests need only a C++17 compiler and CMake, and exercise the
same generic search kernel with a mock graph and exact standard-library set.
They compare against an independent simple-path oracle for all 4096 loop-free
directed graphs on four vertices, every root, and bounds zero through five,
for both ID widths. They also cover cached incoming neighbors, predicate masks,
invalid vertices, self-loops, strict odd/even bounds, reused state, bitmap word
boundaries, graph resizing, and rejecting too-narrow IDs before allocation.

Run only on the intended execution machine:

```sh
cmake -S tests -B build/cycle-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/cycle-tests
ctest --test-dir build/cycle-tests --output-on-failure
```

Tests use explicit failures rather than `assert`, so Release does not disable
their checks. These tests do not cover actual SDBG files, the phmap backend,
or the OpenMP scheduling wrapper. Also validate compact/full multiplicity storage
(especially 0, threshold boundaries, 253, 254, 255, 65535), candidate sets
across worker counts, and full-build integration on the server before use.

Example release configuration, after applying the dependency patch:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release \
  -DMCAAT_ENABLE_IPO=ON -DMCAAT_NATIVE_ARCH=ON \
  -DMCAAT_AGGRESSIVE_LOOPS=OFF
cmake --build build/release --parallel 4
```

Build on the execution CPU or disable `MCAAT_NATIVE_ARCH` for portable builds.
IPO is optional and configuration reports an error when requested but unsupported.
Do not interpret either configuration as a measured speedup.

## Runtime regression follow-up

The first optimization revision introduced a global barrier after each window
of 262144 scanned IDs, plus a global sort of accepted IDs. The user reported a
slowdown in scanning. The window barriers, staging copies, and sort
have been removed conservatively; cheap timers now separate detection,
enumeration, and output. This correction has not been compiled or benchmarked.

Other variables still requiring controlled comparison are adaptive direction
selection, the changed compiler-loop flags, worker count, build configuration,
bitmap allocation/placement, and enumeration order. The cycle detector's exact
upper bound also differs from the old permissive bound. Compare candidate and
output counts alongside runtime. Use the same saved graph, thread count, and
server load for detector comparisons; whole FASTQ runs also time graph building.
Do not infer a successful speedup from removal of overhead alone.

### Progress reporting and comparable inputs

The supplied run logs describe different graphs: 1099909480 versus 904132355
total IDs, and 495151953 versus 325217706 valid edges after pruning. The first
has about 22% more total IDs and 52% more surviving edges. Neither log includes
the completed scan time. The previous optimized revision did not print progress
while scanning, so its silence cannot be compared with the original build's
million-edge messages. These logs do not establish either a speedup or a slowdown.

Progress now counts valid root edges in completed chunks, including roots that
fail candidate selection. It does not count BFS discoveries or accepted roots.
It can lag by the unfinished chunks (up to 20000 IDs per worker). Chunks can
complete out of ID order; the reported count is not a highest visited node ID.
Printing is serialized only among milestone reporters, and concurrent milestones
may be combined into one newer count. The final summary reports the exact total.

The repeated filter shown in both logs counts every low-multiplicity edge again,
including already-invalid ones. Skipping that second pass in the normal pipeline
removes a full scan without changing the graph presented to candidate selection.
It does not establish the cause of any slowdown inside BFS.

Compare both binaries sequentially on the exact same saved graph, with matching
26 threads, length bounds, multiplicity threshold, and low-abundance setting.
Use separate output directories and `--autoclean false` to retain graph data.
Compare scan completion times and counts rather than matching progress messages
from different graphs. Loading times in the supplied logs (22.0 versus 5820.3
seconds) are also very different; storage/cache/environment causes have not been
measured, and those times must be kept separate from cycle scanning.

The progress/filter change has been inspected statically only; no build, test,
or graph processing was run on the user's Mac.
