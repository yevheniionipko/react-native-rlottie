# Detailed implementation plan — `react-native-rlottie`

> Engineering breakdown of `react-native-rlottie-implementation-plan.md`. The
> high-level plan says **what** and **why**; this document says **in what order,
> in what discrete chunks, with what interfaces and acceptance checks**. Read the
> high-level plan and `CLAUDE.md` first — this document does not restate them.
>
> Authored in the **CPPArchitect** role. Where the high-level plan left a
> structural decision open, this document makes it and records the rejected
> alternative. Every non-negotiable invariant from `CLAUDE.md` is preserved.

---

## 0. Architecture decisions locked for implementation

These decisions constrain every chunk below. They are the "design" an
implementer must not re-litigate.

### 0.1 Thread model

Three thread roles, exactly as the high-level plan §5, but with concrete
ownership:

| Thread | Owns | Runs |
|---|---|---|
| **JS thread** | nothing native | prop/command marshaling, event receipt |
| **UI/main thread** | the platform view, the display clock, `PlaybackController`, the generation-bump authority, presentation | tick → compute frame → request render; present published frames; lifecycle |
| **Render worker** (one dedicated serial queue **per view**) | `RlottiePlayerCore`, the `FrameBuffer` render target | load / render / resize / destroy tasks, strictly in order |

**Decision — one serial render worker per view.** Each `RlottieView` owns its
own serial queue (iOS: a dedicated `dispatch_queue_t`, `DISPATCH_QUEUE_SERIAL`;
Android: a single-thread `HandlerThread`/executor). This makes the invariant
"the same `rlottie::Animation` is never rendered and mutated concurrently"
structural and free — a given animation is only ever touched from its own
serial queue.
**Rejected:** a shared global render thread-pool. It would reduce thread count
with many simultaneous animations, but reintroduces per-animation affinity
tracking, cross-view cancellation, and teardown-ordering complexity. Revisit
only if Phase 7 shows thread count is a real cost; the `RenderWorker` interface
below is deliberately poolable later without touching callers.

### 0.2 The core splits into a *front half* and a *back half*

The plan lists both `PlaybackController` and `RenderCoordinator`. Their roles:

- **`PlaybackController`** — **main-thread**, pure logic. Owns the playback
  **state machine**, timing math, and loop/repeat/finish **decisions**. No
  threads, no rlottie, no pixels. Fully headless-testable.
- **`RenderWorker`** (the concrete `RenderCoordinator`) — the **synchronization
  boundary**. Public methods are callable from the main thread and internally
  hop to the serial worker. Owns `RlottiePlayerCore`, the `FrameBuffer`, and the
  authoritative `std::atomic<uint64_t> generation_`.

The platform view is the glue: it owns one `PlaybackController` + one
`RenderWorker`, drives the display clock, and presents. **No C++ core file
references any platform type** — platform callbacks arrive through injected
interfaces (`FrameSink`).

**Rejected:** folding playback state into `RenderWorker` (one class). It would
force the state machine onto the worker thread, make commands async round-trips,
and make the state machine hard to unit-test. Splitting front/back keeps timing
deterministic on the main thread and rendering isolated on the worker.

### 0.3 Frame lifecycle & buffer safety (the crux decision)

Single-in-flight, latest-frame-wins, **request-driven by the display tick** (the
worker never self-schedules):

```
UI tick (main) ──▶ PlaybackController.desiredFrame(now)
                         │
                         ▼
              RenderWorker.requestFrame(frame, gen)
                 │ worker idle → render now
                 │ worker busy → store as pending (overwrite; latest wins)
                 ▼
   [worker] render into FrameBuffer.back → publish() → back becomes front
                 │
                 ▼
        FrameSink.onFramePublished(gen)  (called on worker)
                 │ platform marshals to main
                 ▼
   [main] if gen current: present front buffer; else drop
                 │
                 ▼
        worker picks up pending frame if any
```

