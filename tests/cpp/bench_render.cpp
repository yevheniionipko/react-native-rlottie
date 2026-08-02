// Chunk 7.2 — opt-in benchmark: worker-renderSync() vs rlottie's asynchronous
// render()/std::future path (plan §5, "renderSync versus rlottie asynchronous
// rendering").
//
// NOT part of the default test gate (tests/run-tests.sh with no args). Wired
// exactly like the Chunk 5.5 fuzz target: a separate opt-in binary with its
// own main(), invoked via `tests/run-tests.sh bench`. See
// docs/render-benchmark.md for methodology, results, and the recommendation
// this benchmark exists to support.
//
// Two experiments:
//
//  1. Single-animation latency: for a fixed Animation instance, compare
//     per-frame wall time of renderSync() against render()+future::get()
//     called back-to-back (the only safe usage — see the note by
//     kAsyncSingleInFlightNote below), across several surface sizes.
//     Percentiles reuse MetricsCollector (cpp/Metrics.h, Chunk 7.1) rather
//     than a second percentile implementation, via its public
//     recordFramePublished()/snapshot() API.
//
//  2. Multi-view concurrency: our architecture already gives every view its
//     own OS worker thread (RenderCoordinator ctor). This experiment asks
//     whether rlottie's *internal* shared thread pool (LOTTIE_THREAD=ON)
//     provides additional, measurable benefit ON TOP of that — i.e. is
//     routing multiple concurrent views through render()/std::future instead
//     of "N threads each calling renderSync()" a win, a wash, or a
//     regression (pool contention, thread hop overhead).
#include <rlottie.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Metrics.h"

#ifndef RNRLOTTIE_TEST_DATA_DIR
#define RNRLOTTIE_TEST_DATA_DIR "tests"
#endif

namespace {

using Clock = std::chrono::steady_clock;

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "bench_render: cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string dataPath(const char* rel) {
    return std::string(RNRLOTTIE_TEST_DATA_DIR) + "/" + rel;
}

double msSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Every AnimationImpl reuses a single RenderTask (mTask) across renderAsync()
// calls (cpp/third_party/rlottie/src/lottie/lottieanimation.cpp,
// AnimationImpl::renderAsync): a second render() on the SAME Animation before
// the first future has been waited on mutates that shared task's fields
// (frameNo/surface) while a pool thread may still be reading them — a data
// race, not just a missed pipelining opportunity. So "submit ahead, fetch
// later" is only safe across DIFFERENT Animation instances, never repeatedly
// against one. Every use of render() below therefore calls .get() before
// issuing the next render() on that same instance. See "single in-flight
// render() per Animation" in docs/render-benchmark.md.

std::unique_ptr<rlottie::Animation> loadFixture(const std::string& json,
                                                 const std::string& cacheKey) {
    // useModelCache=false: a real view loads its source once, not repeatedly
    // from a warm rlottie model cache — caching here would understate the
    // parse-adjacent cost and, worse, let Path A's load warm a cache that
    // Path B's load then rides for free (or vice versa), which would bias
    // whichever path is measured second.
    auto anim = rlottie::Animation::loadFromData(json, cacheKey, "", /*useModelCache=*/false);
    if (!anim) {
        std::fprintf(stderr, "bench_render: fixture failed to parse\n");
        std::exit(2);
    }
    return anim;
}

struct PathStats {
    rnrlottie::MetricsSnapshot snap;
    double totalMs = 0.0;
};

// Warms an animation's internal caches (tessellation etc.) with a few
// throwaway renders so neither path pays a cold-cache tax the other doesn't.
void warmUp(rlottie::Animation& anim, std::size_t totalFrames, std::size_t w, std::size_t h) {
    std::vector<std::uint32_t> px(w * h, 0);
    rlottie::Surface s(px.data(), w, h, w * 4);
    for (std::size_t i = 0; i < std::min<std::size_t>(5, totalFrames); ++i) {
        anim.renderSync(i, s, true);
    }
}

