# Chunk 7.3 — lifecycle stress + leak testing

Covers plan §20's "Native lifecycle tests" list and the corresponding §21
non-functional criteria ("no native callback after view destruction", "clean
native leak testing") for the part of that list a HOST binary can actually
exercise, and says explicitly what cannot be — see "What is NOT covered
headless" below. Do not read anything in this document as a device-level
sign-off; it is a native/host-level one.

## What is covered headless

All of it lives in `tests/cpp/lifecycle_tests.cpp`, built and run as part of
the normal `rnrlottie_tests` binary (`tests/run-tests.sh`, no separate opt-in
variant needed — every test here is fast). It drives the same two seams the
platform adapters drive:

- `RenderCoordinator` directly — the shared core's per-view unit (what a
  mounted `RlottieView` owns one of), and
- `JniPlayerHandle`'s registry (`createPlayerHandle`/`destroyPlayerHandle`,
  `livePlayerCount()`) — the closest headless analogue to a real
  ViewManager mount/unmount cycle, already exercised for fault-injection in
  `tests/cpp/android_tests.cpp`.

| plan §20 item | Test(s) | What it actually asserts |
|---|---|---|
| Mount/unmount hundreds of times | `lifecycle_mount_unmount_hundreds` | 300 create→size→load→render→destroy cycles through the JNI handle path; `livePlayerCount()` returns to its pre-loop baseline **after every single iteration**, not just at the end — a leaked registry entry fails on iteration 2. |
| Navigate away while loading | `lifecycle_navigate_away_while_loading` | 150 iterations: `setSource()` then `destroyPlayerHandle()` immediately, without waiting for the load to complete. `release()`'s cancel-then-join contract means no Loaded/Error event or frame can land after destroy returns; registry returns to baseline. |
| Change source rapidly | `lifecycle_rapid_source_change` | 200 back-to-back `setSource()` calls (some with outstanding renders) converge to a **consistent final state**: the coordinator eventually loads the LAST source, renders a frame tagged with the LAST generation, and the buffer dimensions match — not just "didn't crash". |
| Resize while rendering | `lifecycle_resize_while_rendering` | 40 rounds of `setSurfaceSize()` interleaved with `requestFrame()`, settling on a final size; asserts the front buffer's dimensions and stride are exactly the final size once rendering resumes — never a stale or torn one. |
| Multiple views using the same source | `lifecycle_multiple_views_shared_source` | 8 independent `RenderCoordinator`s load the same cache key with `useModelCache=true` (exercising rlottie's shared model cache, `ModelCacheController`), each still renders its OWN correct golden pixel — the shared parsed model does not leak state between independently-owned `Animation` instances. |
| Destruction during a pending render | `lifecycle_destruction_during_pending_render_jni`, `lifecycle_destruction_during_pending_render_repeat` | 150 iterations each: flood `requestFrame()` (guaranteeing an outstanding render) then destroy immediately. JNI variant re-checks `livePlayerCount()` baseline every iteration; the `RenderCoordinator` variant re-checks `coord_no_callback_after_release`'s "no publish after release" contract under repetition. |
| Low-memory notifications | `lifecycle_input_limits_reject_oversize_source`, `lifecycle_framebuffer_allocation_failure_does_not_terminate`, `lifecycle_coordinator_survives_allocation_failure` | See "Allocation-failure paths" below — this is the code path a real low-memory notification would ultimately have to survive, exercised directly rather than via a fabricated OS signal. |

### Allocation-failure paths (the headless approximation of "low-memory")

Three tests, at three layers:

1. **`lifecycle_input_limits_reject_oversize_source`** — `InputLimits`
   rejects an over-budget JSON source (`SourceTooLarge`) before any large
   allocation is attempted, and the `RlottiePlayerCore` is left in a usable
   state: restoring generous limits and loading again succeeds. A rejection
   must not permanently wedge the object.
2. **`lifecycle_framebuffer_allocation_failure_does_not_terminate`** — a
   direct `FrameBuffer::resize()` call sized to genuinely exceed what the
   allocator can satisfy. This is deliberately sized to fail **fast and
   without touching memory** rather than by exhausting real RAM/swap: the
   requested pixel count (3,000,000,000² = 9×10¹⁸) exceeds
   `std::vector<uint32_t>::max_size()` (implementation-defined, but always
   far below `SIZE_MAX/sizeof(T)` — e.g. libc++'s
   `min(PTRDIFF_MAX, allocator_traits::max_size())/sizeof(T)`), so the
   container throws `std::length_error` immediately, before ever asking the
   allocator for memory. Both per-axis values stay individually valid
   (`< UINT32_MAX`) and their product does not overflow `size_t`, so this
   exercises the real `resize()`→`assign()` path rather than being rejected
   earlier by `checkDimensions()`'s own overflow/range guards.

   **A real bug this surfaced and fixed as part of this chunk:**
   `cpp/FrameBuffer.cpp`'s `resize()` originally caught only
   `std::bad_alloc`. An oversized request throws `std::length_error`
   instead (a different exception type — vector rejects it before ever
   calling the allocator), which was **not** caught and would have
   propagated out of `resize()` uncaught, terminating the process — the
   exact opposite of the "must not terminate the process" requirement this
   chunk is supposed to verify. Fixed by also catching
   `std::length_error` and mapping it to the same
   `PlayerErrorCode::AllocationFailed`, since both exceptions mean the same
   externally-observable thing ("this allocation cannot be satisfied").
   First-run evidence for this: the initial version of this test used a
   smaller-but-still-huge size (`200000²` pixels, within `max_size()` but
   requesting ~144 GB across both buffers); on this host that allocation
   actually *succeeded* under macOS's overcommit and then the value-init
   memset started consuming real RSS/swap, driving the test process past
   20 GB resident and multiple minutes before it was killed. That version
   was replaced with the current one specifically so the test is fast and
   safe to run on a real machine (see the file's own comment for exact
   numbers) — mentioned here so a future editor doesn't reintroduce it.
3. **`lifecycle_coordinator_survives_allocation_failure`** — the same
   failure mode through the FULL `RenderCoordinator`/`JniPlayerHandle`
   stack: `setSurfaceSize()` to the pathological size reports a
   `PlayerEvent::Type::Error` (not a crash), and the SAME still-live handle
   remains usable afterwards — a subsequent sane resize, load, and render
   all succeed normally. `livePlayerCount()` returns to baseline on
   destroy.

## What is NOT covered headless — genuine gaps, not faked

These plan §20 items require a device/emulator, a real platform UI thread,
or a real OS signal that this repository's host test binary cannot produce.
They are NOT approximated by a headless substitute and claimed as "covered"
here — they are gaps, to be closed by device-level testing in a later phase
(the plan's Chunk 7.4 / device acceptance criteria territory):

- **Background during playback / resume after foreground.** Requires a real
  `CADisplayLink` pause or `Choreographer` detach driven by an actual
  `UIApplication`/`Activity` lifecycle callback. There is no display link
  and no OS lifecycle in this host binary to pause/resume.
- **JS reload.** There is no JS runtime, bridge, or `RCTBridge` reload event
  in this binary at all — nothing to reload.
- **React root destruction.** Same reason: no React root exists here to
  destroy. The closest headless analogue (`JniPlayerHandle` teardown while
  work is in flight) IS covered above, but that is deliberately described as
  "the JNI registry", not "React root destruction" — the latter also
  exercises the ViewManager/RCTView teardown path, which this stream does
  not own (`ios/`, `android/` ViewManager glue — see `CLAUDE.md` file
  ownership) and is not exercised here.
- **Mount/unmount through the actual `RCTViewManager` /
  `SimpleViewManager`.** This file drives `JniPlayerHandle` and
  `RenderCoordinator` directly — the native core underneath the
  ViewManagers — not the ViewManagers themselves. A real mount/unmount also
  exercises Kotlin/Obj-C view recycling, `UIManager` command dispatch, and
  RN's own view-tree lifecycle, none of which exist in a host C++ binary.
- **Real OS low-memory notifications** (`didReceiveMemoryWarning`,
  `ComponentCallbacks2.onTrimMemory`). What IS covered is the
  allocation-failure CODE PATH those notifications would have to survive if
  the app additionally tried to allocate under pressure (see above) — that
  is a real and useful thing to test, but it is emphatically not the same
  claim as "survives a real low-memory notification," which requires an
  actual device under memory pressure or a simulator-triggered
  notification.

## The leak gate: `tests/run-tests.sh leaks`

### Why this is a separate variant from `asan`

**ASan's LeakSanitizer does not work on macOS.** Running the existing `asan`
variant with `ASAN_OPTIONS=detect_leaks=1` prints `detect_leaks is not
supported on this platform` and exits 0 unconditionally — verified on this
host. So the `asan` variant (part of the default `plain asan tsan` gate)
proves memory-safety (no use-after-free, no double-free, no OOB), but it
proves **nothing** about leak-freedom. Claiming "clean ASan run therefore
leak-clean" on macOS would be false.

**`/usr/bin/leaks` does work** as an at-exit gate on macOS:
`leaks --atExit -- <binary>` runs the binary as a child, then inspects its
heap at exit, printing `N leaks for M total leaked bytes` and exiting 1 on a
leaking binary, or `0 leaks for 0 total leaked bytes` and exiting 0 on a
clean one. Verified directly as part of this chunk against both:

- a genuinely leaking one-line probe binary (`new int[1000]` never freed):
  `leaks` reported `1 leak for 4096 total leaked bytes` and exited 1;
- the actual `rnrlottie_tests` binary (built plain/unsanitized): `leaks`
  reported `196 nodes malloced for 32 KB` / `0 leaks for 0 total leaked
  bytes` and exited 0.

`leaks` and ASan cannot be combined — ASan replaces the process's allocator,
which `leaks` inspects directly — so the leak run **must** use a non-ASan
("plain") build. `tests/run-tests.sh leaks` builds that plain binary
(reusing the existing `plain` fallback/CMake build path) and runs it under
`leaks --atExit` instead of executing it directly.

### How to run it

```bash
tests/run-tests.sh leaks          # ~15-20s on Darwin; opt-in, not part of
                                   # the default `plain asan tsan` gate
tests/run-tests.sh plain leaks    # combine with the normal (fast) suite
```

On a non-Darwin host, there is no `leaks` equivalent wired here. Rather than
silently reporting success, the variant falls back to ASan's LeakSanitizer
(`detect_leaks=1`) — which, unlike on macOS, genuinely works on Linux — and
prints which path it took. If `leaks` is expected (Darwin) but not found on
`PATH` (e.g. Xcode Command Line Tools not installed), the variant fails
loudly with an explicit error rather than reporting a false pass.

### Actual result at the time this chunk shipped

```
leaks Report Version: 4.0, multi-line stacks
Process <pid>: 196 nodes malloced for 32 KB
Process <pid>: 0 leaks for 0 total leaked bytes.
```

Exit code 0. **No suppressions were needed or used** — this is a genuine
zero-leak result for the whole `rnrlottie_tests` binary (the shared core,
the Android JNI-free adapter layer, and the vendored rlottie linked
statically into it), not a filtered one. If a future run surfaces a leak
inside vendored `rlottie` or a system library that cannot be fixed in this
stream (`cpp/third_party/` is out of this stream's ownership — see
`CLAUDE.md`), the right move is to report it here explicitly by name and
byte count, not to add a silent suppression list. None exist today because
none were found.

### A note on the `leaks --atExit` diagnostic banner

Each run prints a boilerplate banner: `Process <pid> is not debuggable. Due
to security restrictions, leaks can only show or save contents of readonly
memory of restricted processes.` This is macOS's standard notice for an
unsigned/unentitled dev binary — it limits `leaks`'s ability to symbolicate
detailed backtraces for any leaks it finds, but does **not** disable leak
*counting*: the leaking-probe check above (built and run the exact same way)
still correctly reported 1 leak / 4096 bytes / exit 1, so the "0 leaks" pass
above is a real negative, not a false one produced by this restriction.
