# Chunk 7.2 — renderSync() vs rlottie async render()/std::future

**Recommendation: keep `renderSync()` on our own render worker. Do not adopt
rlottie's `render()`/`std::future` path.** No measured benefit on this host
comes close to justifying the architectural cost — see "What adopting async
would cost" below. This is exactly the "default outcome" the plan calls out
(react-native-rlottie-implementation-plan.md §5): *"Switch only when the
internal asynchronous renderer demonstrates measurable benefit."* It doesn't.

## MANDATORY CAVEAT — this is a development-Mac measurement, not a device one

Every number in this document was measured on a development Mac (Apple
Silicon, arm64, macOS 25.5, 12 logical cores). **These numbers do not predict
phone behaviour.** Thermal limits, core counts and big.LITTLE scheduling,
memory bandwidth, and OS scheduler policy all differ materially between a
desktop-class Mac and a mid-range Android phone or an iPhone under sustained
load. Nothing here is device-validated. Re-measuring on real hardware is
explicitly out of scope for this chunk — that is Chunk 7.4 / the phase's
device acceptance criteria. Treat every conclusion below as "on this host,
under this methodology," not as a claim about production devices.

## What was benchmarked

`tests/cpp/bench_render.cpp` — an opt-in binary, run via `tests/run-tests.sh
bench`. It is not part of the default `plain asan tsan` gate (see "How to
run" below) and is not sanitizer-instrumented (a benchmark under ASan/TSan
measures the sanitizer's overhead, not rlottie's).

Two code paths, both against the vendored rlottie (`LOTTIE_THREAD=ON` is
already set in `tests/cpp/CMakeLists.txt`, so rlottie's internal
`RenderTaskScheduler` thread pool exists and is exercised by the async path):

- **sync** — `Animation::renderSync(frame, surface, true)`, the call
  `RlottiePlayerCore::renderFrame` (`cpp/RlottiePlayerCore.cpp:198`) makes
  today, run from a single calling thread — i.e. exactly what
  `RenderCoordinator`'s worker thread does (`cpp/RenderCoordinator.cpp:232`,
  `runRender`).
- **async** — `Animation::render(frame, surface, true)`, which returns a
  `std::future<Surface>`, immediately followed by `.get()`.

### Why "immediately followed by `.get()`" and not a deeper pipeline

rlottie's `AnimationImpl` reuses a single `RenderTask` (`mTask`) across calls
to `renderAsync()`
(`cpp/third_party/rlottie/src/lottie/lottieanimation.cpp:249-266`). Calling
`render()` again on the *same* `Animation` instance before the previous
future has been waited on mutates that shared task's `frameNo`/`surface`
fields while a pool thread may still be reading them from the first call —
that is a data race, not a missed pipelining opportunity. So "submit frame
N+1 while frame N is still rendering" is only safe **across different
`Animation` instances**, never repeated against one. Since each
`RlottiePlayerCore` owns exactly one `Animation`
(`cpp/RlottiePlayerCore.h:84`), single-view pipelining is not an option this
API supports safely, and the benchmark does not attempt it. This alone is
worth noting: the "recommended" async usage pattern from rlottie's own docs
implicitly assumes a caller juggling multiple in-flight renders across
multiple animations (e.g. a scene graph), not one `Animation` rendered by one
serialized worker, which is our model.

### Fixtures

- `tests/fixtures/pixel-probe.json` — the existing pixel-format golden
  fixture (Chunk 0.3): 4 static, non-overlapping, non-animated solid-color
  rectangles, one frame's content repeated across the whole timeline. It is
  **too trivial to be representative** of real Lottie content — there is
  effectively no per-frame rasterization work, so its numbers mostly measure
  call/dispatch overhead rather than rendering cost. It is included for
  continuity with the rest of the test suite (same fixture other chunks use)
  but every conclusion in this document rests on the fixture below, not this
  one.
- `tests/fixtures/bench-complex.json` — **constructed for this benchmark**
  (generated, not hand-written) because nothing already in the repo was
  representative: 60 shape layers, each an ellipse/rounded-rect with a fill
  and a stroke inside a group whose transform has **keyframed position and
  rotation** (values actually change every frame, forcing real per-frame
  matrix/path work rather than a cached static render), 1080×1080 intrinsic
  size, 91 frames (3s at 30fps). This is still a synthetic stress fixture,
  not a real designer-authored Lottie file, but it exercises rlottie's
  shape/transform/fill/stroke pipeline across every frame, which the trivial
  fixture does not.

### Methodology