PathStats runSyncPath(rlottie::Animation& anim, std::size_t totalFrames, std::size_t w,
                      std::size_t h, int iterations) {
    rnrlottie::MetricsCollector collector;
    collector.setEnabled(true);
    std::vector<std::uint32_t> px(w * h, 0);
    rlottie::Surface surface(px.data(), w, h, w * 4);
    const auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        const std::size_t frame = totalFrames ? static_cast<std::size_t>(i) % totalFrames : 0;
        const auto start = Clock::now();
        anim.renderSync(frame, surface, true);
        collector.recordFramePublished(msSince(start));
    }
    PathStats stats;
    stats.totalMs = msSince(t0);
    stats.snap = collector.snapshot();
    return stats;
}

PathStats runAsyncPath(rlottie::Animation& anim, std::size_t totalFrames, std::size_t w,
                       std::size_t h, int iterations) {
    rnrlottie::MetricsCollector collector;
    collector.setEnabled(true);
    std::vector<std::uint32_t> px(w * h, 0);
    const auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        const std::size_t frame = totalFrames ? static_cast<std::size_t>(i) % totalFrames : 0;
        rlottie::Surface surface(px.data(), w, h, w * 4);
        const auto start = Clock::now();
        // Fire-and-immediately-wait: the only safe repeated usage against a
        // single Animation instance (see kAsyncSingleInFlightNote above).
        std::future<rlottie::Surface> fut = anim.render(frame, surface, true);
        fut.get();
        collector.recordFramePublished(msSince(start));
    }
    PathStats stats;
    stats.totalMs = msSince(t0);
    stats.snap = collector.snapshot();
    return stats;
}

void printRow(const char* label, std::size_t size, const PathStats& s) {
    std::printf("  %-6s %5zux%-5zu  p50=%7.3fms  p95=%7.3fms  p99=%7.3fms  total=%8.2fms\n",
               label, size, size, s.snap.renderP50Ms, s.snap.renderP95Ms, s.snap.renderP99Ms,
               s.totalMs);
}

// Experiment 2: M simulated concurrent views, each rendering `framesPerView`
// frames at `size`x`size`.
//
//   "threads"       — our current architecture: M OS threads, each owning its
//                      own Animation and calling renderSync() serially (i.e.
//                      M independent RenderCoordinator-style workers).
//   "shared-pool"   — M Animation instances driven from ONE calling thread via
//                      render()/std::future, relying entirely on rlottie's
//                      internal RenderTaskScheduler pool (LOTTIE_THREAD=ON) to
//                      fan the work out across cores.
double runConcurrentThreads(const std::string& json, int viewCount, int framesPerView,
                            std::size_t size) {
    std::vector<std::thread> workers;
    workers.reserve(viewCount);
    const auto t0 = Clock::now();
    for (int v = 0; v < viewCount; ++v) {
        workers.emplace_back([&json, v, framesPerView, size] {
            auto anim = loadFixture(json, "bench-threads-" + std::to_string(v));
            std::size_t w, h;
            anim->size(w, h);
            warmUp(*anim, anim->totalFrame(), size, size);
            std::vector<std::uint32_t> px(size * size, 0);
            rlottie::Surface surface(px.data(), size, size, size * 4);
            const std::size_t total = anim->totalFrame();
            for (int i = 0; i < framesPerView; ++i) {
                const std::size_t frame = total ? static_cast<std::size_t>(i) % total : 0;
                anim->renderSync(frame, surface, true);
            }
        });
    }
    for (auto& t : workers) t.join();
    return msSince(t0);
}

double runConcurrentSharedPool(const std::string& json, int viewCount, int framesPerView,
                               std::size_t size) {
    std::vector<std::unique_ptr<rlottie::Animation>> anims;
    std::vector<std::vector<std::uint32_t>> buffers(viewCount,
                                                     std::vector<std::uint32_t>(size * size, 0));
    for (int v = 0; v < viewCount; ++v) {
        auto anim = loadFixture(json, "bench-pool-" + std::to_string(v));
        warmUp(*anim, anim->totalFrame(), size, size);
        anims.push_back(std::move(anim));
    }
    const auto t0 = Clock::now();
    for (int i = 0; i < framesPerView; ++i) {
        std::vector<std::future<rlottie::Surface>> futures;
        futures.reserve(viewCount);
        for (int v = 0; v < viewCount; ++v) {
            const std::size_t total = anims[v]->totalFrame();
            const std::size_t frame = total ? static_cast<std::size_t>(i) % total : 0;
            rlottie::Surface surface(buffers[v].data(), size, size, size * 4);
            // Safe: exactly one in-flight render() per Animation instance —
            // we .get() every future from this round before starting round
            // i+1 (see kAsyncSingleInFlightNote).
            futures.push_back(anims[v]->render(frame, surface, true));
        }
        for (auto& f : futures) f.get();
    }
    return msSince(t0);
}

}  // namespace