**Buffer-safety rule (double buffer, no per-frame allocation):** a buffer handed
to the presenter is not reused as a render target until the presenter has been
asked to present the *next* frame. Because renders are single-in-flight and
requested once per display tick, by the time the worker writes buffer *N+1* the
compositor has already latched buffer *N*. This is the double-buffer contract in
`CLAUDE.md`.
**Rejected:** triple buffering (safe against any timing, but extra memory and
not in the plan) and per-frame CGImage/Bitmap copies (violates "no per-frame
allocation"). Triple buffering is the documented fallback *if* Phase 0/7 shows
tearing on a target device.

### 0.4 C++ ↔ platform handle model

- **iOS:** the Obj-C++ adapter holds the `RenderWorker`/`PlaybackController` by
  value/`unique_ptr` directly — no handle indirection needed.
- **Android:** JNI exposes an opaque `jlong`. **Decision:** the `jlong` is a
  pointer to a heap `JniPlayerHandle` control block carrying a **magic sentinel**
  (`0x524C4F54…`) plus the owned `RenderWorker`. Every JNI entry validates the
  magic before use; `nativeDestroyPlayer` zeroes the magic then frees, and is
  idempotent. Combined with the generation token and "destroy once," this makes
  a stale/double call from JS fail safe rather than UAF.
  **Rejected:** a global `id → shared_ptr` registry (robust against any bad
  handle, but adds a global lock on every JNI call). Kept as a hardening option
  if fuzzing/QA surfaces handle misuse from JS.

### 0.5 Generation semantics

`std::atomic<uint64_t> generation_` lives in `RenderWorker` and is
**bumped on**: source change, surface-size change, reset, view detach, and
release/destroy. Every enqueued task captures the generation at enqueue; a
render result is presented only if `captured == generation_.load()`. The view
resets `PlaybackController` in lock-step whenever it bumps for a source change,
so no stale `onFinish`/`onLoop` escapes.

---

## 1. Conventions (binding for all chunks)

| Topic | Rule |
|---|---|
| **C++ standard** | **C++17** for the shared core and JNI; rlottie itself builds as **C++14** (plan §14) in its own subtree. *Decision:* the plan pins C++14 globally, but the core is compiled by our own targets, not rlottie's, so C++17 (`std::optional`, `std::variant`, structured bindings, guaranteed copy elision) costs nothing and improves the result-type ergonomics. rlottie stays C++14. **Flag:** confirm the iOS deployment target's `libc++` and the NDK both support C++17 (they do for all supported RN ≤ 0.81 toolchains). |
| **Error handling** | **No exceptions cross a JNI or Objective-C++ boundary, ever.** Core APIs return typed results (`LoadResult` / `bool` + out-params / `PlayerErrorCode`). Internal core code may use exceptions only if caught before the boundary; prefer result types throughout. rlottie is built with its own exception settings. |
| **Ownership in signatures** | Owning = `std::unique_ptr`/by-value. Non-owning, non-null = reference. Non-owning, nullable = raw pointer, documented. Caller-provided buffers are raw pointer + size, never owned by the core. |
| **Threading affinity** | Every public method documents its allowed caller thread: `[main]`, `[worker]`, or `[any]`. `RenderWorker` public API is `[any]` (thread-safe façade); `PlaybackController` is `[main]`; `RlottiePlayerCore` is `[worker]` (single-thread-use, not internally synchronized). |
| **Naming** | C++: `PascalCase` types, `camelCase` methods, `trailingUnderscore_` members. TS: standard RN library conventions. Files per plan §18. |
| **No UB / no data races** | All shared mutable state either lives on one thread or is `std::atomic`. Sanitizer-clean (ASan/UBSan/TSan) is an acceptance gate for every C++ chunk. |

---

## 2. Chunk dependency graph

```
Phase 0
  0.1 scaffold ─┬─▶ 0.2 vendor rlottie ─▶ 0.3 minimal native build ─▶ 0.4 pixel spike ──┐
               │                                                                          │
Phase 1 (headless C++, no platform)                                                       │
  1.1 types ─▶ 1.2 PlayerCore ─▶ 1.3 FrameBuffer ─▶ 1.5 RenderWorker ─▶ 1.7 C++ tests     │
        └────▶ 1.4 PlaybackController ───────────────▶ 1.5                                 │
        └────▶ 1.6 cache/limits ───────────────────▶ 1.5                                   │
                                                                                          │
Phase 2 iOS      ◀── needs 1.5 + 0.4 ◀────────────────────────────────────────────────────┤
  2.1 adapter ─▶ 2.2 view/presenter ─▶ 2.3 view manager ─▶ 2.4 module                      │
                                                                                          │
Phase 3 Android  ◀── needs 1.5 + 0.4 ◀────────────────────────────────────────────────────┘
  3.1 JNI ─▶ 3.2 view/presenter ─▶ 3.3 view manager ─▶ 3.4 module/resolver ─▶ 3.5 gradle/cmake

Phase 4 TS       ◀── needs 2.3 + 3.3 (native component names/commands/events exist)
  4.1 types/source ─▶ 4.2 native component+commands ─▶ 4.3 RlottieView.tsx ─▶ 4.4 module/exports

Phase 5 hardening ◀── needs 1.6 + 3.4 + 4.1     (limits, secure paths, cache keys, remote, fuzz)
Phase 6 dynamic props ◀── needs 2.x + 3.x + 4.3 (color/opacity/marker overrides)
Phase 7 perf/stability ◀── needs Phase 2+3 complete
Phase 8 publication ◀── everything
```

**Parallelizable:** After **1.5** is merged, **Phase 2 (iOS)** and **Phase 3
(Android)** proceed fully in parallel — iOS to **RNEngineer** + **CPPEngineer**,
Android to **CPPEngineer** (JNI) + **RNEngineer** (Kotlin). Within Phase 1,
**1.4 (PlaybackController)** is independent of 1.2/1.3 and can be built in
parallel. Phase 4 (TS) can start its **4.1** as soon as the event/prop contract
is frozen (end of 2.3/3.3), not before.

---

# Phase 0 — Compatibility & rendering spike

> Exit criterion (plan §19): the same fixture renders visually and
> pixel-semantically correctly on iOS and Android. **This phase gates all real
> implementation.**

### Chunk 0.1 — Repo scaffold & tooling
| | |
|---|---|
| **Goal** | Create the package skeleton and dev tooling so subsequent chunks have a home. |
| **Depends on** | — |
| **Deliverables** | `package.json` (name, RN peer dep `<=0.81`, scripts placeholders), `tsconfig.json`, `.editorconfig`, `.gitignore`, `.clang-format`, `.clang-tidy`, ESLint/Prettier config, empty `src/ cpp/ ios/ android/ example/ tests/` tree per plan §18, `licenses/` dir, `THIRD_PARTY_NOTICES.md` stub. |
| **Interfaces/contracts** | `package.json` declares `react-native.config.js` autolinking stub; no exports resolve yet. |
| **Threading/ownership** | n/a |
| **Acceptance** | `npm install` succeeds; `tsc --noEmit` passes on an empty `src/index.ts`; formatters run clean. |
| **Agent** | RNEngineer |
| **Risks** | Low. Pin exact RN version used for local dev in the example app. |

### Chunk 0.2 — Vendor & pin rlottie
| | |
|---|---|
| **Goal** | Bring rlottie into the tree at a pinned, patched commit — never fetched at build time. |
| **Depends on** | 0.1 |
| **Deliverables** | `cpp/third_party/rlottie/` (vendored source at a full commit SHA carrying all security fixes through the pinned date), `scripts/update-rlottie.sh` (reproducible update: fetch SHA, copy sources, strip examples/tests/tools, record SHA), `licenses/rlottie-LICENSE.txt`, SHA recorded in `THIRD_PARTY_NOTICES.md` + a `cpp/RlottieVersion.h` constant. |
| **Interfaces/contracts** | `constexpr char kRlottieCommit[] = "<sha>";` exposed for cache-key derivation (Chunk 5.3) and `getNativeVersion()` (2.4/3.4). |
| **Threading/ownership** | n/a |
| **Acceptance** | Fresh checkout builds without network access to rlottie; `update-rlottie.sh` reproduces the exact tree; submodules are **not** the sole delivery mechanism (npm consumers get vendored source). |
| **Agent** | CPPEngineer |
| **Risks** | **High cost if wrong:** picking an unpatched SHA ships known memory-safety CVEs (plan §13/§16). Verify the SHA against the fix list before pinning. |

### Chunk 0.3 — Minimal native build wiring
| | |
|---|---|
| **Goal** | Compile rlottie + a trivial C++ shim on both platforms — prove the toolchain, not playback. |
| **Depends on** | 0.2 |
| **Deliverables** | `android/src/main/cpp/CMakeLists.txt` + `android/build.gradle` (rlottie options per plan §14: `BUILD_SHARED_LIBS OFF`, `LOTTIE_THREAD ON`, `LOTTIE_CACHE ON`, `LOTTIE_MODULE OFF`, `LOTTIE_TEST OFF`; ABIs `arm64-v8a`, `x86_64`, `armeabi-v7a` optional); `react-native-rlottie.podspec` (compiles `cpp/**` + `ios/**`, `CLANG_CXX_LANGUAGE_STANDARD c++17`, rlottie subtree c++14, `libc++`); a `cpp/spike/probe.cpp` that links rlottie and returns its version. |
| **Interfaces/contracts** | One exported symbol per platform that returns the rlottie build string. |
| **Threading/ownership** | n/a |
| **Acceptance** | Android `.so` links for `arm64-v8a` + `x86_64`; iOS pod compiles for device + simulator; upstream examples/tests/tools excluded from both builds. |
| **Agent** | CPPEngineer |
| **Risks** | C++14/17 mixing in one CMake tree — keep rlottie's standard scoped to its own target, don't set it globally. |

### Chunk 0.4 — Pixel-format spike (the gate)
| | |
|---|---|
| **Goal** | Determine rlottie's exact on-buffer pixel semantics on each platform before any presenter is written. |
| **Depends on** | 0.3 |
| **Deliverables** | `tests/fixtures/pixel-probe.json` (opaque R/G/B, 50% transparent red, gradients, transparent edges — plan §8); a spike harness on each platform that renders frame 0 into a raw buffer and dumps it; `docs/pixel-format-report.md` recording channel order, premultiplication, stride/`bytesPerLine`, row orientation, and the **exact conversion** (if any) needed for `CGImage` (`kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst`) and for Android `Bitmap.Config.ARGB_8888` (expect BGRA→RGBA channel handling). |
| **Interfaces/contracts** | The report defines constants later chunks consume: `kCoreSurfaceFormat = ARGB32_Premultiplied`, `kIOSBitmapInfo`, `kAndroidNeedsChannelSwap` (bool). |
| **Threading/ownership** | Spike may run on any thread; single-shot. |
| **Acceptance** | **Same fixture renders pixel-semantically correct on iOS and Android** (plan §19 Phase 0 exit). Report reviewed and committed. No downstream presenter chunk (2.2/3.2) starts until this is signed off. |
| **Agent** | CPPEngineer |
| **Risks** | **Highest-leverage risk in the project.** Assuming the 32-bit buffer passes to native bitmap APIs unchanged is the classic failure (plan §8). Getting stride/premultiply wrong corrupts every later golden test. |

---

# Phase 1 — Shared C++ engine (headless, no platform code)

> Exit criterion (plan §19): C++ tests can load, seek, render, loop and safely
> replace sources without any platform UI code.

### Chunk 1.1 — Core value types & errors
| | |
|---|---|
| **Goal** | Define the vocabulary types shared by every core class and both boundaries. |
| **Depends on** | 0.1 |
| **Deliverables** | `cpp/AnimationSource.h`, `cpp/AnimationMetadata.h`, `cpp/PlaybackState.h`, `cpp/PlayerError.h`. Header-only. |
| **Interfaces/contracts** | See below. |

```cpp
// PlayerError.h  — [any]
enum class PlayerErrorCode {
    None, InvalidSource, SourceNotFound, SourceTooLarge, ParseFailed,
    InvalidDimensions, AllocationFailed, RenderFailed, Released,
};
struct PlayerError {
    PlayerErrorCode code = PlayerErrorCode::None;
    std::string message;               // developer-readable
    std::uint64_t generation = 0;      // source generation this error belongs to
    // optional native diagnostic (dev builds only) added by adapters
    explicit operator bool() const { return code != PlayerErrorCode::None; }
};

// AnimationMetadata.h — value type, [any]
struct Marker { std::string name; std::size_t startFrame, endFrame; };
struct AnimationMetadata {
    std::size_t width = 0, height = 0;      // intrinsic
    double frameRate = 0.0;
    std::size_t totalFrames = 0;
    double duration = 0.0;                   // seconds
    std::vector<Marker> markers;
};

// PlaybackState.h — [main] (owned by PlaybackController)
enum class PlaybackState { Empty, Loading, Ready, Playing, Paused, Stopped, Failed, Released };

// AnimationSource.h — resolved, platform-neutral. No URIs, no HTTP.
struct AnimationSource {
    enum class Kind { File, Data } kind;
    std::string path;          // Kind::File — canonicalized, app-private
    std::string json;          // Kind::Data — raw JSON bytes
    std::string cacheKey;      // deterministic (Chunk 5.3)
    std::string resourcePath;  // external raster assets root (validated, may be empty)
    bool useModelCache = true;
};
```

| **Threading/ownership** | Pure values, copyable, `[any]`. |
| **Acceptance** | Compiles under C++17 with `-Wall -Wextra -Werror`; no rlottie include. |
| **Agent** | CPPEngineer |

### Chunk 1.2 — `RlottiePlayerCore`
| | |
|---|---|
| **Goal** | A single-thread-use façade over rlottie; the only file that includes `rlottie.h`. |
| **Depends on** | 1.1, 0.4 (pixel format), 0.2 (rlottie) |
| **Deliverables** | `cpp/RlottiePlayerCore.h/.cpp` |

```cpp
// RlottiePlayerCore.h — ALL methods [worker]; NOT internally synchronized.
class RlottiePlayerCore final {
public:
    struct LoadResult { bool success; AnimationMetadata metadata; PlayerError error; };

    RlottiePlayerCore();
    ~RlottiePlayerCore();                       // destroys rlottie::Animation on worker
    RlottiePlayerCore(const RlottiePlayerCore&) = delete;
    RlottiePlayerCore& operator=(const RlottiePlayerCore&) = delete;

    LoadResult loadFromFile(const std::string& path, bool useModelCache);
    LoadResult loadFromData(std::string json, const std::string& cacheKey,
                            const std::string& resourcePath, bool useModelCache);

    // Renders `frame` into caller-owned premultiplied-ARGB32 memory.
    // pixels != null; width*height*4 <= buffer; bytesPerLine >= width*4.
    // Returns false on RenderFailed/InvalidDimensions (see lastError()).
    bool renderFrame(std::size_t frame, std::uint32_t* pixels,
                     std::size_t width, std::size_t height,
                     std::size_t bytesPerLine, bool preserveAspectRatio);

    void setColor(const std::string& keyPath, float r, float g, float b);  // Phase 6
    AnimationMetadata metadata() const;             // valid after successful load
    std::size_t frameForProgress(double progress) const;  // clamps [0,1]
    bool isLoaded() const;

private:
    std::unique_ptr<rlottie::Animation> animation_;  // owns; sole reference
    AnimationMetadata metadata_;
};
```

| **Interfaces/contracts** | `renderFrame` never allocates. Dimension/overflow validation happens here *and* in `FrameBuffer`; this method rejects `width==0`, `height==0`, or `bytesPerLine < width*4`. Uses `renderSync()` on the worker (plan §5 rollout). |
| **Threading/ownership** | `[worker]` only. Owns the `rlottie::Animation`. Never shared across threads. |
| **Acceptance** | Unit tests: load valid/invalid/empty/single-frame; metadata matches fixtures; render frame 0 matches the golden buffer hash from 0.4; `frameForProgress` clamps. ASan/UBSan clean. |
| **Agent** | CPPEngineer |
| **Risks** | Passing untrusted JSON to rlottie — size/dimension caps enforced upstream (1.6) *before* this is called. |

### Chunk 1.3 — `FrameBuffer`
| | |
|---|---|
| **Goal** | Own reusable double-buffered pixel memory with safe resize and overflow checks. |
| **Depends on** | 1.1 |
| **Deliverables** | `cpp/FrameBuffer.h/.cpp` |

```cpp
// FrameBuffer.h — construction [any]; render-target & publish [worker]; readFront [main].
class FrameBuffer final {
public:
    struct Dimensions { std::size_t width = 0, height = 0; };   // physical pixels
    struct Limits { std::size_t maxPixels; };                    // clamp (Chunk 1.6)

    explicit FrameBuffer(Limits limits);

    // [worker] Ensure both buffers match dims; reallocates only on change.
    // Rejects zero/overflowing dims or > maxPixels -> false + error.
    bool resize(Dimensions dims, PlayerError& outError);

    // [worker] The buffer to render INTO (the current back buffer). null if unsized.
    std::uint32_t* acquireRenderTarget();
    Dimensions dimensions() const;
    std::size_t bytesPerLine() const;      // width*4

    // [worker] Mark the just-rendered back buffer as the new front (swap indices).
    void publish();

    // [main] The buffer to present. Valid until the NEXT publish() (double-buffer
    // contract §0.3). Returns null if nothing published for current dims.
    const std::uint32_t* readFront() const;

private:
    std::array<std::vector<std::uint32_t>, 2> buffers_;
    std::atomic<int> frontIndex_{0};       // publish() flips; readFront reads
    Dimensions dims_;
    Limits limits_;
};
```

| **Interfaces/contracts** | `resize` checks `width*height` multiplication overflow *before* allocating (plan §7). Allocation only after dims known; buffers reused until dims change. Premultiplied-ARGB32 throughout. The double-buffer safety rule (§0.3) is the caller's contract, enforced by single-in-flight scheduling in `RenderWorker`. |
| **Threading/ownership** | Owns the two `std::vector` buffers. `frontIndex_` is the only cross-thread field (atomic). Back buffer written by `[worker]`, front read by `[main]`. |
| **Acceptance** | Tests: resize allocates once and is idempotent for equal dims; overflow/`>maxPixels` rejected; `readFront` returns stable memory across one render cycle; TSan clean under a producer(worker)/consumer(main) stress harness. |
| **Agent** | CPPEngineer |
| **Risks** | The double-buffer/tearing question (§0.3) — the TSan stress harness must model "worker writes N+1 while main reads N" and prove the contract holds or escalate to triple buffering. |

### Chunk 1.4 — `PlaybackController`
| | |
|---|---|
| **Goal** | The main-thread state machine + timing math; decides frames and lifecycle events. Pure. |
| **Depends on** | 1.1 |
| **Deliverables** | `cpp/PlaybackController.h/.cpp` |

```cpp
// PlaybackController.h — ALL methods [main]. No threads, no rlottie, no pixels.
struct PlaybackConfig {
    bool loop = false;
    int repeatCount = 0;          // 0 = infinite when loop; else N extra plays
    double speed = 1.0;           // may be negative if reverse supported
    std::size_t startFrame = 0, endFrame = 0;  // 0/0 => full range from metadata
    bool autoPlay = false;
};
enum class PlaybackEvent { None, Started, Paused, Loop, Finished };

class PlaybackController final {
public:
    void onLoaded(const AnimationMetadata&);   // Empty/Loading -> Ready
    void onLoadFailed(const PlayerError&);      // -> Failed
    void configure(const PlaybackConfig&);
    void play(std::optional<std::size_t> startFrame, std::optional<std::size_t> endFrame);
    void pause(); void resume(); void stop(); void reset();
    void seekToFrame(std::size_t); void seekToProgress(double);
    void setSpeed(double);

    // Called every display tick with a monotonic timestamp (seconds).
    // Returns the frame to render + any event that fired this tick.
    struct Tick { std::size_t frame; PlaybackEvent event; bool shouldRender; };
    Tick advance(double monotonicNow);

    PlaybackState state() const;
    const AnimationMetadata& metadata() const;

private:
    // animationTime = startOffset + elapsed*speed;  frame = start + floor(animTime*fps)
    // applies segment range, repeatCount, loop rules, direction. (plan §5)
};
```

| **Interfaces/contracts** | Frame math exactly per plan §5. One `PlaybackState` enum + explicit transitions — **never** parallel booleans (plan §4). `advance` is pure given its inputs; deterministic and fully unit-testable with a fake clock. |
| **Threading/ownership** | `[main]` only. Holds no shared state. The view resets it in lock-step with generation bumps (§0.5). |
| **Acceptance** | Table-driven tests with a synthetic clock: loop vs once, finite `repeatCount`, segment ranges, seek clamping, `speed` incl. zero/negative policy, `Finished` fires exactly once, `Loop` fires per wrap. No I/O, no platform. |
| **Agent** | CPPEngineer |
| **Risks** | Off-by-one at segment/loop boundaries — the most common animation bug; covered by boundary tests. |

### Chunk 1.5 — `RenderWorker` (the concrete `RenderCoordinator`)
| | |
|---|---|
| **Goal** | The synchronization boundary: owns the worker thread, generation, single-in-flight scheduling, and ties `RlottiePlayerCore` + `FrameBuffer` together behind a thread-safe façade. |
| **Depends on** | 1.2, 1.3, 1.1 |
| **Deliverables** | `cpp/RenderCoordinator.h/.cpp`, `cpp/FrameSink.h` |

```cpp
// FrameSink.h — implemented by each platform adapter.
struct PlayerEvent { enum Type { Loaded, Error } type; AnimationMetadata metadata; PlayerError error; };
class FrameSink {
public:
    virtual ~FrameSink() = default;
    // [worker] after a frame is published to the front buffer. Must be cheap;
    // platform marshals presentation to its UI thread.
    virtual void onFramePublished(std::uint64_t generation) = 0;
    // [worker] load result / render error, to be marshaled + mapped to a React event.
    virtual void onEvent(const PlayerEvent&) = 0;
};

// RenderCoordinator.h — public API is [any] (thread-safe façade); work runs [worker].
class RenderCoordinator final {
public:
    RenderCoordinator(FrameSink& sink, FrameBuffer::Limits limits);
    ~RenderCoordinator();                          // release() semantics; joins worker

    // Structural changes bump generation. Async: enqueue on worker, return immediately.
    std::uint64_t setSource(AnimationSource source);        // -> Loading; bumps gen
    std::uint64_t setSurfaceSize(std::size_t w, std::size_t h); // bumps gen; resizes
    void requestFrame(std::size_t frame, std::uint64_t generation); // single-in-flight
    void setColor(std::string keyPath, float r, float g, float b);  // Phase 6
    void reset();                                  // bumps gen; clears pending
    void release();                                // bumps gen; drains; destroys core

    const FrameBuffer& frameBuffer() const;        // [main] presenter reads front
    std::uint64_t generation() const;              // atomic load

private:
    void runLoad(AnimationSource, std::uint64_t gen);   // [worker]
    void runRender(std::size_t frame, std::uint64_t gen); // [worker]
    // pending-frame slot (latest wins), busy flag, serial queue, atomic generation_
};
```

| **Interfaces/contracts** | Implements the single-in-flight, latest-frame-wins policy (§0.3): `requestFrame` renders now if idle else overwrites the pending slot; on render completion, presents (via `publish()`+`onFramePublished`) then drains the pending slot. Every task captures `generation` at enqueue and drops stale results (§0.5). `release()` bumps generation, cancels pending work, destroys the core **on the worker**, then joins — no callback fires after `release()` returns. |
| **Threading/ownership** | Owns the worker thread, `RlottiePlayerCore`, `FrameBuffer`, the `generation_` atomic, and the pending slot. `FrameSink&` is a non-owning reference outliving the coordinator (adapter guarantees). |
| **Acceptance** | Tests with a fake `FrameSink`: load→render→publish path; source replacement mid-render presents only the new source (generation drops stale); destruction during a pending render never calls the sink afterward; no render queue growth under a flood of `requestFrame`; TSan + ASan clean. **Phase 1 exit criterion is demonstrated here.** |
| **Agent** | CPPEngineer |
| **Risks** | **Highest in Phase 1.** Teardown ordering (destroy-during-render) is the UAF hotspot (plan §22). The generation + join-on-release design must be verified under TSan with a source-replacement stress test. |

### Chunk 1.6 — `ModelCacheController` + limits/validation config
| | |
|---|---|
| **Goal** | Global rlottie model-cache control and the input-limit policy object consumed by loaders. |
| **Depends on** | 1.1, 0.2 |
| **Deliverables** | `cpp/ModelCacheController.h/.cpp`, `cpp/InputLimits.h` |

```cpp
struct InputLimits {                       // consumed by RlottiePlayerCore + resolvers
    std::size_t maxJsonBytes;              // reject SourceTooLarge
    std::size_t maxPixels;                 // FrameBuffer clamp
    std::size_t maxFrames;                 // reject pathological frame counts
    std::size_t maxExternalAssets, maxExternalBytes;
};
class ModelCacheController {               // [any] — wraps a global rlottie setting
public:
    static void setModelCacheSize(std::size_t entries);
    static void clearModelCache();
};
```

| **Interfaces/contracts** | Limits are validated *before* rlottie sees input. `ModelCacheController` maps to rlottie's library-level cache config; per-view playback never routes through it (plan §15). |
| **Threading/ownership** | `[any]`; cache calls are process-global — document as such. |
| **Acceptance** | Oversized JSON rejected with `SourceTooLarge`; frame/pixel caps enforced; cache size set/clear reflected in rlottie. |
| **Agent** | CPPEngineer |

### Chunk 1.7 — C++ unit-test + sanitizer harness
| | |
|---|---|
| **Goal** | A headless test target and golden-frame harness runnable in CI with sanitizers. |
| **Depends on** | 1.2–1.6 |
| **Deliverables** | `tests/cpp/` (test runner, e.g. GoogleTest/Catch2), `tests/golden/` (frame hashes), `tests/fixtures/`, a CMake test target with ASan/UBSan/TSan variants, CI script. |
| **Interfaces/contracts** | Golden test: render selected frames headless, hash the buffer, compare with tolerance for permitted platform conversion differences (plan §20). |
| **Threading/ownership** | Tests drive `[main]`/`[worker]` via the real coordinator; a fake clock and fake `FrameSink`. |
| **Acceptance** | Full plan §20 "Shared C++ tests" list passes; golden frames stable; **clean ASan/UBSan/TSan** — a §21 acceptance criterion. |
| **Agent** | CPPEngineer |

---

# Phase 2 — iOS native component

> Exit criterion: multiple animations mount, play, resize, unmount repeatedly
> without blocking the main thread or leaking native resources.

### Chunk 2.1 — iOS player adapter (Obj-C++)
| | |
|---|---|
| **Goal** | Own the `RenderCoordinator` + `PlaybackController`, implement `FrameSink`, and bridge to a dedicated serial `dispatch_queue_t`. |
| **Depends on** | 1.5, 0.4 |
| **Deliverables** | `ios/RNRlottiePlayer.h/.mm` |
| **Interfaces/contracts** | Obj-C++ class holding `std::unique_ptr<RenderCoordinator>` + `PlaybackController` (by value). Implements `FrameSink`: `onFramePublished` → `dispatch_async(main)` present; `onEvent` → main-thread `RCTDirectEventBlock`. Exposes `-setSource:`, `-configure:`, `-play/-pause/…`, `-onDisplayTick:`, `-setSurfaceSize::`, `-release`. |
| **Threading/ownership** | Adapter owns coordinator; guarantees `FrameSink` (self) outlives the coordinator by calling `release()` in `dealloc` **before** the C++ objects destruct. |
| **Acceptance** | Unit-testable via a headless harness that drives the adapter without a view; no callback after `release`. |
| **Agent** | RNEngineer (with CPPEngineer on the C++ boundary) |

### Chunk 2.2 — `RNRlottieView` + Core Graphics presenter
| | |
|---|---|
| **Goal** | The `UIView` with `CADisplayLink`, presentation, resize, and lifecycle. |
| **Depends on** | 2.1 |
| **Deliverables** | `ios/RNRlottieView.h/.mm`, `ios/RNRlottieFramePresenter.h/.mm` |
| **Interfaces/contracts** | `CADisplayLink` tick (main) → `PlaybackController.advance` → `coordinator.requestFrame`. Presenter builds a `CGImage` referencing the front buffer (no per-frame copy; reuse `CGColorSpace`, `kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst` per 0.4) and assigns `self.layer.contents` on main, then the buffer swap is safe per §0.3. Resize: `layoutSubviews` → `pixelW = layoutW*screenScale*renderScale` (clamped) → `setSurfaceSize` (debounced; bump generation; keep showing last valid frame). |
| **Threading/ownership** | All presentation `[main]`. Owns the display link; invalidates it in `dealloc`. |
| **Acceptance** | Renders the fixture correctly on device + simulator; resize keeps last frame until new buffer ready; `dealloc` invalidates the link, bumps generation, prevents future callbacks, releases buffers (plan §9). Background/foreground pause-resume works. |
| **Agent** | RNEngineer |
| **Risks** | `CADisplayLink` retain cycles (use a weak proxy target) — a classic iOS leak. |

### Chunk 2.3 — `RNRlottieViewManager`
| | |
|---|---|
| **Goal** | Export the component, props, direct events, and commands over the legacy bridge. |
| **Depends on** | 2.2 |
| **Deliverables** | `ios/RNRlottieViewManager.mm` |
| **Interfaces/contracts** | Subclass `RCTViewManager`; `RCT_EXPORT_VIEW_PROPERTY` for all props (plan §9); custom conversion for the `source` object; commands mapped to the stable numeric IDs (plan §11) via `RCT_EXPORT_METHOD`/view registry lookup; direct events `onAnimationLoaded/Start/Pause/Loop/Finish/Error`. **Freezes the prop/event/command contract that Phase 4 consumes.** |
| **Threading/ownership** | Prop setters + commands arrive on main; forwarded to the adapter. |
| **Acceptance** | Every prop maps; every command reaches the right view; events don't fire after unmount (plan §20 bridge tests). |
| **Agent** | RNEngineer |

### Chunk 2.4 — `RNRlottieModule` (global config)
| | |
|---|---|
| **Goal** | Old-arch native module for global ops only: model cache + native version. |
| **Depends on** | 1.6, 2.1 |
| **Deliverables** | `ios/RNRlottieModule.mm` |
| **Interfaces/contracts** | `configure({modelCacheSize})`, `clearModelCache()`, `getNativeVersion()` → `ModelCacheController` + `kRlottieCommit`. Never routes per-view playback (plan §15). |
| **Acceptance** | Cache size/clear reflected; version returns pinned SHA. |
| **Agent** | RNEngineer |

---

# Phase 3 — Android native component

> Exit criterion: Android behaviour matches iOS for playback, events, source
> replacement, and error handling.

### Chunk 3.1 — JNI layer + handle safety
| | |
|---|---|
| **Goal** | A narrow, handle-validated JNI surface owning the `RenderCoordinator`; the C++/JNI half of Android. |
| **Depends on** | 1.5, 0.4 |
| **Deliverables** | `android/src/main/cpp/RlottieJni.cpp`, `android/src/main/cpp/JniPlayerHandle.cpp/.h`, `android/src/main/cpp/AndroidFrameSink.cpp` |
| **Interfaces/contracts** | JNI functions per plan §10: `nativeCreatePlayer → jlong`, `nativeLoadFromData(handle, json, cacheKey, resourcePath)`, `nativeSetSurfaceSize`, `nativeRequestFrame`, `nativeRenderInto(bitmap)` path, `nativeSetColor`, `nativeDestroyPlayer`. The `jlong` is a `JniPlayerHandle*` with a magic sentinel (§0.4); every entry validates magic; destroy zeroes magic + is idempotent. `AndroidFrameSink` implements `FrameSink`: marshals `onFramePublished`/`onEvent` to the JVM via a cached `JavaVM*` + weak global ref + `Choreographer`/`Handler` post. |
| **Threading/ownership** | Handle owns the `RenderCoordinator`. Presentation model: **two `ARGB_8888` Bitmaps** (front/back) form the double buffer; the worker locks the back Bitmap via `AndroidBitmap_lockPixels`, renders rlottie directly into it (applying `kAndroidNeedsChannelSwap` from 0.4), unlocks, publishes; UI thread draws the front Bitmap. No per-frame allocation or copy. |
| **Acceptance** | Invalid/stale/double handle calls fail safe (no UAF/crash) — verified by a fault-injection test; render into Bitmap matches golden; no callback after destroy. ASan (via NDK) clean on `x86_64`. |
| **Agent** | CPPEngineer |
| **Risks** | **High:** JNI local/global ref lifetime and `JavaVM` attach/detach on the worker thread; magic-handle validation is the UAF guard (plan §10/§22). |

### Chunk 3.2 — `RlottieView.kt` + Choreographer presenter
| | |
|---|---|
| **Goal** | Custom `View` with `Choreographer` scheduling and double-Bitmap presentation. |
| **Depends on** | 3.1 |
| **Deliverables** | `android/src/main/java/.../RlottieView.kt` |
| **Interfaces/contracts** | Holds the native `long` handle; `Choreographer.FrameCallback` tick → `PlaybackController` (native) → `nativeRequestFrame`; `onDraw` draws the current front Bitmap; `onSizeChanged` → `pixelW = dp*density*renderScale` (clamped, debounced) → `nativeSetSurfaceSize` (bump gen; keep last frame). Attach/detach + activity lifecycle pause/resume. |
| **Threading/ownership** | Owns the handle's lifetime with the View; presentation on UI thread. |
| **Acceptance** | Renders fixture; resize keeps last frame; matches iOS visually. |
| **Agent** | RNEngineer |

### Chunk 3.3 — `RlottieViewManager.kt` + `RlottiePackage.kt`
| | |
|---|---|
| **Goal** | `SimpleViewManager` exposing props/commands/events; the second half of the frozen bridge contract. |
| **Depends on** | 3.2 |
| **Deliverables** | `android/src/main/java/.../RlottieViewManager.kt`, `RlottiePackage.kt`, `RlottieEvents.kt` |
| **Interfaces/contracts** | `@ReactProp` for all props; `getCommandsMap` with the **same numeric IDs as iOS** (plan §11) — implement both integer and string command paths for RN-version compatibility (plan §10); direct events via `getExportedCustomDirectEventTypeConstants`; `onDropViewInstance` teardown (stop Choreographer, bump generation, stop events, release handle after in-flight work is safe). Registered via `ReactPackage.createViewManagers`. |
| **Acceptance** | Command/prop/event parity with iOS (plan §20 bridge tests); `onDropViewInstance` leaves no callbacks or leaked handles. |
| **Agent** | RNEngineer |

### Chunk 3.4 — `RlottieModule.kt` + `RlottieSourceResolver.kt`
| | |
|---|---|
| **Goal** | Global config module + platform source resolution (bundled/file/content URI/raw JSON). |
| **Depends on** | 3.1, 1.6 |
| **Deliverables** | `android/src/main/java/.../RlottieModule.kt`, `RlottieSourceResolver.kt` |
| **Interfaces/contracts** | Module: `configure/clearModelCache/getNativeVersion`. Resolver: turns a JS source into an app-controlled path or JSON string **before** the C++ core sees it; C++ performs no HTTP (plan §12). Content URIs copied into app-private storage. |
| **Acceptance** | All v1 source kinds resolve; C++ never receives a raw URI. |
| **Agent** | RNEngineer |

### Chunk 3.5 — Gradle + CMake full integration
| | |
|---|---|
| **Goal** | Build the `react-native-rlottie` shared lib linking rlottie + core + JNI. |
| **Depends on** | 3.1, 0.3 |
| **Deliverables** | Final `android/src/main/cpp/CMakeLists.txt` (links `rlottie::rlottie android log jnigraphics`), `android/build.gradle` externalNativeBuild + ABI filters. |
| **Acceptance** | `.so` builds for `arm64-v8a` + `x86_64` (+ optional `armeabi-v7a`); example app runs on device + emulator. |
| **Agent** | CPPEngineer + RNEngineer |

---

# Phase 4 — TypeScript API

> Exit criterion: consumers interact only with the documented TypeScript API and
> never access platform-specific methods.

| Chunk | Goal | Depends | Deliverables & contract | Agent |
|---|---|---|---|---|
| **4.1** | Source normalization + types | 2.3, 3.3 | `src/types.ts`, `src/source.ts`. `RlottieSource` discriminated union (plan §3); **stringify object sources exactly once** here; compute deterministic cache key (Chunk 5.3 formula) in TS. | RNEngineer |
| **4.2** | Native component + commands | 4.1 | `src/RlottieNativeComponent.ts` (`requireNativeComponent`), `src/commands.ts` (the stable `RlottieCommand` enum from plan §11; a `dispatch` wrapper hiding `findNodeHandle` + `UIManager.dispatchViewManagerCommand`; guards commands before mount). | RNEngineer |
| **4.3** | `RlottieView.tsx` | 4.2 | `forwardRef` + `useImperativeHandle` exposing `RlottieViewRef` (`play/pause/resume/stop/reset/seekToProgress/seekToFrame/setSpeed`, plan §3); prop validation; typed event handlers; `NativeSyntheticEvent<RlottieLoadedEvent/ErrorEvent>`. | RNEngineer |
| **4.4** | Module + exports + autolinking | 4.3 | `src/RlottieModule.ts` (`Rlottie.configure/clearModelCache/getNativeVersion`), `src/index.ts`, `react-native.config.js`, `package.json` exports. | RNEngineer |

**Acceptance (phase):** a consumer app uses only `RlottieView` + `Rlottie` +
types; TS types match native event payloads exactly; command before mount and
invalid args don't crash (plan §20 bridge tests).

---

# Phase 5 — Caching & source hardening

> Exit criterion: invalid or hostile sources fail predictably without
> uncontrolled memory use or filesystem access.

| Chunk | Goal | Depends | Contract | Agent |
|---|---|---|---|---|
| **5.1** | Input limits enforced end-to-end | 1.6, 3.4, 4.1 | Byte/frame/pixel/dimension caps (plan §16) checked in resolvers *and* core; overflow-checked allocation; failures → typed `PlayerError`. | CPPEngineer + RNEngineer |
| **5.2** | Secure resource paths | 3.4 | Per-animation app-private dir; reject `..`; canonicalize; no filesystem escape; allow-list image extensions; file-count/size limits (plan §12). | RNEngineer |
| **5.3** | Deterministic cache keys | 4.1, 0.2 | Internal key = `sha256(jsonBytes) + ":" + callerCacheKey + ":" + rlottieCommit + ":" + parseConfig` (plan §15). Never let two payloads share a key silently. | RNEngineer (TS) + CPPEngineer (native check) |
| **5.4** | Remote loading policy | 3.4, 4.1 | Opt-in `allowRemoteSources`; HTTPS-only default; resolver downloads to app-private cache; **C++ never does HTTP** (plan §12/§16). v1.1 scope but stub the gate now. | RNEngineer |
| **5.5** | Malformed/fuzz corpus + harness | 1.7 | `tests/malformed/` (recursive/oversized/invalid structures); a fuzz target over `loadFromData`/`renderFrame` (plan §16/§20). | CPPEngineer |

---

# Phase 6 — Dynamic properties

| Chunk | Goal | Depends | Contract | Agent |
|---|---|---|---|---|
| **6.1** | Color/opacity/stroke overrides | 1.2, 2.3, 3.3, 4.3 | `colorOverrides` prop → `RenderCoordinator.setColor` (keyPath) → `RlottiePlayerCore.setColor`, executed **on the same serialized worker queue as rendering** (plan §6). Extend to opacity/stroke width. | CPPEngineer (core) + RNEngineer (prop) |
| **6.2** | Marker playback | 1.4, 4.3 | Metadata markers surfaced in `onAnimationLoaded`; play-by-marker resolves to a frame segment in `PlaybackController`. | CPPEngineer + RNEngineer |

---

# Phase 7 — Performance & stability

| Chunk | Goal | Depends | Contract | Agent |
|---|---|---|---|---|
| **7.1** | Instrumentation & metrics | Phase 2+3 | Measure parse time, first-frame, render p50/p95/p99, dropped frames, buffer alloc count, peak native memory, UI-thread stalls (plan §7 metrics). | CPPEngineer + RNEngineer |
| **7.2** | `renderSync` vs async benchmark | 7.1 | Benchmark worker-`renderSync()` vs rlottie `render()`/`std::future`; switch only on measurable benefit (plan §5). | CPPEngineer |
| **7.3** | Lifecycle stress + leak tests | Phase 2+3 | Mount/unmount hundreds of times, rapid source change, resize-while-rendering, bg/fg, JS reload, multiple views sharing a source, low-memory (plan §20). Native leak-clean. | CPPEngineer + RNEngineer |
| **7.4** | Cross-platform golden verification | 1.7, 2.2, 3.2 | Golden frames verified on-device on both platforms with permitted-conversion tolerance. | CPPEngineer |

**Acceptance:** performance budgets documented against named reference devices +
representative fixtures; the full §21 non-functional criteria pass.

---

# Phase 8 — Publication

| Chunk | Goal | Deliverables | Agent |
|---|---|---|---|
| **8.1** | Ship v1 | Install guide, supported-RN matrix, API reference, troubleshooting, `THIRD_PARTY_NOTICES.md`, example app, release notes, ABI verification, npm content verification (plan §19 Phase 8). | RNEngineer |

---

## Appendix A — Decisions the user should review

These are choices I made where the high-level plan was open. Flag any you'd
overturn before implementation starts.

1. **C++17 for the core** (rlottie stays C++14). Plan §14 says C++14 globally;
   I scoped C++17 to our own targets for `std::optional`/`variant`/structured
   bindings. *Reject if you need to support a toolchain without C++17 `libc++`.*
2. **One serial render worker per view** (not a shared pool). Simplest correct
   concurrency; revisit in Phase 7 if thread count bites with many animations.
3. **Front/back split of the core** into main-thread `PlaybackController` +
   worker-boundary `RenderCoordinator`. Keeps timing deterministic and rendering
   isolated; makes both headless-testable.
4. **Double-buffer safety via tick-driven single-in-flight** (present-before-next-request),
   with triple buffering as the documented fallback if a device tears.
5. **Android presentation = two `ARGB_8888` Bitmaps, direct render via
   `AndroidBitmap_lockPixels`** (no per-frame copy), pending the 0.4 channel-swap
   finding. Fallback is the native-buffer-then-`Canvas.drawBitmap` path (plan §10).
6. **Android handle = magic-validated pointer**, not a global id registry.
   Registry kept as a hardening option if fuzzing surfaces JS handle misuse.

## Appendix B — Risk register (ranked by cost of getting it wrong)

| Rank | Risk | Owning chunk(s) | Mitigation |
|---|---|---|---|
| 1 | Pixel-format/stride/premultiply wrong | 0.4 | Mandatory spike gate before any presenter |
| 2 | UAF on destroy-during-render / unmount | 1.5, 3.1 | Generation token + join-on-release + magic handle; TSan stress |
| 3 | Unpatched rlottie CVEs | 0.2, 5.5 | Pinned patched SHA + fuzzing + input caps |
| 4 | Render queue growth / UI jank | 1.5, 2.2, 3.2 | Single in-flight + latest-wins + native display clocks |
| 5 | iOS/Android behavioural divergence | 2.3, 3.3, 1.4 | Shared `PlaybackController` + cross-platform contract tests |
| 6 | Buffer tearing under load | 1.3 | Double-buffer contract; triple-buffer fallback |
| 7 | Large JSON crossing the bridge | 4.1, 3.4 | Stringify once; prefer file sources; native resolution |
| 8 | External assets escaping resource dir | 5.2 | Canonicalized app-private paths + allow-listing |
```