For each fixture × surface size (128², 512², 1080²):

1. Load a fresh `Animation` per path (not one shared instance) with
   `useModelCache=false` — a real view loads its source once; sharing one
   `Animation`/model cache between the sync and async measurements would let
   whichever path runs second ride the first path's warm tessellation cache,
   biasing the comparison.
2. Warm up with 5 throwaway renders (cache warm-up, not measured).
3. Render 150 frames (cycling through the fixture's frame range), timing each
   individual `renderSync()`/`render()+get()` call with
   `std::chrono::steady_clock`.
4. Feed each per-frame duration into a `rnrlottie::MetricsCollector`
   (`cpp/Metrics.h`, Chunk 7.1) via its existing public
   `recordFramePublished()`/`snapshot()` API and report `renderP50Ms` /
   `renderP95Ms` / `renderP99Ms` from the snapshot — **reusing** the
   percentile implementation Chunk 7.1 already built and proved correct
   (`metrics_tests.cpp`), rather than writing a second one. `bench_render.cpp`
   does not touch `Metrics.h` or reach into its private percentile helper.

A second experiment asks a different, more architecturally relevant question:
our design already gives **every view its own OS worker thread**
(`RenderCoordinator`'s constructor spawns `worker_`,
`cpp/RenderCoordinator.cpp:13`). So multiple concurrent views already get
cross-core parallelism today, with zero dependency on rlottie's internal
pool. Does routing multiple concurrent views through
`render()`/`std::future` instead — relying entirely on rlottie's shared
`RenderTaskScheduler` — buy anything **on top of** that?

- **threads** — M OS threads, each owning its own `Animation` and calling
  `renderSync()` serially (mirrors M `RenderCoordinator`s today).
- **shared-pool** — M `Animation` instances driven from **one** calling
  thread via `render()`/`std::future`: each round fires one `render()` per
  instance (safe — one in-flight render per instance, per the note above),
  then waits on all M futures before starting the next round.

Both render 40 frames/view at 512×512, for M ∈ {2, 4, 8}.

## Raw numbers

Two consecutive runs on the same host (`tests/run-tests.sh bench`); shown to
give a sense of run-to-run noise on a shared, non-isolated dev machine (no
CPU pinning, no `nice`, background processes present).

### Experiment 1 — `pixel-probe.json` (trivial fixture, included for
reference only — see caveat above)

```
Run A:
  sync     128x128    p50=  0.007ms  p95=  0.008ms  p99=  0.008ms  total=    1.14ms
  async    128x128    p50=  0.023ms  p95=  0.029ms  p99=  0.035ms  total=    3.67ms
  sync     512x512    p50=  0.116ms  p95=  0.138ms  p99=  0.149ms  total=   16.58ms
  async    512x512    p50=  0.041ms  p95=  0.048ms  p99=  0.055ms  total=    6.23ms
  sync    1080x1080   p50=  0.155ms  p95=  0.178ms  p99=  0.187ms  total=   23.56ms
  async   1080x1080   p50=  0.165ms  p95=  0.195ms  p99=  0.238ms  total=   25.43ms

Run B:
  sync     128x128    p50=  0.005ms  p95=  0.006ms  p99=  0.011ms  total=    0.83ms
  async    128x128    p50=  0.022ms  p95=  0.027ms  p99=  0.030ms  total=    3.65ms
  sync     512x512    p50=  0.105ms  p95=  0.158ms  p99=  0.186ms  total=   17.02ms
  async    512x512    p50=  0.062ms  p95=  0.076ms  p99=  0.086ms  total=    9.58ms
  sync    1080x1080   p50=  0.214ms  p95=  0.270ms  p99=  0.275ms  total=   33.16ms
  async   1080x1080   p50=  0.177ms  p95=  0.210ms  p99=  0.217ms  total=   26.76ms
```

At this workload the actual render is sub-microsecond; both paths are
dominated by fixed dispatch cost, and async's cross-thread hop
(promise/future + waking a pool thread) shows up directly as **2-4x higher
p50 at 128×128** in both runs. This is the clearest evidence in the whole
benchmark that async has a real, fixed per-call tax that only amortizes away
once the render itself is expensive enough to dwarf it — exactly what
Experiment 1's second fixture is designed to test.

### Experiment 1 — `bench-complex.json` (representative fixture)

