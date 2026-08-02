// Chunk 5.5 — libFuzzer target over the load/render path (plan §16/§20).
//
// This is a SEPARATE binary from rnrlottie_tests: libFuzzer supplies its own
// main() (linked in via -fsanitize=fuzzer), so this translation unit defines
// ONLY LLVMFuzzerTestOneInput and is never compiled into the CHECK/TEST binary
// (test_harness.cpp's main() would collide with libFuzzer's).
//
// Each iteration feeds arbitrary bytes to RlottiePlayerCore::loadFromData
// exactly as an attacker-controlled `source.json` would reach it (plan §16 —
// untrusted input, parsed by native C++), then — if it loaded — renders one
// frame into a small fixed-size buffer to also exercise renderFrame() against
// whatever metadata the input produced. No assertions beyond "does not
// crash/hang/leak": that property is what ASan/UBSan (linked in alongside
// -fsanitize=fuzzer, see tests/run-tests.sh's `fuzz` variant) and libFuzzer's
// own leak detector enforce.
//
// Deliberately tightens InputLimits once, up front: the default 16 MiB/100k
// frame budget would let the fuzzer spend most of its time inside a real
// rlottie parse of huge-but-otherwise-valid-shaped input, which is a good use
// of a long-running fuzz campaign but not of the bounded smoke run wired into
// tests/run-tests.sh. A single global setInputLimits() call is safe here
// (unlike in the CHECK-based tests) because this binary runs exactly one
// "test" — LLVMFuzzerTestOneInput — with no other test state to pollute.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "InputLimits.h"
#include "RlottiePlayerCore.h"

namespace {

bool g_limitsConfigured = false;

void ensureLimitsConfigured() {
    if (g_limitsConfigured) return;
    rnrlottie::InputLimits limits;
    limits.maxJsonBytes = 256u * 1024u;  // keep each iteration fast
    limits.maxFrames = 5000u;
    limits.maxPixels = 2048u * 2048u;
    rnrlottie::setInputLimits(limits);
    g_limitsConfigured = true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    ensureLimitsConfigured();

    std::string json(reinterpret_cast<const char*>(data), size);

    rnrlottie::RlottiePlayerCore core;
    auto result = core.loadFromData(std::move(json), "fuzz", "", /*useModelCache=*/false);
    if (result.success) {
        constexpr std::size_t kW = 8, kH = 8;
        std::vector<std::uint32_t> buf(kW * kH, 0);
        core.renderFrame(0, buf.data(), kW, kH, kW * 4, /*preserveAspectRatio=*/true);
        // Also probe a frame index far outside any plausible declared range —
        // must clamp (RlottiePlayerCore::renderFrame), never crash.
        core.renderFrame(SIZE_MAX / 2, buf.data(), kW, kH, kW * 4, true);
    }
    return 0;
}