int main() {
    const std::string trivialJson = readText(dataPath("fixtures/pixel-probe.json"));
    const std::string complexJson = readText(dataPath("fixtures/bench-complex.json"));

    std::printf("=== Chunk 7.2 render benchmark ===\n");
    std::printf("(development host — see docs/render-benchmark.md for the device caveat)\n\n");

    std::printf("--- Experiment 1: single-animation per-frame latency ---\n");
    std::printf("fixture: tests/fixtures/pixel-probe.json (trivial — 4 static solid layers,\n");
    std::printf("kept for continuity with other tests, NOT representative on its own)\n");
    {
        const int iterations = 150;
        for (std::size_t size : {std::size_t{128}, std::size_t{512}, std::size_t{1080}}) {
            auto syncAnim = loadFixture(trivialJson, "bench-trivial-sync-" + std::to_string(size));
            std::size_t w, h;
            syncAnim->size(w, h);
            warmUp(*syncAnim, syncAnim->totalFrame(), size, size);
            PathStats sync = runSyncPath(*syncAnim, syncAnim->totalFrame(), size, size, iterations);

            auto asyncAnim =
                loadFixture(trivialJson, "bench-trivial-async-" + std::to_string(size));
            warmUp(*asyncAnim, asyncAnim->totalFrame(), size, size);
            PathStats async =
                runAsyncPath(*asyncAnim, asyncAnim->totalFrame(), size, size, iterations);

            printRow("sync", size, sync);
            printRow("async", size, async);
        }
    }

    std::printf(
        "\nfixture: tests/fixtures/bench-complex.json (constructed for this benchmark — 60\n");
    std::printf(
        "animated shape layers: keyframed position/rotation, fill+stroke, 1080x1080,\n");
    std::printf("91 frames; pixel-probe.json's 4 static solids are too trivial to be\n");
    std::printf("representative of real Lottie content — see docs/render-benchmark.md)\n");
    {
        const int iterations = 150;
        for (std::size_t size : {std::size_t{128}, std::size_t{512}, std::size_t{1080}}) {
            auto syncAnim = loadFixture(complexJson, "bench-complex-sync-" + std::to_string(size));
            warmUp(*syncAnim, syncAnim->totalFrame(), size, size);
            PathStats sync = runSyncPath(*syncAnim, syncAnim->totalFrame(), size, size, iterations);

            auto asyncAnim =
                loadFixture(complexJson, "bench-complex-async-" + std::to_string(size));
            warmUp(*asyncAnim, asyncAnim->totalFrame(), size, size);
            PathStats async =
                runAsyncPath(*asyncAnim, asyncAnim->totalFrame(), size, size, iterations);

            printRow("sync", size, sync);
            printRow("async", size, async);
        }
    }

    std::printf("\n--- Experiment 2: multi-view concurrency (bench-complex.json, 512x512) ---\n");
    std::printf(
        "\"threads\"      = our architecture: M OS threads, each renderSync() on its own\n");
    std::printf("                  Animation (one worker thread per view, as today).\n");
    std::printf(
        "\"shared-pool\"  = M Animations driven from ONE thread via render()/std::future,\n");
    std::printf("                  relying solely on rlottie's internal pool (LOTTIE_THREAD=ON)\n");
    std::printf("                  to fan work across cores.\n");
    for (int viewCount : {2, 4, 8}) {
        const int framesPerView = 40;
        const double threadsMs = runConcurrentThreads(complexJson, viewCount, framesPerView, 512);
        const double poolMs = runConcurrentSharedPool(complexJson, viewCount, framesPerView, 512);
        std::printf("  views=%d  threads=%8.2fms  shared-pool=%8.2fms\n", viewCount, threadsMs,
                   poolMs);
    }

    std::printf("\n=== done ===\n");
    return 0;
}