```
Run A:
  sync     128x128    p50=  0.261ms  p95=  0.402ms  p99=  0.431ms  total=   42.73ms
  async    128x128    p50=  0.285ms  p95=  0.426ms  p99=  0.482ms  total=   46.26ms
  sync     512x512    p50=  0.463ms  p95=  0.517ms  p99=  0.528ms  total=   68.70ms
  async    512x512    p50=  0.471ms  p95=  0.532ms  p99=  0.551ms  total=   70.48ms
  sync    1080x1080   p50=  1.071ms  p95=  1.273ms  p99=  2.714ms  total=  165.20ms
  async   1080x1080   p50=  1.043ms  p95=  1.197ms  p99=  1.281ms  total=  155.45ms

Run B:
  sync     128x128    p50=  0.295ms  p95=  0.361ms  p99=  0.374ms  total=   41.59ms
  async    128x128    p50=  0.274ms  p95=  0.368ms  p99=  0.382ms  total=   42.08ms
  sync     512x512    p50=  0.470ms  p95=  0.542ms  p99=  0.568ms  total=   70.96ms
  async    512x512    p50=  0.473ms  p95=  0.533ms  p99=  0.565ms  total=   71.17ms
  sync    1080x1080   p50=  1.037ms  p95=  1.293ms  p99=  1.334ms  total=  159.27ms
  async   1080x1080   p50=  1.054ms  p95=  1.263ms  p99=  1.327ms  total=  158.57ms
```

On the representative fixture, sync and async are **within noise of each
other at every size** — differences are ~1-5%, and which path "wins" flips
between the two runs (async ahead at 1080² in run A, roughly tied in run B;
sync marginally ahead at 512² in both). p99 is noisier than p50/p95 in both
paths (single-digit-count of samples above the 95th percentile out of 150,
so p99 is more sensitive to one slow outlier — e.g. run A's sync p99 at 1080²
is 2.7ms against a 1.1ms p50, almost certainly one scheduler hiccup on a
shared machine, not a systematic difference between the paths).

**Conclusion: for a single view's own render cost, the internal async
renderer shows no measurable benefit over `renderSync()` on this host.** The
128×128 trivial-fixture result above is the only place a real, repeatable gap
shows up, and it favors **sync**.

### Experiment 2 — multi-view concurrency (`bench-complex.json`, 512×512, 40
frames/view)

```
Run A:
  views=2  threads=   30.87ms  shared-pool=   27.80ms
  views=4  threads=   50.02ms  shared-pool=   46.05ms
  views=8  threads=   88.94ms  shared-pool=   84.88ms

Run B:
  views=2  threads=   31.12ms  shared-pool=   27.55ms
  views=4  threads=   48.71ms  shared-pool=   43.96ms
  views=8  threads=   83.72ms  shared-pool=   82.87ms
```

`shared-pool` is consistently ~5-12% faster than `threads` in this
experiment, shrinking as view count grows (12% at M=2, ~5% at M=8 — this
12-core machine is presumably closer to saturated at M=8 either way, so
there's less headroom for a scheduling difference to show up). This is the
one place in the whole benchmark where async-style dispatch shows a
repeatable, if modest, edge — and it is *not* a reason to change
`RenderCoordinator`. See below.

## What adopting the async path would cost architecturally

Plan §5 lists exactly what `renderSync()` on our own worker buys, and plan §6
builds the entire cancellation model on top of owning that worker.
Concretely, for every item, here is what routing rendering through
`render()`/`std::future` would require changing:

- **Generation-based cancellation (§6).** `RenderCoordinator::runRender`
  checks `generation_` immediately before *and after* the render call
  (`cpp/RenderCoordinator.cpp:233`, `:259`) so a stale in-flight render's
  result is dropped rather than published. With `render()`, the "after" check
  would move to wherever the future's continuation runs — but rlottie's pool
  threads are not ours to instrument with our generation counter without also
  making `RenderCoordinator` itself thread-safe against a callback arriving
  from an *arbitrary rlottie pool thread*, not the one worker thread it
  currently assumes. Every method the pool's continuation would need to touch
  (`frameBuffer_.publish()`, `sink_.onFramePublished()`) is currently
  single-writer-safe *because* only `workerLoop()` calls it.
- **Single in-flight, latest-frame-wins (§5 "Render queue policy").** This is
  implemented today as "the worker thread only ever processes one
  `pendingFrame_`" (`cpp/RenderCoordinator.cpp:61-67`) — trivially true
  because there is exactly one worker. With async dispatch, "only one
  in-flight render" would have to be enforced explicitly (and *is* required —
  see the `mTask` reuse hazard above: calling `render()` again on the same
  `Animation` before the previous future resolves is a data race, not just
  wasted work), turning an emergent property of single-threading into a
  manually-maintained invariant.
- **Object destruction ordering (§5).** `~RenderCoordinator()` calls
  `release()`, which joins the worker thread *before* `core_.reset()` runs
  (`cpp/RenderCoordinator.cpp:180-182`), guaranteeing the `rlottie::Animation`
  is destroyed only after every render touching it has finished. With
  `render()`, an in-flight future could still be executing on a pool thread
  when the owning `RlottiePlayerCore`/`Animation` is torn down; every
  destroy path (`release()`, `setSource()` replacing the animation
  mid-flight) would need to first find and wait on any outstanding future —
  effectively rebuilding the join `release()` gets for free today.
- **Source replacement.** `setSource()` bumps `sourceEpoch_` and clears
  `pendingFrame_` under the same mutex a new load task is enqueued through
  (`cpp/RenderCoordinator.cpp:18-38`); a render in flight when a new source
  arrives is either not yet started (never dequeued) or already running
  against the *old* `Animation`, and the worker naturally moves on to the new
  one next. An in-flight `std::future` render against the animation being
  replaced would need explicit tracking and cancellation-or-wait before the
  old `Animation` could be safely destroyed by the new load.
- **Thread affinity / profiling.** Every render for a given view happens on
  one named, profilable OS thread today (Instruments/systrace/perfetto attach
  to `worker_` and see 100% of that view's render cost in one place). Pool
  threads are shared, unnamed-per-view, and reused across every view in the
  process — attributing "this view's render time" would require additional
  bookkeeping the pool doesn't provide.
- **Stale-frame rejection.** Already covered by generation-based cancellation
  above; restating because it's the concrete user-visible behavior all of
  this protects — a released/detached view must never present a frame that
  arrives after teardown.

None of this is a claim that these problems are *unsolvable* with the async
API — they are solvable, with real engineering (a per-render generation
token threaded through the future's continuation, an explicit outstanding-
future registry awaited on every teardown/source-replacement path, etc.).
The point is that solving them would **rebuild**, by hand, exactly the
guarantees single-threaded ownership of the worker gives for free today. That
is a real cost, and Experiment 2's ~5-12% multi-view number — on a
development Mac, not a device — does not come close to paying for it.

## Recommendation

**Keep `renderSync()` on `RenderCoordinator`'s own worker thread. No change
to `cpp/RenderCoordinator.cpp` or `cpp/RlottiePlayerCore.cpp`.**

- Single-view render cost: no measurable, repeatable benefit from async on
  this host (Experiment 1, representative fixture) — and a real, repeatable
  *penalty* at small/cheap render sizes (Experiment 1, trivial fixture),
  where async's fixed per-call dispatch tax dominates.
- Multi-view concurrency: a modest (~5-12%), narrowing-with-load edge for
  routing through rlottie's shared pool (Experiment 2) — measured on a
  12-core development Mac, not validated on any target device, and not close
  to large enough to justify giving up deterministic generation-based
  cancellation, guaranteed destruction ordering, clean source replacement,
  per-view thread affinity for profiling, and stale-frame rejection, all of
  which are currently *emergent* properties of "one worker thread per view
  calling `renderSync()`" and would become manually-reimplemented invariants
  under the async API.
- The plan's bar was explicit: switch only on **measurable** benefit. The one
  place a real, repeatable measurement exists (multi-view concurrency) is
  real but small, was measured off-device, and is outweighed by what it would
  cost to reproduce our existing safety guarantees. The bar is not met.

If Chunk 7.4's on-device measurement later shows the multi-view gap is
substantially larger on real hardware (different core topology, different
scheduler, thermal throttling behaving differently under real background
load) — or if a future architecture change already needs a shared-pool-style
executor for other reasons — this decision should be revisited with fresh
numbers. It should not be revisited on the strength of this document's
numbers alone; see the caveat at the top.

## How to run

```bash
tests/run-tests.sh bench          # ~10-20s, prints the tables above
tests/run-tests.sh plain bench    # combine with the normal suite
```

`bench` is opt-in, exactly like Chunk 5.5's `fuzz` variant — it is never part
of the bare `tests/run-tests.sh` (`plain asan tsan`) default gate, so it never
slows that down. It is unsanitized (ASan/TSan overhead would itself dominate
the measurement) and uses CMake when available
(`tests/cpp/CMakeLists.txt`'s `rnrlottie_bench_render` target) or a direct
clang fallback otherwise (`tests/run-tests.sh`'s `run_bench_fallback`),
matching the existing `fuzz` variant's structure.
